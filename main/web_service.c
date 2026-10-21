#include "web_service.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network_service.h"
#include "serial_service.h"
#include "web_assets.h"

#define MAX_PEERS 3
#define MAX_SESSIONS 8
#define MESSAGE_LIMIT 4096
#define OUTPUT_LIMIT 8192
#define HEARTBEAT_US 15000000LL
#define SESSION_US (24LL * 60 * 60 * 1000000)

typedef struct {
    char token[65];
    int64_t touched;
    bool valid;
} session_t;

typedef struct {
    int fd;
    uint32_t id;
    int session;
    bool closing, fragmenting;
    size_t incoming_len;
    char incoming[MESSAGE_LIMIT + 1];
    uint32_t cursor;
    int64_t heartbeat, last_status;
} peer_t;

typedef struct {
    uint32_t owner;
    char type[32], id[41], payload[512];
} serial_event_t;

static const char *TAG = "web";
static httpd_handle_t server;
static session_t sessions[MAX_SESSIONS];
static peer_t peers[MAX_PEERS];
static uint32_t controller, next_peer_id = 1;
static char boot_id[17];
static QueueHandle_t events;
static SemaphoreHandle_t lease_lock;
static uint32_t lease_owner, expired_owner;
static int64_t lease_deadline;
static portMUX_TYPE scheduling_lock = portMUX_INITIALIZER_UNLOCKED;
static bool tick_pending, control_dirty;
static uint32_t dropped_events;
static int64_t login_after, send_deadline, receive_deadline;

static void close_peer(peer_t *peer);

static void random_hex(char *dest, size_t bytes)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < bytes; ++i) {
        uint8_t b;
        esp_fill_random(&b, 1);
        dest[2 * i] = hex[b >> 4];
        dest[2 * i + 1] = hex[b & 15];
    }
    dest[2 * bytes] = 0;
}

static peer_t *peer_by_fd(int fd)
{
    for (size_t i = 0; i < MAX_PEERS; ++i) if (peers[i].fd == fd) return &peers[i];
    return NULL;
}

static void release_control(peer_t *peer)
{
    xSemaphoreTake(lease_lock, portMAX_DELAY);
    if (peer && peer->id && controller == peer->id) {
        controller = 0;
        lease_owner = expired_owner = 0;
        serial_service_set_owner(0);
        control_dirty = true;
    }
    xSemaphoreGive(lease_lock);
}

/* Only the HTTP task touches peers/controller. The independent timer task can
 * revoke serial ownership even if a slow HTTP client blocks that task. */
static void sync_expired_lease(void)
{
    xSemaphoreTake(lease_lock, portMAX_DELAY);
    uint32_t expired = expired_owner;
    expired_owner = 0;
    if (expired && controller == expired) {
        controller = 0;
        control_dirty = true;
    }
    xSemaphoreGive(lease_lock);
    if (expired) for (size_t i = 0; i < MAX_PEERS; ++i) if (peers[i].id == expired) close_peer(&peers[i]);
}

static void close_peer(peer_t *peer)
{
    if (!peer || peer->fd < 0 || peer->closing) return;
    peer->closing = true;
    release_control(peer);
    httpd_sess_trigger_close(server, peer->fd);
}

static void close_session(httpd_handle_t handle, int fd)
{
    (void)handle;
    peer_t *peer = peer_by_fd(fd);
    if (peer) {
        release_control(peer);
        memset(peer, 0, sizeof(*peer));
        peer->fd = -1;
    }
    close(fd);
}

/* ESP-IDF's WS helper only checks negative writes, not partial writes.
 * This override sends all bytes by a shared frame deadline or returns error;
 * any partial frame error closes the socket rather than corrupting the stream. */
static int bounded_send(httpd_handle_t handle, int fd, const char *data, size_t length, int flags)
{
    (void)handle;
    const int64_t deadline = send_deadline ? send_deadline : esp_timer_get_time() + 20000;
    size_t offset = 0;
    while (offset < length) {
        if (esp_timer_get_time() >= deadline) return HTTPD_SOCK_ERR_TIMEOUT;
        int count = send(fd, data + offset, length - offset, flags | MSG_DONTWAIT);
        if (count > 0) { offset += count; continue; }
        if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) return HTTPD_SOCK_ERR_FAIL;
        int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) return HTTPD_SOCK_ERR_TIMEOUT;
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(fd, &writable);
        struct timeval timeout = {.tv_sec = 0, .tv_usec = (long)remaining};
        int result = select(fd + 1, NULL, &writable, NULL, &timeout);
        if (result <= 0 && errno != EINTR) return HTTPD_SOCK_ERR_TIMEOUT;
    }
    return (int)offset;
}

