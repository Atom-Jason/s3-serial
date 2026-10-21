#include "network_service.h"

#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs.h"

static const char *TAG = "network";
static SemaphoreHandle_t lock;
static QueueHandle_t changes;
static esp_netif_t *ap_netif;
static char device_password[64], ap_ssid[33], sta_ssid[33], sta_password[64];
static char sta_ip[16], last_error[128];
static bool sta_enabled, connected, connecting;
static uint32_t configuration_generation;
static int64_t retry_at;

static void generate_password(void)
{
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    uint8_t bytes[24];
    esp_fill_random(bytes, sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); ++i) device_password[i] = alphabet[bytes[i] % (sizeof(alphabet) - 1)];
    device_password[sizeof(bytes)] = 0;
}

typedef struct {
    char ssid[33];
    char password[64];
    bool enabled;
    uint32_t expected_generation; /* Nonzero only for internal conflict actions. */
} network_change_t;

static esp_err_t store_password(const char *password)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("network", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_str(nvs, "device_pass", password);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t store_sta(const network_change_t *change)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("network", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_str(nvs, "sta_ssid", change->ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, "sta_pass", change->password);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "sta_enable", change->enabled);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static wifi_config_t ap_config(void)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.ap.ssid, ap_ssid, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen(ap_ssid);
    strlcpy((char *)cfg.ap.password, device_password, sizeof(cfg.ap.password));
    cfg.ap.channel = 6;
    cfg.ap.max_connection = 4;
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.pmf_cfg.capable = true;
    cfg.ap.pmf_cfg.required = false;
    return cfg;
}

static bool overlaps_ap(const esp_netif_ip_info_t *sta)
{
    esp_netif_ip_info_t ap;
    if (esp_netif_get_ip_info(ap_netif, &ap) != ESP_OK) return true;
    uint32_t a = ntohl(ap.ip.addr), am = ntohl(ap.netmask.addr);
    uint32_t s = ntohl(sta->ip.addr), sm = ntohl(sta->netmask.addr);
    return (a & am) <= ((s & sm) | ~sm) && (s & sm) <= ((a & am) | ~am);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        const bool conflict = overlaps_ap(&event->ip_info);
        network_change_t disabled = {0};
        xSemaphoreTake(lock, portMAX_DELAY);
        if (!sta_enabled || (!connecting && !connected)) {
            xSemaphoreGive(lock);
            return; /* Ignore an event queued by a superseded connection. */
        }
        connecting = false;
        if (conflict) {
            connected = false;
            sta_enabled = false;
            sta_ip[0] = 0;
            strlcpy(last_error, "STA subnet overlaps AP 192.168.4.0/24; STA disabled", sizeof(last_error));
            strlcpy(disabled.ssid, sta_ssid, sizeof(disabled.ssid));
            strlcpy(disabled.password, sta_password, sizeof(disabled.password));
            disabled.expected_generation = configuration_generation;
        } else if (sta_enabled) {
            connected = true;
            snprintf(sta_ip, sizeof(sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
            last_error[0] = 0;
            ESP_LOGI(TAG, "STA address: http://%s", sta_ip);
        }
        xSemaphoreGive(lock);
        if (conflict) {
            ESP_LOGW(TAG, "STA subnet overlaps AP; keeping AP access");
            /* The network task performs Wi-Fi/NVS changes outside this event loop. */
            if (xQueueSend(changes, &disabled, 0) != pdTRUE) {
                esp_wifi_disconnect();
            }
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        xSemaphoreTake(lock, portMAX_DELAY);
        connected = connecting = false;
        sta_ip[0] = 0;
        retry_at = esp_timer_get_time() + 5000000;
        if (sta_enabled) snprintf(last_error, sizeof(last_error), "STA disconnected (reason %u); retrying", event->reason);
        xSemaphoreGive(lock);
    }
}

static void network_task(void *arg)
{
    (void)arg;
    network_change_t change;
    for (;;) {
        if (xQueueReceive(changes, &change, pdMS_TO_TICKS(250)) == pdTRUE) {
            xSemaphoreTake(lock, portMAX_DELAY);
            if (change.expected_generation && change.expected_generation != configuration_generation) {
                xSemaphoreGive(lock);
                continue;
            }
            ++configuration_generation;
            sta_enabled = false;
            connected = connecting = false;
            sta_ip[0] = 0;
            xSemaphoreGive(lock);
            esp_wifi_disconnect();
            esp_err_t err = store_sta(&change);
            if (err == ESP_OK) err = esp_wifi_set_mode(change.enabled ? WIFI_MODE_APSTA : WIFI_MODE_AP);
            if (err == ESP_OK && change.enabled) {
                wifi_config_t cfg = {0};
                memcpy(cfg.sta.ssid, change.ssid, strlen(change.ssid));
                strlcpy((char *)cfg.sta.password, change.password, sizeof(cfg.sta.password));
                cfg.sta.threshold.authmode = change.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
                cfg.sta.pmf_cfg.capable = true;
                err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
            }
            xSemaphoreTake(lock, portMAX_DELAY);
            strlcpy(sta_ssid, change.ssid, sizeof(sta_ssid));
            strlcpy(sta_password, change.password, sizeof(sta_password));
            sta_enabled = change.enabled && err == ESP_OK;
            retry_at = esp_timer_get_time() + 500000;
            if (err != ESP_OK) snprintf(last_error, sizeof(last_error), "Network configuration failed: %s", esp_err_to_name(err));
            else if (change.enabled) strlcpy(last_error, "Connecting to router", sizeof(last_error));
            else if (!strstr(last_error, "overlaps")) last_error[0] = 0;
            xSemaphoreGive(lock);
        }
        const int64_t now = esp_timer_get_time();
        xSemaphoreTake(lock, portMAX_DELAY);
        bool connect_now = sta_enabled && !connected && !connecting && now >= retry_at;
        if (connect_now) connecting = true;
        xSemaphoreGive(lock);
        if (connect_now) {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                xSemaphoreTake(lock, portMAX_DELAY);
                connecting = false;
                retry_at = now + 5000000;
                snprintf(last_error, sizeof(last_error), "Connect failed: %s", esp_err_to_name(err));
                xSemaphoreGive(lock);
            }
        }
    }
}

esp_err_t network_service_init(void)
{
    lock = xSemaphoreCreateMutex();
    changes = xQueueCreate(2, sizeof(network_change_t));
    if (!lock || !changes) return ESP_ERR_NO_MEM;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("network", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    size_t size = sizeof(device_password);
    if (nvs_get_str(nvs, "device_pass", device_password, &size) != ESP_OK) device_password[0] = 0;
    network_change_t initial = {0};
    size = sizeof(initial.ssid);
    nvs_get_str(nvs, "sta_ssid", initial.ssid, &size);
    size = sizeof(initial.password);
    nvs_get_str(nvs, "sta_pass", initial.password, &size);
    uint8_t enabled = 0;
    nvs_get_u8(nvs, "sta_enable", &enabled);
    initial.enabled = enabled && initial.ssid[0];
    nvs_close(nvs);

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif || !esp_netif_create_default_wifi_sta()) return ESP_ERR_NO_MEM;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "Wi-Fi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "AP mode");
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), TAG, "MAC");
    snprintf(ap_ssid, sizeof(ap_ssid), "S3-Serial-%02X%02X", mac[4], mac[5]);
    const bool new_password = !device_password[0];
    /* Temporary unadvertised credential lets us start RF before generating the
     * persistent credential with continuously replenished hardware entropy. */
    if (new_password) generate_password();
    wifi_config_t ap = ap_config();
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "AP config");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL), TAG, "Wi-Fi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL), TAG, "IP handler");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start");
    if (new_password) {
        generate_password();
        ESP_RETURN_ON_ERROR(store_password(device_password), TAG, "save initial password");
        ap = ap_config();
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "initial password");
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "Hotspot: %s", ap_ssid);
    ESP_LOGI(TAG, "Device / Wi-Fi password: %s", device_password);
    ESP_LOGI(TAG, "Open http://192.168.4.1  UART1 TX=GPIO5 RX=GPIO6");
    if (xTaskCreate(network_task, "network", 4096, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    if (initial.enabled) xQueueSend(changes, &initial, 0);
    return ESP_OK;
}

bool network_service_password_matches(const char *candidate)
{
    if (!candidate || strlen(candidate) > 63) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    const size_t n = strlen(device_password), m = strlen(candidate);
    unsigned diff = n ^ m;
    for (size_t i = 0; i < n; ++i) diff |= (unsigned char)device_password[i] ^ (i < m ? (unsigned char)candidate[i] : 0);
    xSemaphoreGive(lock);
    return diff == 0;
}

cJSON *network_service_status(void)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    xSemaphoreTake(lock, portMAX_DELAY);
    cJSON_AddStringToObject(obj, "ssid", sta_ssid);
    cJSON_AddBoolToObject(obj, "sta_enabled", sta_enabled);
    cJSON_AddBoolToObject(obj, "connected", connected);
    cJSON_AddStringToObject(obj, "sta_ip", sta_ip);
    cJSON_AddStringToObject(obj, "ap_ssid", ap_ssid);
    cJSON_AddStringToObject(obj, "ap_ip", "192.168.4.1");
    cJSON_AddStringToObject(obj, "error", last_error);
    xSemaphoreGive(lock);
    return obj;
}

