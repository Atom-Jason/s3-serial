#include "serial_core.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static serial_config_t config = { .baud_rate = 115200, .data_bits = 8,
                                  .stop_bits = 1, .parity = 'N', .gap_us = 2000 };

static void test_config_and_wire_time(void)
{
    assert(!serial_config_validate(&config));
    assert(serial_wire_time_us(&config, 1024) == 88889);
    serial_config_t c = config;
    c.baud_rate = 1200;
    c.parity = 'E';
    c.stop_bits = 2;
    assert(serial_wire_time_us(&c, 1) == 10000);
    c.baud_rate = 0;
    assert(serial_config_validate(&c));
    assert(serial_wire_time_us(&c, 1) == 0);
    c = config; c.gap_us = 100001;
    assert(serial_config_validate(&c));
    c = config; c.data_bits = 9;
    assert(serial_config_validate(&c));
    c = config; c.parity = 'X';
    assert(serial_config_validate(&c));
}

static void test_base64(void)
{
    static const struct { const char *text; const char *encoded; } vectors[] = {
        { "", "" }, { "f", "Zg==" }, { "fo", "Zm8=" },
        { "foo", "Zm9v" }, { "foob", "Zm9vYg==" },
        { "fooba", "Zm9vYmE=" }, { "foobar", "Zm9vYmFy" },
    };
    char encoded[1400];
    uint8_t decoded[SERIAL_MAX_BYTES];
    size_t length;
    for (size_t i = 0; i < sizeof(vectors) / sizeof(*vectors); ++i) {
        size_t input_length = strlen(vectors[i].text);
        assert(serial_base64_encode((const uint8_t *)vectors[i].text, input_length,
                                     encoded, sizeof(encoded)) == strlen(vectors[i].encoded));
        assert(!strcmp(encoded, vectors[i].encoded));
        assert(serial_base64_decode(encoded, strlen(encoded), decoded, sizeof(decoded), &length));
        assert(length == input_length);
        assert(!memcmp(decoded, vectors[i].text, length));
    }
    static const char *invalid[] = {
        "A", "AAA", "AAAA=", "====", "A===", "AA=A", "Zg==AAAA", "Zh==",
        "Zm9=", "AA-A", "AA_A", " TQ==", "TQ==\n", "TQ\t=", "Zm=v", "*AAA",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        length = 999;
        assert(!serial_base64_decode(invalid[i], strlen(invalid[i]), decoded,
                                      sizeof(decoded), &length));
        assert(length == 0);
    }
    assert(!serial_base64_decode("AAAA", 4, decoded, 2, &length));
    assert(serial_base64_decode("AA==", 4, decoded, 1, &length));
    assert(length == 1 && decoded[0] == 0);
    assert(!serial_base64_encode((const uint8_t *)"f", 1, encoded, 4));

    /* All byte values, every legal packet length, and every padding variant. */
    uint8_t input[SERIAL_MAX_BYTES];
    for (size_t i = 0; i < sizeof(input); ++i) input[i] = (uint8_t)(i * 73 + 19);
    for (size_t n = 0; n <= SERIAL_MAX_BYTES; ++n) {
        size_t encoded_length = serial_base64_encode(input, n, encoded, sizeof(encoded));
        assert(encoded_length == ((n + 2) / 3) * 4);
        assert(serial_base64_decode(encoded, encoded_length, decoded, sizeof(decoded), &length));
        assert(length == n && !memcmp(input, decoded, n));
    }
}