static int bounded_recv(httpd_handle_t handle, int fd, char *data, size_t length, int flags)
{
    (void)handle;
    const int64_t deadline = receive_deadline ? receive_deadline : esp_timer_get_time() + 1000000;
    for (;;) {
        if (esp_timer_get_time() >= deadline) return HTTPD_SOCK_ERR_TIMEOUT;
        int count = recv(fd, data, length, flags | MSG_DONTWAIT);
        if (count >= 0) return count;
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return HTTPD_SOCK_ERR_FAIL;
        int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) return HTTPD_SOCK_ERR_TIMEOUT;
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        struct timeval timeout = {.tv_sec = remaining / 1000000, .tv_usec = remaining % 1000000};
        int result = select(fd + 1, &readable, NULL, NULL, &timeout);
        if (result <= 0 && errno != EINTR) return HTTPD_SOCK_ERR_TIMEOUT;
    }
}

static esp_err_t ws_frame(peer_t *peer, httpd_ws_type_t type, const void *data, size_t length)
{
    if (!peer || peer->closing || length > OUTPUT_LIMIT) return ESP_ERR_INVALID_STATE;
    httpd_ws_frame_t frame = {.type = type, .payload = (uint8_t *)data, .len = length};
    send_deadline = esp_timer_get_time() + 20000;
    esp_err_t result = httpd_ws_send_frame_async(server, peer->fd, &frame);
    send_deadline = 0;
    if (result != ESP_OK) close_peer(peer);
    return result;
}

static esp_err_t ws_json(peer_t *peer, const char *type, const char *id, const cJSON *payload)
{
    cJSON *message = cJSON_CreateObject();
    if (!message) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(message, "v", 1);
    cJSON_AddStringToObject(message, "type", type);
    if (id && *id) cJSON_AddStringToObject(message, "id", id);
    cJSON *copy = payload ? cJSON_Duplicate(payload, true) : cJSON_CreateObject();
    if (!copy) { cJSON_Delete(message); return ESP_ERR_NO_MEM; }
    cJSON_AddItemToObject(message, "payload", copy);
    char *text = cJSON_PrintUnformatted(message);
    cJSON_Delete(message);
    if (!text) return ESP_ERR_NO_MEM;
    esp_err_t err = ws_frame(peer, HTTPD_WS_TYPE_TEXT, text, strlen(text));
    free(text);
    return err;
}

static void ws_error(peer_t *peer, const char *id, const char *code)
{
    cJSON *body = cJSON_CreateObject();
    if (!body) return;
    cJSON_AddStringToObject(body, "code", code);
    cJSON_AddStringToObject(body, "message", code);
    ws_json(peer, "error", id, body);
    cJSON_Delete(body);
}

static bool origin_ok(httpd_req_t *request, bool required)
{
    size_t length = httpd_req_get_hdr_value_len(request, "Origin");
    if (!length) return !required;
    if (length >= 192) return false;
    char origin[192], host[128], expected[144];
    if (httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)) != ESP_OK ||
        httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK) return false;
    snprintf(expected, sizeof(expected), "http://%s", host);
    return strcasecmp(origin, expected) == 0;
}

static int find_session(httpd_req_t *request)
{
    char cookie[512];
    if (httpd_req_get_hdr_value_str(request, "Cookie", cookie, sizeof(cookie)) != ESP_OK) return -1;
    char *save = NULL;
    char token[65] = {0};
    for (char *part = strtok_r(cookie, ";", &save); part; part = strtok_r(NULL, ";", &save)) {
        while (*part == ' ') ++part;
        if (!strncmp(part, "s3sid=", 6) && strlen(part + 6) == 64) {
            memcpy(token, part + 6, 64);
            break;
        }
    }
    if (!token[0]) return -1;
    int64_t now = esp_timer_get_time();
    for (size_t i = 0; i < MAX_SESSIONS; ++i) {
        if (sessions[i].valid && !strcmp(sessions[i].token, token) && now - sessions[i].touched < SESSION_US) {
            sessions[i].touched = now;
            return (int)i;
        }
    }
    return -1;
}

