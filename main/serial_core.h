#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SERIAL_MAX_BYTES 1024u
#define SERIAL_HISTORY_BYTES (32u * 1024u)
#define SERIAL_HISTORY_RECORDS 512u
#define SERIAL_REQUEST_ID_MAX 40u

typedef struct {
    uint32_t baud_rate;
    uint32_t gap_us;
    uint8_t data_bits;
    uint8_t stop_bits;
    char parity;
} serial_config_t;

typedef struct {
    uint64_t t_us;
    uint32_t seq;
    uint32_t config_revision;
    uint16_t offset;
    uint16_t length;
    char direction;
    char id[SERIAL_REQUEST_ID_MAX + 1u];
} serial_record_t;

/* Metadata and payload have independent bounds. No per-record 1 KiB slot. */
typedef struct {
    uint8_t bytes[SERIAL_HISTORY_BYTES];
    serial_record_t records[SERIAL_HISTORY_RECORDS];
    size_t head;
    size_t count;
    size_t byte_head;
    size_t byte_used;
    uint32_t latest_seq;
    uint64_t evicted_bytes;
} serial_history_t;

const char *serial_config_validate(const serial_config_t *config);
uint64_t serial_wire_time_us(const serial_config_t *config, size_t length);
uint16_t serial_crc16_modbus(const uint8_t *bytes, size_t length);
/* Strict RFC 4648 standard alphabet, canonical padding, no whitespace. */
bool serial_base64_decode(const char *text, size_t text_length,
                          uint8_t *out, size_t capacity, size_t *out_length);
size_t serial_base64_encode(const uint8_t *bytes, size_t length,
                            char *out, size_t capacity);
const char *serial_prepare_tx(const serial_config_t *config,
                              const char *base64, size_t base64_length,
                              bool append_modbus_crc, uint8_t *out,
                              size_t *out_length);
void serial_history_init(serial_history_t *history);
uint32_t serial_history_append(serial_history_t *history, char direction,
                               const uint8_t *bytes, size_t length,
                               uint64_t t_us, uint32_t revision,
                               const char *request_id);
/* Caller serializes access. Copies one logical record and wrapped payload. */
bool serial_history_get(const serial_history_t *history, size_t logical_index,
                         serial_record_t *record, uint8_t *bytes,
                         size_t capacity);
uint32_t serial_history_oldest_seq(const serial_history_t *history);