static void test_tx_validation(void)
{
    const uint8_t standard[] = "123456789";
    assert(serial_crc16_modbus(standard, 9) == 0x4b37);
    uint8_t out[SERIAL_MAX_BYTES];
    size_t length;
    assert(!serial_prepare_tx(&config, "MTIzNDU2Nzg5", 12, true, out, &length));
    assert(length == 11 && out[9] == 0x37 && out[10] == 0x4b);
    assert(!strcmp(serial_prepare_tx(&config, "", 0, false, out, &length), "empty_data"));
    serial_config_t c = config;
    c.data_bits = 7;
    assert(!serial_prepare_tx(&c, "fw==", 4, false, out, &length));
    assert(!strcmp(serial_prepare_tx(&c, "gA==", 4, false, out, &length), "byte_out_of_range"));
    assert(!strcmp(serial_prepare_tx(&c, "fw==", 4, true, out, &length), "crc_requires_8_bits"));
    c.data_bits = 5;
    assert(!serial_prepare_tx(&c, "Hw==", 4, false, out, &length));
    assert(!strcmp(serial_prepare_tx(&c, "IA==", 4, false, out, &length), "byte_out_of_range"));
    char encoded[1400];
    uint8_t input[SERIAL_MAX_BYTES] = {0};
    for (size_t n = 1022; n <= 1024; ++n) {
        size_t text_length = serial_base64_encode(input, n, encoded, sizeof(encoded));
        assert(!serial_prepare_tx(&config, encoded, text_length, false, out, &length));
        assert(length == n);
        const char *error = serial_prepare_tx(&config, encoded, text_length, true, out, &length);
        if (n == 1022) assert(!error && length == 1024);
        else assert(error && !strcmp(error, "too_large"));
    }
}

static uint8_t sample_byte(uint32_t seq, size_t offset)
{
    return (uint8_t)(seq * 17 + offset * 31);
}

static void test_history(void)
{
    serial_history_t *history = calloc(1, sizeof(*history));
    assert(history && sizeof(*history) < 96 * 1024);
    serial_history_init(history);
    assert(!serial_history_oldest_seq(history));
    uint8_t bytes[SERIAL_MAX_BYTES], copied[SERIAL_MAX_BYTES];
    serial_record_t record;
    size_t total = 0;
    for (uint32_t seq = 1; seq <= 4000; ++seq) {
        size_t length = (seq * 137u) % SERIAL_MAX_BYTES + 1;
        for (size_t n = 0; n < length; ++n) bytes[n] = sample_byte(seq, n);
        total += length;
        assert(serial_history_append(history, seq % 2 ? 'r' : 't', bytes, length,
                                      (uint64_t)seq * 1000, 7, "packet-id") == seq);
        assert(history->byte_used <= SERIAL_HISTORY_BYTES);
        assert(history->count <= SERIAL_HISTORY_RECORDS);
        assert(history->evicted_bytes + history->byte_used == total);
        if (seq % 67 == 0 || seq == 4000) {
            size_t used = 0;
            uint32_t first_seq = serial_history_oldest_seq(history);
            for (size_t i = 0; i < history->count; ++i) {
                assert(serial_history_get(history, i, &record, copied, sizeof(copied)));
                assert(record.seq == first_seq + i);
                assert(record.t_us == (uint64_t)record.seq * 1000);
                assert(record.config_revision == 7 && !strcmp(record.id, "packet-id"));
                assert(record.direction == (record.seq % 2 ? 'r' : 't'));
                for (size_t n = 0; n < record.length; ++n) {
                    assert(copied[n] == sample_byte(record.seq, n));
                }
                used += record.length;
            }
            assert(used == history->byte_used);
        }
    }
    assert(!serial_history_get(history, history->count, &record, copied, sizeof(copied)));
    assert(!serial_history_get(history, 0, &record, copied, 0));
    size_t before = history->count;
    assert(!serial_history_append(history, 'x', bytes, 1, 0, 1, NULL));
    assert(history->count == before);

    /* Metadata pressure must evict old payload too, even below byte budget. */
    serial_history_init(history);
    for (unsigned i = 0; i < 513; ++i) {
        bytes[0] = (uint8_t)i;
        assert(serial_history_append(history, 'r', bytes, 1, i, 1, NULL) == i + 1);
    }
    assert(history->count == 512 && history->byte_used == 512);
    assert(history->evicted_bytes == 1 && serial_history_oldest_seq(history) == 2);
    free(history);
}

int main(void)
{
    test_config_and_wire_time();
    test_base64();
    test_tx_validation();
    test_history();
    puts("serial_core: configuration, strict Base64, CRC, wire timing, byte width, and bounded history passed");
    return 0;
}