static bool session_has_peer(int index)
{
    for (size_t i = 0; i < MAX_PEERS; ++i) if (peers[i].fd >= 0 && peers[i].session == index) return true;
    return false;
}

static void revoke_session(int index)
{
    if (index < 0 || index >= MAX_SESSIONS) return;
    sessions[index].valid = false;
    for (size_t i = 0; i < MAX_PEERS; ++i) if (peers[i].fd >= 0 && peers[i].session == index) close_peer(&peers[i]);
}

static int create_session(void)
{
    int choice = -1;
    for (size_t i = 0; i < MAX_SESSIONS; ++i) {
        if (!sessions[i].valid) { choice = i; break; }
        if (!session_has_peer(i) && (choice < 0 || sessions[i].touched < sessions[choice].touched)) choice = i;
    }
    if (choice < 0) return -1;
    random_hex(sessions[choice].token, 32);
    sessions[choice].valid = true;
    sessions[choice].touched = esp_timer_get_time();
    return choice;
}

static cJSON *status_json(void)
{
    cJSON *status = cJSON_CreateObject();
    if (!status) return NULL;
    cJSON_AddStringToObject(status, "device", "S3 Serial / UART1 GPIO5 GPIO6");
    cJSON_AddStringToObject(status, "firmware", esp_app_get_description()->version);
    cJSON_AddStringToObject(status, "boot_id", boot_id);
    cJSON_AddNumberToObject(status, "uptime_ms", esp_timer_get_time() / 1000);
    cJSON_AddNumberToObject(status, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(status, "minimum_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddItemToObject(status, "network", network_service_status());
    cJSON_AddItemToObject(status, "serial", serial_service_status());
    cJSON_AddNumberToObject(status, "controller_id", controller);
    int count = 0;
    for (size_t i = 0; i < MAX_PEERS; ++i) count += peers[i].fd >= 0 && !peers[i].closing;
    cJSON_AddNumberToObject(status, "client_count", count);
    cJSON_AddNumberToObject(status, "control_events_dropped", dropped_events);
    return status;
}

static void security_headers(httpd_req_t *request)
{
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self' ws:; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
}

static esp_err_t json_response(httpd_req_t *request, cJSON *body)
{
    security_headers(request);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    char *text = body ? cJSON_PrintUnformatted(body) : NULL;
    cJSON_Delete(body);
    if (!text) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    esp_err_t result = httpd_resp_send(request, text, HTTPD_RESP_USE_STRLEN);
    free(text);
    return result;
}

static esp_err_t http_error(httpd_req_t *request, const char *status, const char *code)
{
    httpd_resp_set_status(request, status);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "error", code);
    cJSON_AddStringToObject(body, "message", code);
    return json_response(request, body);
}

/* Bound parser nesting independently of the SDK cJSON build configuration. */
static bool shallow_json(const char *text, size_t length)
{
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = text[i];
        if (!ch) return false;
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else {
            if (ch == '"') quoted = true;
            else if (ch == '{' || ch == '[') { if (++depth > 12) return false; }
            else if (ch == '}' || ch == ']') { if (!depth) return false; --depth; }
        }
    }
    return !quoted && !depth;
}

static cJSON *parse_json(const char *text, size_t length)
{
    if (!shallow_json(text, length)) return NULL;
    return cJSON_ParseWithLengthOpts(text, length + 1, NULL, true);
}

static cJSON *read_json(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len > MESSAGE_LIMIT) return NULL;
    char *text = malloc(request->content_len + 1);
    if (!text) return NULL;
    httpd_sess_set_recv_override(server, httpd_req_to_sockfd(request), bounded_recv);
    receive_deadline = esp_timer_get_time() + 1000000;
    size_t offset = 0;
    while (offset < request->content_len) {
        int count = httpd_req_recv(request, text + offset, request->content_len - offset);
        if (count <= 0) { receive_deadline = 0; free(text); return NULL; }
        offset += count;
    }
    text[offset] = 0;
    receive_deadline = 0;
    cJSON *body = parse_json(text, offset);
    free(text);
    if (body && !cJSON_IsObject(body)) { cJSON_Delete(body); return NULL; }
    return body;
}

