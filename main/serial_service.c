#include "serial_service.h"
#include "serial_core.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define PORT UART_NUM_1
#define TX_PIN 5
#define RX_PIN 6
#define JOB_COUNT 4u
#define RX_PAUSE_REQUEST BIT0
#define RX_PAUSED BIT1

typedef enum { JOB_TX, JOB_CONFIG, JOB_SAVE } job_type_t;
typedef struct {
    job_type_t type;
    uint32_t owner;
    uint32_t epoch;
    uint32_t revision;
    uint32_t repeat_generation;
    bool periodic;
    char id[SERIAL_REQUEST_ID_MAX + 1];
    serial_config_t config;
    size_t length;
    uint8_t bytes[SERIAL_MAX_BYTES];
} serial_job_t;

typedef struct {
    bool active;
    uint32_t interval_ms;
    uint64_t skipped;
    uint32_t generation;
    int64_t next_us;
    serial_job_t snapshot;
} repeat_state_t;

static struct {
    SemaphoreHandle_t lock;
    EventGroupHandle_t rx_sync;
    QueueHandle_t uart_events;
    TaskHandle_t worker;
    TaskHandle_t reader;
    serial_event_cb_t callback;
    bool initialized;
    bool tx_busy;
    bool processing;
    bool config_pending;
    bool tx_fault;
    uint32_t owner;
    uint32_t epoch;
    uint32_t revision;
    serial_config_t config;
    serial_job_t jobs[JOB_COUNT];
    size_t job_head;
    size_t job_count;
    repeat_state_t repeat;
    int64_t next_tx_us;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t uart_overflows;
    uint64_t frame_errors;
    uint64_t parity_errors;
    serial_history_t history;
} s;

static void take_lock(void) { xSemaphoreTake(s.lock, portMAX_DELAY); }
static void give_lock(void) { xSemaphoreGive(s.lock); }
static void wake_worker(void) { if (s.worker) xTaskNotifyGive(s.worker); }

static cJSON *config_json(const serial_config_t *config)
{
    cJSON *result = cJSON_CreateObject();
    if (!result) return NULL;
    char parity[2] = { config->parity, 0 };
    cJSON_AddNumberToObject(result, "baud_rate", config->baud_rate);
    cJSON_AddNumberToObject(result, "data_bits", config->data_bits);
    cJSON_AddStringToObject(result, "parity", parity);
    cJSON_AddNumberToObject(result, "stop_bits", config->stop_bits);
    cJSON_AddNumberToObject(result, "gap_us", config->gap_us);
    return result;
}

/* The callback copies small events. It never receives a UART data buffer and
 * is never invoked while the serial mutex is held. */
static void emit(uint32_t owner, const char *type, const char *id, cJSON *payload)
{
    if (payload && s.callback) s.callback(owner, type, id && *id ? id : NULL, payload);
    cJSON_Delete(payload);
}

static void emit_failure(const serial_job_t *job, const char *type, const char *code)
{
    cJSON *payload = cJSON_CreateObject();
    if (payload) cJSON_AddStringToObject(payload, "code", code);
    emit(job->owner, type, job->id, payload);
}

static void emit_repeat(uint32_t owner, const char *id)
{
    take_lock();
    bool active = s.repeat.active;
    uint32_t interval = s.repeat.interval_ms;
    uint64_t skipped = s.repeat.skipped;
    give_lock();
    cJSON *payload = cJSON_CreateObject();
    if (payload) {
        cJSON_AddBoolToObject(payload, "active", active);
        cJSON_AddNumberToObject(payload, "interval_ms", interval);
        cJSON_AddNumberToObject(payload, "skipped", (double)skipped);
    }
    emit(owner, "repeat.changed", id, payload);
}

static bool uint_field(const cJSON *payload, const char *key,
                       uint32_t min_value, uint32_t max_value, uint32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(payload, key);
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= min_value) ||
        !(item->valuedouble <= max_value)) return false;
    uint32_t value = (uint32_t)item->valuedouble;
    if ((double)value != item->valuedouble) return false;
    *out = value;
    return true;
}