const char *network_service_configure(const cJSON *payload)
{
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(payload, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(payload, "password");
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(payload, "enabled");
    if (!cJSON_IsBool(enabled) || !cJSON_IsString(ssid) || !cJSON_IsString(pass)) return "invalid_parameter";
    size_t n = strlen(ssid->valuestring), p = strlen(pass->valuestring);
    if (n > 32 || p > 63 || (cJSON_IsTrue(enabled) && !n) || (p && p < 8)) return "invalid_parameter";
    network_change_t change = {.enabled = cJSON_IsTrue(enabled)};
    strlcpy(change.ssid, ssid->valuestring, sizeof(change.ssid));
    strlcpy(change.password, pass->valuestring, sizeof(change.password));
    return xQueueSend(changes, &change, 0) == pdTRUE ? NULL : "queue_full";
}

esp_err_t network_service_change_password(const char *password)
{
    if (!password) return ESP_ERR_INVALID_ARG;
    size_t n = strlen(password);
    if (n < 12 || n > 63) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < n; ++i) if ((unsigned char)password[i] < 33 || (unsigned char)password[i] > 126) return ESP_ERR_INVALID_ARG;
    esp_err_t err = store_password(password);
    if (err != ESP_OK) return err;
    xSemaphoreTake(lock, portMAX_DELAY);
    strlcpy(device_password, password, sizeof(device_password));
    wifi_config_t cfg = ap_config();
    xSemaphoreGive(lock);
    /* Persistence is the commit point: all web sessions must be revoked once
     * it succeeds, even if the AP driver cannot apply the new key immediately. */
    err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err != ESP_OK) {
        xSemaphoreTake(lock, portMAX_DELAY);
        snprintf(last_error, sizeof(last_error), "Device password saved; AP apply failed (%s), restart device", esp_err_to_name(err));
        xSemaphoreGive(lock);
        ESP_LOGE(TAG, "Password saved; restart to apply AP key: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}