static bool caller_controls(int session, const cJSON *body)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(body, "client_id");
    if (!cJSON_IsNumber(id) || id->valuedouble != controller || !controller) return false;
    for (size_t i = 0; i < MAX_PEERS; ++i) {
        if (peers[i].id == controller && peers[i].fd >= 0 && !peers[i].closing && peers[i].session == session) return true;
    }
    return false;
}

static esp_err_t api_handler(httpd_req_t *request)
{
    sync_expired_lease();
    if (request->content_len > MESSAGE_LIMIT) {
        http_error(request, "413 Payload Too Large", "message_too_large");
        return ESP_FAIL; /* Do not reuse a connection with an unread body. */
    }
    bool login = !strcmp(request->uri, "/api/v1/session") && request->method == HTTP_POST;
    if (!origin_ok(request, false)) return http_error(request, "403 Forbidden", "origin_rejected");
    int session = find_session(request);
    if (login) {
        if (esp_timer_get_time() < login_after) return http_error(request, "429 Too Many Requests", "login_rate_limited");
        login_after = esp_timer_get_time() + 500000;
        cJSON *body = read_json(request);
        if (!body) {
            http_error(request, "400 Bad Request", "invalid_json");
            return ESP_FAIL;
        }
        const cJSON *password = cJSON_GetObjectItemCaseSensitive(body, "password");
        bool valid = cJSON_IsString(password) && network_service_password_matches(password->valuestring);
        cJSON_Delete(body);
        if (!valid) return http_error(request, "401 Unauthorized", "invalid_password");
        if (session < 0) session = create_session();
        if (session < 0) return http_error(request, "503 Service Unavailable", "session_limit");
        char cookie[128];
        snprintf(cookie, sizeof(cookie), "s3sid=%s; HttpOnly; SameSite=Strict; Path=/", sessions[session].token);
        httpd_resp_set_hdr(request, "Set-Cookie", cookie);
        cJSON *response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "ok", true);
        return json_response(request, response);
    }
    if (session < 0) return http_error(request, "401 Unauthorized", "login_required");
    if (!strcmp(request->uri, "/api/v1/session") && request->method == HTTP_DELETE) {
        revoke_session(session);
        httpd_resp_set_hdr(request, "Set-Cookie", "s3sid=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
        cJSON *response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "ok", true);
        return json_response(request, response);
    }
    if (!strcmp(request->uri, "/api/v1/status") && request->method == HTTP_GET) return json_response(request, status_json());
    if (!strcmp(request->uri, "/api/v1/network") && request->method == HTTP_GET) return json_response(request, network_service_status());
    if (request->method != HTTP_PUT) return http_error(request, "404 Not Found", "not_found");
    cJSON *body = read_json(request);
    if (!body) {
        http_error(request, "400 Bad Request", "invalid_json");
        return ESP_FAIL;
    }
    if (!caller_controls(session, body)) {
        cJSON_Delete(body);
        return http_error(request, "403 Forbidden", "not_controller");
    }
    const char *error = NULL;
    bool password_changed = false;
    if (!strcmp(request->uri, "/api/v1/network")) error = network_service_configure(body);
    else if (!strcmp(request->uri, "/api/v1/password")) {
        const cJSON *password = cJSON_GetObjectItemCaseSensitive(body, "password");
        esp_err_t err = cJSON_IsString(password) ? network_service_change_password(password->valuestring) : ESP_ERR_INVALID_ARG;
        if (err != ESP_OK) error = err == ESP_ERR_INVALID_ARG ? "invalid_password" : "password_save_failed";
        else password_changed = true;
    } else error = "not_found";
    cJSON_Delete(body);
    if (error) return http_error(request, "400 Bad Request", error);
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "ok", true);
    if (password_changed) {
        cJSON_AddBoolToObject(response, "reconnect_required", true);
        cJSON_AddItemToObject(response, "network", network_service_status());
    }
    esp_err_t result = json_response(request, response);
    if (password_changed) for (size_t i = 0; i < MAX_SESSIONS; ++i) revoke_session(i);
    return result;
}