static bool valid_id(const char *id)
{
    if (!id || !*id) return false;
    size_t length = 0;
    while (id[length]) {
        if ((unsigned char)id[length] < 32 || (unsigned char)id[length] > 126 ||
            ++length > SERIAL_REQUEST_ID_MAX) return false;
    }
    return true;
}

static bool parse_config(const cJSON *payload, serial_config_t *config)
{
    uint32_t bits, stops;
    const cJSON *parity = cJSON_GetObjectItemCaseSensitive(payload, "parity");
    if (!uint_field(payload, "baud_rate", 1200, 115200, &config->baud_rate) ||
        !uint_field(payload, "data_bits", 5, 8, &bits) ||
        !uint_field(payload, "stop_bits", 1, 2, &stops) ||
        !uint_field(payload, "gap_us", 0, 100000, &config->gap_us) ||
        !cJSON_IsString(parity) || !parity->valuestring ||
        strlen(parity->valuestring) != 1) return false;
    config->data_bits = (uint8_t)bits;
    config->stop_bits = (uint8_t)stops;
    config->parity = parity->valuestring[0];
    return serial_config_validate(config) == NULL;
}

static uart_config_t uart_config(const serial_config_t *config)
{
    return (uart_config_t) {
        .baud_rate = (int)config->baud_rate,
        .data_bits = (uart_word_length_t)(config->data_bits - 5),
        .parity = config->parity == 'E' ? UART_PARITY_EVEN :
                  config->parity == 'O' ? UART_PARITY_ODD : UART_PARITY_DISABLE,
        .stop_bits = config->stop_bits == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
}

static void load_config(serial_config_t *config)
{
    nvs_handle_t handle;
    if (nvs_open("serial", NVS_READONLY, &handle) != ESP_OK) return;
    uint32_t values[6];
    size_t length = sizeof(values);
    esp_err_t result = nvs_get_blob(handle, "config", values, &length);
    nvs_close(handle);
    if (result != ESP_OK || length != sizeof(values) || values[0] != 1 ||
        values[2] < 5 || values[2] > 8 || values[3] > 127 ||
        values[4] < 1 || values[4] > 2) return;
    serial_config_t loaded = {
        .baud_rate = values[1], .data_bits = (uint8_t)values[2],
        .parity = (char)values[3], .stop_bits = (uint8_t)values[4],
        .gap_us = values[5],
    };
    if (!serial_config_validate(&loaded)) *config = loaded;
}

static esp_err_t save_config(const serial_config_t *config)
{
    const uint32_t values[] = { 1, config->baud_rate, config->data_bits,
                               (uint32_t)config->parity, config->stop_bits,
                               config->gap_us };
    nvs_handle_t handle;
    esp_err_t error = nvs_open("serial", NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_set_blob(handle, "config", values, sizeof(values));
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}

/* Called with lock held. Epoch changes revoke jobs even when owner IDs are
 * subsequently reused. A started UART frame remains outside this queue. */
static void clear_pending(void)
{
    s.job_head = 0;
    s.job_count = 0;
    s.config_pending = false;
    s.repeat.active = false;
    ++s.repeat.generation;
}

/* Completion waits and NVS writes can span several scheduled deadlines. All
 * deadlines elapsed while busy are skipped rather than emitted afterward. */
static void skip_busy_deadlines_locked(int64_t now)
{
    if (!s.repeat.active || now < s.repeat.next_us) return;
    int64_t period = (int64_t)s.repeat.interval_ms * 1000;
    uint64_t elapsed = (uint64_t)((now - s.repeat.next_us) / period) + 1;
    s.repeat.next_us += (int64_t)elapsed * period;
    s.repeat.skipped += elapsed;
}

void serial_service_set_owner(uint32_t owner)
{
    if (!s.initialized) return;
    take_lock();
    if (s.owner == owner) {
        give_lock();
        return;
    }
    uint32_t previous = s.owner;
    bool was_repeating = s.repeat.active;
    s.owner = owner;
    ++s.epoch;
    clear_pending();
    give_lock();
    wake_worker();
    if (was_repeating) emit_repeat(previous, NULL);
}

static const char *check_owner_locked(uint32_t owner, uint32_t epoch)
{
    if (!owner || owner != s.owner || epoch != s.epoch) return "no_control";
    if (s.tx_fault) return "device_busy";
    return NULL;
}

const char *serial_service_command(uint32_t owner, const char *id,
                                  const char *type, const cJSON *payload,
                                  cJSON *reply)
{
    if (!s.initialized) return "not_ready";
    if (!valid_id(id) || !type || !cJSON_IsObject(payload)) return "invalid_argument";
    take_lock();
    uint32_t epoch = s.epoch;
    uint32_t revision = s.revision;
    serial_config_t config = s.config;
    const char *error = check_owner_locked(owner, epoch);
    give_lock();
    if (error) return error;

    if (!strcmp(type, "repeat.stop")) {
        /* This cancellation path never waits behind ordinary TX jobs. */
        take_lock();
        error = check_owner_locked(owner, epoch);
        if (!error) {
            s.repeat.active = false;
            ++s.repeat.generation;
        }
        give_lock();
        if (error) return error;
        wake_worker();
        emit_repeat(owner, id);
        return NULL;
    }

    serial_job_t job = { .owner = owner, .epoch = epoch, .revision = revision };
    memcpy(job.id, id, strlen(id) + 1);
    bool repeat_start = !strcmp(type, "repeat.start");
    uint32_t interval = 0;
    if (!strcmp(type, "tx.send") || repeat_start) {
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(payload, "data_b64");
        const cJSON *crc = cJSON_GetObjectItemCaseSensitive(payload, "crc");
        if (!cJSON_IsString(data) || !data->valuestring ||
            !cJSON_IsString(crc) || !crc->valuestring) return "invalid_argument";
        bool append_crc = !strcmp(crc->valuestring, "modbus");
        if (!append_crc && strcmp(crc->valuestring, "none")) return "invalid_crc";
        error = serial_prepare_tx(&config, data->valuestring, strlen(data->valuestring),
                                  append_crc, job.bytes, &job.length);
        if (error) return error;
        job.type = JOB_TX;
        if (repeat_start && (!uint_field(payload, "interval_ms", 20, 3600000, &interval) ||
            (uint64_t)interval * 1000 < serial_wire_time_us(&config, job.length) + config.gap_us)) {
            return "invalid_interval";
        }
    } else if (!strcmp(type, "uart.configure")) {
        job.type = JOB_CONFIG;
        if (!parse_config(payload, &job.config)) return "invalid_config";
    } else if (!strcmp(type, "uart.save")) {
        job.type = JOB_SAVE;
    } else {
        return "unknown_command";
    }

    take_lock();
    error = check_owner_locked(owner, epoch);
    if (!error && (revision != s.revision || s.config_pending)) error = "device_busy";
    if (!error && job.type == JOB_CONFIG &&
        (s.tx_busy || s.processing || s.job_count || s.repeat.active)) error = "device_busy";
    if (!error && repeat_start && s.repeat.active) error = "device_busy";
    if (!error && !repeat_start && s.job_count == JOB_COUNT) error = "queue_full";
    if (!error) {
        if (repeat_start) {
            job.periodic = true;
            job.repeat_generation = ++s.repeat.generation;
            s.repeat.snapshot = job;
            s.repeat.interval_ms = interval;
            s.repeat.skipped = 0;
            s.repeat.next_us = esp_timer_get_time();
            s.repeat.active = true;
        } else {
            size_t tail = (s.job_head + s.job_count) % JOB_COUNT;
            s.jobs[tail] = job;
            ++s.job_count;
            if (job.type == JOB_CONFIG) s.config_pending = true;
        }
    }
    size_t queued = s.job_count;
    give_lock();
    if (error) return error;
    if (reply && !repeat_start) cJSON_AddNumberToObject(reply, "queued", (double)queued);
    wake_worker();
    if (repeat_start) emit_repeat(owner, id);
    return NULL;
}

static void record_rx(const uint8_t *bytes, size_t length)
{
    uint64_t now = (uint64_t)esp_timer_get_time();
    take_lock();
    s.rx_bytes += length;
    serial_history_append(&s.history, 'r', bytes, length, now, s.revision, NULL);
    give_lock();
}

static void read_task(void *arg)
{
    (void)arg;
    uint8_t bytes[SERIAL_MAX_BYTES];
    for (;;) {
        if (xEventGroupGetBits(s.rx_sync) & RX_PAUSE_REQUEST) {
            /* Drain a bounded amount under the old config, then acknowledge.
             * Continuous traffic must not prevent a requested configuration. */
            for (size_t read = 0; read < 16384;) {
                int length = uart_read_bytes(PORT, bytes, sizeof(bytes), 0);
                if (length <= 0) break;
                record_rx(bytes, (size_t)length);
                read += (size_t)length;
            }
            xEventGroupSetBits(s.rx_sync, RX_PAUSED);
            while (xEventGroupGetBits(s.rx_sync) & RX_PAUSE_REQUEST) vTaskDelay(1);
            xEventGroupClearBits(s.rx_sync, RX_PAUSED);
        }
        uart_event_t event;
        while (xQueueReceive(s.uart_events, &event, 0) == pdTRUE) {
            take_lock();
            if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) ++s.uart_overflows;
            if (event.type == UART_FRAME_ERR) ++s.frame_errors;
            if (event.type == UART_PARITY_ERR) ++s.parity_errors;
            give_lock();
            /* Do not flush valid buffered RX bytes on an overflow notification.
             * UART hardware loss cannot be represented as an exact byte count. */
        }
        int length = uart_read_bytes(PORT, bytes, sizeof(bytes), pdMS_TO_TICKS(10) ?: 1);
        if (length > 0) record_rx(bytes, (size_t)length);
    }
}

static void run_config(const serial_job_t *job)
{
    xEventGroupClearBits(s.rx_sync, RX_PAUSED);
    xEventGroupSetBits(s.rx_sync, RX_PAUSE_REQUEST);
    EventBits_t bits = xEventGroupWaitBits(s.rx_sync, RX_PAUSED, pdFALSE, pdTRUE,
                                          pdMS_TO_TICKS(250));
    size_t discarded = 0;
    esp_err_t error = ESP_ERR_TIMEOUT;
    bool restore_failed = false;
    if (bits & RX_PAUSED) {
        take_lock();
        if (job->owner == s.owner && job->epoch == s.epoch) {
            uart_get_buffered_data_len(PORT, &discarded);
            uart_flush_input(PORT);
            uart_config_t hardware = uart_config(&job->config);
            error = uart_param_config(PORT, &hardware);
            if (error == ESP_OK) {
                /* The hardware and its published revision change atomically
                 * with respect to owner revocation and new command snapshots. */
                s.config = job->config;
                ++s.revision;
                s.next_tx_us = 0;
            } else {
                /* IDF can fail after changing some UART registers. Restore the
                 * old configuration before allowing more bytes onto the bus. */
                uart_config_t previous = uart_config(&s.config);
                if (uart_param_config(PORT, &previous) != ESP_OK) {
                    restore_failed = true;
                    s.tx_fault = true;
                    clear_pending();
                }
            }
        } else {
            error = ESP_ERR_INVALID_STATE;
        }
        give_lock();
    }
    take_lock();
    uint32_t revision = s.revision;
    skip_busy_deadlines_locked(esp_timer_get_time());
    s.config_pending = false;
    s.processing = false;
    give_lock();
    xEventGroupClearBits(s.rx_sync, RX_PAUSE_REQUEST);
    /* Complete the handshake before the worker can start another configure
     * job. Otherwise a stale RX_PAUSED bit could acknowledge the next pause. */
    for (unsigned i = 0; i < 25 &&
         (xEventGroupGetBits(s.rx_sync) & RX_PAUSED); ++i) vTaskDelay(1);
    if (error != ESP_OK) {
        emit_failure(job, "command.failed", restore_failed ? "configure_restore_failed" : "configure_failed");
        return;
    }
    cJSON *payload = cJSON_CreateObject();
    if (payload) {
        cJSON_AddNumberToObject(payload, "config_revision", revision);
        cJSON_AddItemToObject(payload, "config", config_json(&job->config));
        cJSON_AddNumberToObject(payload, "rx_discarded_bytes", (double)discarded);
    }
    emit(job->owner, "config.applied", job->id, payload);
}

static void run_save(const serial_job_t *job)
{
    take_lock();
    if (job->owner != s.owner || job->epoch != s.epoch) {
        s.processing = false;
        give_lock();
        return;
    }
    serial_config_t config = s.config;
    uint32_t revision = s.revision;
    give_lock();
    esp_err_t result = save_config(&config);
    take_lock();
    skip_busy_deadlines_locked(esp_timer_get_time());
    s.processing = false;
    give_lock();
    if (result != ESP_OK) {
        emit_failure(job, "command.failed", "save_failed");
        return;
    }
    cJSON *payload = cJSON_CreateObject();
    if (payload) cJSON_AddNumberToObject(payload, "config_revision", revision);
    emit(job->owner, "config.saved", job->id, payload);
}

static void run_tx(const serial_job_t *job)
{
    /* The driver TX ring has 2 KiB and only this task writes to it. After the
     * previous uart_wait_tx_done, this <=1 KiB submission cannot wait for wire
     * time. Holding the mutex through submission makes revocation atomic with
     * starting a frame, without ever holding it during completion waits. */
    take_lock();
    if (job->owner != s.owner || job->epoch != s.epoch ||
        (job->periodic && (!s.repeat.active ||
                          job->repeat_generation != s.repeat.generation))) {
        s.processing = false;
        give_lock();
        return;
    }
    if (s.tx_fault || job->revision != s.revision) {
        s.processing = false;
        give_lock();
        emit_failure(job, "tx.failed", "config_changed");
        return;
    }
    serial_config_t config = s.config;
    int written = uart_write_bytes(PORT, job->bytes, job->length);
    s.tx_busy = written > 0;
    give_lock();
    if (written <= 0) {
        take_lock();
        s.processing = false;
        give_lock();
        emit_failure(job, "tx.failed", "write_failed");
        return;
    }
    cJSON *started = cJSON_CreateObject();
    if (started) cJSON_AddNumberToObject(started, "length", written);
    emit(job->owner, "tx.started", job->id, started);
    uint64_t wire_us = serial_wire_time_us(&config, (size_t)written);
    uint32_t timeout_ms = (uint32_t)(wire_us / 1000 + 1000);
    esp_err_t result = uart_wait_tx_done(PORT, pdMS_TO_TICKS(timeout_ms));
    bool timeout = result != ESP_OK;
    if (timeout) {
        take_lock();
        s.tx_fault = true;
        clear_pending();
        give_lock();
        emit_failure(job, "tx.failed", "tx_timeout");
        /* A timeout must not start the next frame while hardware may still be
         * sending. Stay faulted until it actually drains; never retry bytes. */
        while (uart_wait_tx_done(PORT, pdMS_TO_TICKS(100)) != ESP_OK) vTaskDelay(1);
    }
    uint64_t finished = (uint64_t)esp_timer_get_time();
    take_lock();
    uint32_t seq = 0;
    skip_busy_deadlines_locked((int64_t)finished);
    if (!timeout) {
        s.tx_bytes += (size_t)written;
        seq = serial_history_append(&s.history, 't', job->bytes, (size_t)written,
                                     finished, job->revision, job->id);
    }
    s.next_tx_us = (int64_t)finished + config.gap_us;
    s.tx_busy = false;
    s.processing = false;
    s.tx_fault = false;
    give_lock();
    if (timeout) return;
    if ((size_t)written != job->length) {
        emit_failure(job, "tx.failed", "partial_write");
        return;
    }
    cJSON *payload = cJSON_CreateObject();
    if (payload) {
        cJSON_AddNumberToObject(payload, "length", written);
        cJSON_AddNumberToObject(payload, "seq", seq);
        cJSON_AddNumberToObject(payload, "t_us", (double)finished);
    }
    emit(job->owner, "tx.done", job->id, payload);
}

static void write_task(void *arg)
{
    (void)arg;
    serial_job_t job;
    for (;;) {
        bool ready = false;
        take_lock();
        int64_t now = esp_timer_get_time();
        bool queued = s.job_count != 0;
        /* Count every elapsed deadline, including time spent in blocking UART
         * completion waits. There is never a queue of missed periodic work. */
        if (s.repeat.active && now >= s.repeat.next_us) {
            int64_t period = (int64_t)s.repeat.interval_ms * 1000;
            uint64_t elapsed = (uint64_t)((now - s.repeat.next_us) / period) + 1;
            s.repeat.next_us += (int64_t)elapsed * period;
            if (queued || now < s.next_tx_us || s.tx_fault) {
                s.repeat.skipped += elapsed;
            } else {
                s.repeat.skipped += elapsed - 1;
                job = s.repeat.snapshot;
                ready = true;
            }
        }
        if (!ready && queued &&
            (s.jobs[s.job_head].type != JOB_TX || now >= s.next_tx_us)) {
            job = s.jobs[s.job_head];
            s.job_head = (s.job_head + 1) % JOB_COUNT;
            --s.job_count;
            ready = true;
        }
        if (ready) {
            if (job.owner != s.owner || job.epoch != s.epoch) {
                ready = false;
                if (job.type == JOB_CONFIG) s.config_pending = false;
            } else {
                s.processing = true;
            }
        }
        give_lock();
        if (!ready) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5) ?: 1);
            continue;
        }
        if (job.type == JOB_TX) run_tx(&job);
        else if (job.type == JOB_CONFIG) run_config(&job);
        else run_save(&job);
    }
}

