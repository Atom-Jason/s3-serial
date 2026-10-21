#include "serial_core.h"

#include <string.h>

const char *serial_config_validate(const serial_config_t *c)
{
    if (!c || c->baud_rate < 1200 || c->baud_rate > 115200 ||
        c->data_bits < 5 || c->data_bits > 8 ||
        (c->stop_bits != 1 && c->stop_bits != 2) ||
        (c->parity != 'N' && c->parity != 'E' && c->parity != 'O') ||
        c->gap_us > 100000) {
        return "invalid_config";
    }
    return NULL;
}

uint64_t serial_wire_time_us(const serial_config_t *c, size_t length)
{
    if (serial_config_validate(c)) return 0;
    const uint64_t bits = 1u + c->data_bits + c->stop_bits + (c->parity != 'N');
    return ((uint64_t)length * bits * 1000000u + c->baud_rate - 1u) / c->baud_rate;
}

uint16_t serial_crc16_modbus(const uint8_t *bytes, size_t length)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (uint16_t)((crc >> 1) ^ ((crc & 1) ? 0xa001 : 0));
        }
    }
    return crc;
}

static int b64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool serial_base64_decode(const char *text, size_t length,
                          uint8_t *out, size_t capacity, size_t *out_length)
{
    if (out_length) *out_length = 0;
    if (!text || !out_length || length % 4 != 0) return false;
    if (length == 0) return true;
    size_t padding = (text[length - 1] == '=') + (text[length - 2] == '=');
    size_t decoded = length / 4 * 3 - padding;
    if (!out || decoded > capacity) return false;
    size_t cursor = 0;
    for (size_t i = 0; i < length; i += 4) {
        int a = b64_value((unsigned char)text[i]);
        int b = b64_value((unsigned char)text[i + 1]);
        int c = b64_value((unsigned char)text[i + 2]);
        int d = b64_value((unsigned char)text[i + 3]);
        bool last = i + 4 == length;
        if (a < 0 || b < 0) return false;
        if (text[i + 2] == '=') {
            if (!last || text[i + 3] != '=' || (b & 15) != 0) return false;
            out[cursor++] = (uint8_t)((a << 2) | (b >> 4));
        } else if (text[i + 3] == '=') {
            if (!last || c < 0 || (c & 3) != 0) return false;
            out[cursor++] = (uint8_t)((a << 2) | (b >> 4));
            out[cursor++] = (uint8_t)((b << 4) | (c >> 2));
        } else {
            if (c < 0 || d < 0) return false;
            out[cursor++] = (uint8_t)((a << 2) | (b >> 4));
            out[cursor++] = (uint8_t)((b << 4) | (c >> 2));
            out[cursor++] = (uint8_t)((c << 6) | d);
        }
    }
    *out_length = cursor;
    return true;
}

size_t serial_base64_encode(const uint8_t *bytes, size_t length,
                            char *out, size_t capacity)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t required = ((length + 2) / 3) * 4;
    if ((!bytes && length) || !out || required + 1 > capacity) return 0;
    size_t cursor = 0;
    for (size_t i = 0; i < length; i += 3) {
        uint32_t value = (uint32_t)bytes[i] << 16;
        if (i + 1 < length) value |= (uint32_t)bytes[i + 1] << 8;
        if (i + 2 < length) value |= bytes[i + 2];
        out[cursor++] = alphabet[(value >> 18) & 63];
        out[cursor++] = alphabet[(value >> 12) & 63];
        out[cursor++] = i + 1 < length ? alphabet[(value >> 6) & 63] : '=';
        out[cursor++] = i + 2 < length ? alphabet[value & 63] : '=';
    }
    out[cursor] = '\0';
    return cursor;
}

const char *serial_prepare_tx(const serial_config_t *c, const char *base64,
                              size_t base64_length, bool crc,
                              uint8_t *out, size_t *length)
{
    const char *error = serial_config_validate(c);
    if (error) return error;
    if (!out || !length) return "invalid_argument";
    *length = 0;
    if (base64_length > ((SERIAL_MAX_BYTES + 2) / 3) * 4) return "too_large";
    if (!serial_base64_decode(base64, base64_length, out, SERIAL_MAX_BYTES, length)) {
        return "invalid_base64";
    }
    if (*length == 0) return "empty_data";
    if (c->data_bits != 8) {
        if (crc) return "crc_requires_8_bits";
        unsigned max_value = (1u << c->data_bits) - 1u;
        for (size_t i = 0; i < *length; ++i) {
            if (out[i] > max_value) return "byte_out_of_range";
        }
    }
    if (crc) {
        if (*length > SERIAL_MAX_BYTES - 2) return "too_large";
        uint16_t value = serial_crc16_modbus(out, *length);
        out[(*length)++] = (uint8_t)value;
        out[(*length)++] = (uint8_t)(value >> 8);
    }
    return NULL;
}

void serial_history_init(serial_history_t *history)
{
    memset(history, 0, sizeof(*history));
}

static void evict_one(serial_history_t *h)
{
    const serial_record_t *record = &h->records[h->head];
    h->byte_used -= record->length;
    h->evicted_bytes += record->length;
    h->head = (h->head + 1) % SERIAL_HISTORY_RECORDS;
    --h->count;
}

uint32_t serial_history_append(serial_history_t *h, char direction,
                               const uint8_t *bytes, size_t length,
                               uint64_t t_us, uint32_t revision,
                               const char *id)
{
    if (!h || !bytes || !length || length > SERIAL_MAX_BYTES ||
        (direction != 'r' && direction != 't')) return 0;
    while (h->count && (h->count == SERIAL_HISTORY_RECORDS ||
           h->byte_used + length > SERIAL_HISTORY_BYTES)) evict_one(h);
    size_t index = (h->head + h->count) % SERIAL_HISTORY_RECORDS;
    serial_record_t *record = &h->records[index];
    memset(record, 0, sizeof(*record));
    /* The protocol's uint32 sequence is scoped by boot_id. Skip reserved zero. */
    if (++h->latest_seq == 0) ++h->latest_seq;
    record->seq = h->latest_seq;
    record->t_us = t_us;
    record->config_revision = revision;
    record->offset = (uint16_t)h->byte_head;
    record->length = (uint16_t)length;
    record->direction = direction;
    if (id) {
        strncpy(record->id, id, SERIAL_REQUEST_ID_MAX);
        record->id[SERIAL_REQUEST_ID_MAX] = '\0';
    }
    size_t first = SERIAL_HISTORY_BYTES - h->byte_head;
    if (first > length) first = length;
    memcpy(h->bytes + h->byte_head, bytes, first);
    memcpy(h->bytes, bytes + first, length - first);
    h->byte_head = (h->byte_head + length) % SERIAL_HISTORY_BYTES;
    h->byte_used += length;
    ++h->count;
    return record->seq;
}

bool serial_history_get(const serial_history_t *h, size_t index,
                         serial_record_t *record, uint8_t *bytes,
                         size_t capacity)
{
    if (!h || !record || !bytes || index >= h->count) return false;
    const serial_record_t *source = &h->records[(h->head + index) % SERIAL_HISTORY_RECORDS];
    if (source->length > capacity) return false;
    *record = *source;
    size_t first = SERIAL_HISTORY_BYTES - source->offset;
    if (first > source->length) first = source->length;
    memcpy(bytes, h->bytes + source->offset, first);
    memcpy(bytes + first, h->bytes, source->length - first);
    return true;
}

uint32_t serial_history_oldest_seq(const serial_history_t *h)
{
    return h->count ? h->records[h->head].seq : 0;
}