static esp_err_t static_handler(httpd_req_t *request)
{
    const web_asset_t *asset = web_asset_find(request->uri);
    if (!asset) return http_error(request, "404 Not Found", "not_found");
    security_headers(request);
    httpd_resp_set_type(request, asset->content_type);
    httpd_resp_set_hdr(request, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(request, "Vary", "Accept-Encoding");
    httpd_resp_set_hdr(request, "ETag", asset->etag);
    httpd_resp_set_hdr(request, "Cache-Control", "no-cache");
    return httpd_resp_send(request, (const char *)asset->data, asset->size);
}

static bool safe_id(const cJSON *id)
{
    if (!cJSON_IsString(id)) return false;
    size_t n = strlen(id->valuestring);
    if (!n || n > 40) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = id->valuestring[i];
        if (!(isalnum(c) || c == '-' || c == '_' || c == '.' || c == ':')) return false;
    }
    return true;
}

static void process_command(peer_t *peer, const char *text, size_t length)
{
    sync_expired_lease();
    if (peer->closing) return;
    cJSON *message = parse_json(text, length);
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(message, "v");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(message, "type");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(message, "id");
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(message, "payload");
    if (!cJSON_IsObject(message) || !cJSON_IsNumber(version) || version->valuedouble != 1 ||
        !cJSON_IsString(type) || strlen(type->valuestring) > 32 || !safe_id(id) || !cJSON_IsObject(payload)) {
        ws_error(peer, NULL, "invalid_message");
        cJSON_Delete(message);
        return;
    }
    const char *error = NULL;
    cJSON *reply = cJSON_CreateObject();
    if (!reply) { cJSON_Delete(message); return; }
    cJSON_AddBoolToObject(reply, "ok", true);
    if (!strcmp(type->valuestring, "heartbeat")) {
        peer->heartbeat = esp_timer_get_time();
        sessions[peer->session].touched = peer->heartbeat;
        xSemaphoreTake(lease_lock, portMAX_DELAY);
        if (lease_owner == peer->id) lease_deadline = peer->heartbeat + HEARTBEAT_US;
        xSemaphoreGive(lease_lock);
    } else if (!strcmp(type->valuestring, "control.claim")) {
        xSemaphoreTake(lease_lock, portMAX_DELAY);
        if (controller && controller != peer->id) error = "controller_busy";
        else if (!controller) {
            controller = peer->id;
            lease_owner = controller;
            lease_deadline = peer->heartbeat + HEARTBEAT_US;
            serial_service_set_owner(controller);
            control_dirty = true;
        }
        xSemaphoreGive(lease_lock);
        cJSON_AddNumberToObject(reply, "controller_id", controller);
    } else if (!strcmp(type->valuestring, "control.release")) {
        if (controller != peer->id) error = "not_controller";
        else release_control(peer);
    } else if (!strcmp(type->valuestring, "history.get")) {
        const cJSON *after = cJSON_GetObjectItemCaseSensitive(payload, "after_seq");
        if (!cJSON_IsNumber(after) || after->valuedouble < 0 || after->valuedouble > UINT32_MAX || floor(after->valuedouble) != after->valuedouble) error = "invalid_parameter";
        else peer->cursor = (uint32_t)after->valuedouble;
    } else if (controller != peer->id) error = "not_controller";
    else error = serial_service_command(peer->id, id->valuestring, type->valuestring, payload, reply);
    if (error) ws_error(peer, id->valuestring, error);
    else ws_json(peer, "ack", id->valuestring, reply);
    cJSON_Delete(reply);
    cJSON_Delete(message);
}