esp_err_t serial_service_init(serial_event_cb_t callback)
{
    if (s.initialized) return ESP_ERR_INVALID_STATE;
    memset(&s, 0, sizeof(s));
    s.config = (serial_config_t) { .baud_rate = 115200, .data_bits = 8,
                                   .parity = 'N', .stop_bits = 1, .gap_us = 2000 };
    load_config(&s.config);
    s.callback = callback;
    s.revision = 1;
    s.epoch = 1;
    s.lock = xSemaphoreCreateMutex();
    s.rx_sync = xEventGroupCreate();
    if (!s.lock || !s.rx_sync) goto no_memory;
    uart_config_t hardware = uart_config(&s.config);
    esp_err_t error = uart_param_config(PORT, &hardware);
    if (error != ESP_OK) goto failed;
    error = uart_set_pin(PORT, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (error != ESP_OK) goto failed;
    error = uart_driver_install(PORT, 16384, 2048, 32, &s.uart_events, 0);
    if (error != ESP_OK) goto failed;
    error = uart_set_mode(PORT, UART_MODE_UART);
    if (error != ESP_OK) goto driver_failed;
    if (xTaskCreate(read_task, "serial_rx", 4096, NULL, 12, &s.reader) != pdPASS) {
        error = ESP_ERR_NO_MEM;
        goto driver_failed;
    }
    if (xTaskCreate(write_task, "serial_tx", 6144, NULL, 9, &s.worker) != pdPASS) {
        vTaskDelete(s.reader);
        s.reader = NULL;
        error = ESP_ERR_NO_MEM;
        goto driver_failed;
    }
    s.initialized = true;
    return ESP_OK;
driver_failed:
    uart_driver_delete(PORT);
failed:
    vEventGroupDelete(s.rx_sync);
    vSemaphoreDelete(s.lock);
    s.rx_sync = NULL;
    s.lock = NULL;
    return error;
no_memory:
    if (s.rx_sync) vEventGroupDelete(s.rx_sync);
    if (s.lock) vSemaphoreDelete(s.lock);
    s.rx_sync = NULL;
    s.lock = NULL;
    return ESP_ERR_NO_MEM;
}

cJSON *serial_service_status(void)
{
    if (!s.initialized) return NULL;
    take_lock();
    serial_config_t config = s.config;
    uint32_t revision = s.revision, owner = s.owner;
    bool busy = s.tx_busy, active = s.repeat.active;
    size_t queued = s.job_count;
    uint32_t interval = s.repeat.interval_ms;
    uint64_t skipped = s.repeat.skipped;
    uint64_t rx = s.rx_bytes, tx = s.tx_bytes;
    uint64_t overflows = s.uart_overflows, frame = s.frame_errors, parity = s.parity_errors;
    uint64_t evicted = s.history.evicted_bytes;
    give_lock();
    cJSON *result = cJSON_CreateObject();
    cJSON *repeat = cJSON_CreateObject();
    cJSON *stats = cJSON_CreateObject();
    if (!result || !repeat || !stats) {
        cJSON_Delete(result); cJSON_Delete(repeat); cJSON_Delete(stats);
        return NULL;
    }
    cJSON_AddItemToObject(result, "config", config_json(&config));
    cJSON_AddNumberToObject(result, "config_revision", revision);
    cJSON_AddNumberToObject(result, "owner_id", owner);
    cJSON_AddBoolToObject(result, "tx_busy", busy);
    cJSON_AddNumberToObject(result, "queued", (double)queued);
    cJSON_AddBoolToObject(repeat, "active", active);
    cJSON_AddNumberToObject(repeat, "interval_ms", interval);
    cJSON_AddNumberToObject(repeat, "skipped", (double)skipped);
    cJSON_AddItemToObject(result, "repeat", repeat);
    cJSON_AddNumberToObject(stats, "rx_bytes", (double)rx);
    cJSON_AddNumberToObject(stats, "tx_bytes", (double)tx);
    cJSON_AddNumberToObject(stats, "uart_overflows", (double)overflows);
    cJSON_AddNumberToObject(stats, "frame_errors", (double)frame);
    cJSON_AddNumberToObject(stats, "parity_errors", (double)parity);
    cJSON_AddNumberToObject(stats, "history_evicted_bytes", (double)evicted);
    cJSON_AddItemToObject(result, "stats", stats);
    return result;
}

typedef struct {
    serial_record_t record;
    uint8_t bytes[SERIAL_MAX_BYTES];
} copied_record_t;

cJSON *serial_service_history(uint32_t after_seq, size_t max_records)
{
    if (!s.initialized) return NULL;
    if (max_records < 1) max_records = 1;
    if (max_records > 4) max_records = 4;
    copied_record_t *copies = calloc(max_records, sizeof(*copies));
    if (!copies) return NULL;
    take_lock();
    uint32_t oldest = serial_history_oldest_seq(&s.history);
    uint32_t latest = s.history.latest_seq;
    uint64_t evicted = s.history.evicted_bytes;
    bool gap = after_seq && oldest && after_seq < oldest - 1;
    size_t count = 0;
    for (size_t i = 0; i < s.history.count && count < max_records; ++i) {
        const serial_record_t *record = &s.history.records[(s.history.head + i) % SERIAL_HISTORY_RECORDS];
        if (after_seq && record->seq <= after_seq) continue;
        if (serial_history_get(&s.history, i, &copies[count].record,
                                copies[count].bytes, SERIAL_MAX_BYTES)) ++count;
    }
    give_lock();
    cJSON *result = cJSON_CreateObject();
    cJSON *records = cJSON_CreateArray();
    if (!result || !records) {
        cJSON_Delete(result); cJSON_Delete(records); free(copies);
        return NULL;
    }
    cJSON_AddItemToObject(result, "records", records);
    cJSON_AddNumberToObject(result, "oldest_seq", oldest);
    cJSON_AddNumberToObject(result, "latest_seq", latest);
    cJSON_AddBoolToObject(result, "gap", gap);
    cJSON_AddNumberToObject(result, "evicted_bytes", (double)evicted);
    char base64[((SERIAL_MAX_BYTES + 2) / 3) * 4 + 1];
    for (size_t i = 0; i < count; ++i) {
        serial_record_t *record = &copies[i].record;
        serial_base64_encode(copies[i].bytes, record->length, base64, sizeof(base64));
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(result); free(copies);
            return NULL;
        }
        cJSON_AddNumberToObject(item, "seq", record->seq);
        cJSON_AddNumberToObject(item, "t_us", (double)record->t_us);
        cJSON_AddStringToObject(item, "direction", record->direction == 'r' ? "rx" : "tx");
        cJSON_AddStringToObject(item, "data_b64", base64);
        cJSON_AddNumberToObject(item, "length", record->length);
        cJSON_AddNumberToObject(item, "config_revision", record->config_revision);
        if (*record->id) cJSON_AddStringToObject(item, "id", record->id);
        cJSON_AddItemToArray(records, item);
    }
    free(copies);
    return result;
}

uint32_t serial_service_latest_seq(void)
{
    if (!s.initialized) return 0;
    take_lock();
    uint32_t seq = s.history.latest_seq;
    give_lock();
    return seq;
}