static esp_err_t ws_read_message(httpd_req_t *request, peer_t *peer)
{
    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(request, &frame, 0);
    if (err != ESP_OK) return err;
    if (frame.type == HTTPD_WS_TYPE_CLOSE || frame.type == HTTPD_WS_TYPE_PING || frame.type == HTTPD_WS_TYPE_PONG) {
        uint8_t control_payload[126];
        if (frame.len > 125 || !frame.final) return ESP_FAIL;
        frame.payload = control_payload;
        if (frame.len && httpd_ws_recv_frame(request, &frame, sizeof(control_payload)) != ESP_OK) return ESP_FAIL;
        if (frame.type == HTTPD_WS_TYPE_CLOSE) {
            release_control(peer);
            return ESP_FAIL;
        }
        if (frame.type == HTTPD_WS_TYPE_PING) return ws_frame(peer, HTTPD_WS_TYPE_PONG, control_payload, frame.len);
        return ESP_OK;
    }
    if (frame.type == HTTPD_WS_TYPE_TEXT) {
        if (peer->fragmenting) return ESP_FAIL;
        peer->incoming_len = 0;
    } else if (frame.type != HTTPD_WS_TYPE_CONTINUE || !peer->fragmenting) return ESP_FAIL;
    if (frame.len > MESSAGE_LIMIT - peer->incoming_len) return ESP_FAIL;
    frame.payload = (uint8_t *)peer->incoming + peer->incoming_len;
    if (frame.len && httpd_ws_recv_frame(request, &frame, MESSAGE_LIMIT - peer->incoming_len) != ESP_OK) return ESP_FAIL;
    peer->incoming_len += frame.len;
    peer->incoming[peer->incoming_len] = 0;
    peer->fragmenting = !frame.final;
    if (frame.final) {
        process_command(peer, peer->incoming, peer->incoming_len);
        peer->incoming_len = 0;
    }
    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *request)
{
    int fd = httpd_req_to_sockfd(request);
    if (request->method == HTTP_GET) {
        /* IDF 5.5 invokes this handler immediately after its upgrade handshake.
         * Do not allocate a peer or send any device state until auth succeeds. */
        int session = find_session(request);
        if (session < 0 || !origin_ok(request, true)) return ESP_FAIL;
        peer_t *peer = NULL;
        for (size_t i = 0; i < MAX_PEERS; ++i) if (peers[i].fd < 0) { peer = &peers[i]; break; }
        if (!peer) return ESP_FAIL;
        memset(peer, 0, sizeof(*peer));
        peer->fd = fd;
        peer->id = next_peer_id++;
        if (!peer->id) peer->id = next_peer_id++;
        peer->session = session;
        peer->heartbeat = esp_timer_get_time();
        peer->cursor = serial_service_latest_seq();
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 200000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        httpd_sess_set_send_override(server, fd, bounded_send);
        httpd_sess_set_recv_override(server, fd, bounded_recv);
        cJSON *hello = cJSON_CreateObject();
        if (!hello) return ESP_ERR_NO_MEM;
        cJSON_AddNumberToObject(hello, "client_id", peer->id);
        cJSON_AddStringToObject(hello, "boot_id", boot_id);
        cJSON_AddNumberToObject(hello, "controller_id", controller);
        cJSON_AddNumberToObject(hello, "latest_seq", peer->cursor);
        cJSON_AddItemToObject(hello, "serial", serial_service_status());
        esp_err_t err = ws_json(peer, "hello", NULL, hello);
        cJSON_Delete(hello);
        return err;
    }
    peer_t *peer = peer_by_fd(fd);
    if (!peer || peer->closing || !sessions[peer->session].valid) return ESP_FAIL;
    receive_deadline = esp_timer_get_time() + 500000;
    esp_err_t err = ws_read_message(request, peer);
    receive_deadline = 0;
    return err;
}

void web_service_serial_event(uint32_t owner, const char *type, const char *request_id, const cJSON *payload)
{
    if (!events) return;
    serial_event_t event = {.owner = owner};
    strlcpy(event.type, type ? type : "event", sizeof(event.type));
    strlcpy(event.id, request_id ? request_id : "", sizeof(event.id));
    bool fit = !payload || cJSON_PrintPreallocated((cJSON *)payload, event.payload, sizeof(event.payload), false);
    if (!fit || xQueueSend(events, &event, 0) != pdTRUE) {
        portENTER_CRITICAL(&scheduling_lock);
        ++dropped_events;
        portEXIT_CRITICAL(&scheduling_lock);
    }
}

static void push_tick(void *arg)
{
    (void)arg;
    sync_expired_lease();
    const int64_t now = esp_timer_get_time();
    for (size_t i = 0; i < MAX_PEERS; ++i) {
        peer_t *peer = &peers[i];
        if (peer->fd < 0 || peer->closing) continue;
        if (!sessions[peer->session].valid || now - peer->heartbeat > HEARTBEAT_US) close_peer(peer);
    }
    if (control_dirty) {
        control_dirty = false;
        cJSON *state = cJSON_CreateObject();
        cJSON_AddNumberToObject(state, "controller_id", controller);
        for (size_t i = 0; i < MAX_PEERS; ++i) if (peers[i].fd >= 0 && !peers[i].closing) ws_json(&peers[i], "control.changed", NULL, state);
        cJSON_Delete(state);
    }
    serial_event_t event;
    for (size_t count = 0; count < 16 && xQueueReceive(events, &event, 0) == pdTRUE; ++count) {
        cJSON *payload = event.payload[0] ? cJSON_Parse(event.payload) : cJSON_CreateObject();
        for (size_t i = 0; i < MAX_PEERS; ++i) {
            peer_t *peer = &peers[i];
            if (peer->fd >= 0 && !peer->closing && (!event.owner || event.owner == peer->id)) ws_json(peer, event.type, event.id, payload);
        }
        cJSON_Delete(payload);
    }
    for (size_t i = 0; i < MAX_PEERS; ++i) {
        peer_t *peer = &peers[i];
        if (peer->fd < 0 || peer->closing) continue;
        if (serial_service_latest_seq() != peer->cursor) {
            cJSON *history = serial_service_history(peer->cursor, 4);
            if (history) {
                cJSON_AddStringToObject(history, "boot_id", boot_id);
                const cJSON *records = cJSON_GetObjectItemCaseSensitive(history, "records");
                const cJSON *last = cJSON_GetArrayItem(records, cJSON_GetArraySize(records) - 1);
                const cJSON *seq = cJSON_GetObjectItemCaseSensitive(last, "seq");
                if (cJSON_IsNumber(seq) && ws_json(peer, "history", NULL, history) == ESP_OK) peer->cursor = (uint32_t)seq->valuedouble;
                cJSON_Delete(history);
            }
        }
        if (now - peer->last_status >= 1000000) {
            cJSON *state = status_json();
            if (state) { ws_json(peer, "status", NULL, state); cJSON_Delete(state); }
            peer->last_status = now;
        }
    }
    for (size_t i = 0; i < MAX_SESSIONS; ++i) if (sessions[i].valid && now - sessions[i].touched >= SESSION_US) revoke_session(i);
    portENTER_CRITICAL(&scheduling_lock);
    tick_pending = false;
    portEXIT_CRITICAL(&scheduling_lock);
}

static void tick_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(lease_lock, portMAX_DELAY);
        if (lease_owner && esp_timer_get_time() >= lease_deadline) {
            expired_owner = lease_owner;
            lease_owner = 0;
            serial_service_set_owner(0);
        }
        xSemaphoreGive(lease_lock);
        bool schedule;
        portENTER_CRITICAL(&scheduling_lock);
        schedule = !tick_pending;
        if (schedule) tick_pending = true;
        portEXIT_CRITICAL(&scheduling_lock);
        if (schedule && httpd_queue_work(server, push_tick, NULL) != ESP_OK) {
            portENTER_CRITICAL(&scheduling_lock);
            tick_pending = false;
            portEXIT_CRITICAL(&scheduling_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t web_service_init(void)
{
    for (size_t i = 0; i < MAX_PEERS; ++i) peers[i].fd = -1;
    random_hex(boot_id, 8);
    lease_lock = xSemaphoreCreateMutex();
    events = xQueueCreate(16, sizeof(serial_event_t));
    if (!events || !lease_lock) return ESP_ERR_NO_MEM;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 10240;
    config.max_uri_handlers = 12;
    config.max_open_sockets = 10;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.close_fn = close_session;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) return err;
    const httpd_uri_t routes[] = {
        {.uri = "/api/v1/session", .method = HTTP_POST, .handler = api_handler},
        {.uri = "/api/v1/session", .method = HTTP_DELETE, .handler = api_handler},
        {.uri = "/api/v1/status", .method = HTTP_GET, .handler = api_handler},
        {.uri = "/api/v1/network", .method = HTTP_GET, .handler = api_handler},
        {.uri = "/api/v1/network", .method = HTTP_PUT, .handler = api_handler},
        {.uri = "/api/v1/password", .method = HTTP_PUT, .handler = api_handler},
        {.uri = "/api/v1/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true, .handle_ws_control_frames = true},
        {.uri = "/*", .method = HTTP_GET, .handler = static_handler}
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) return err;
    }
    if (xTaskCreate(tick_task, "web_tick", 3072, NULL, 3, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "HTTP/WebSocket ready; maximum 3 pages, one controller");
    return ESP_OK;
}
