import test from 'node:test';
import assert from 'node:assert/strict';
import { setLocale } from '../web/i18n.mjs';
setLocale('zh-CN');
import { parseHex, hex, base64, unbase64, crc16, finalizeBytes, buildPayload, validateConfig, minimumInterval, validateInterval, LogStore, exportLog, validateMacros } from '../web/core.mjs';

test('HEX parsing preserves all binary byte values and rejects ambiguous input', () => {
  const all = Uint8Array.from({ length: 256 }, (_, index) => index);
  assert.deepEqual(parseHex(hex(all)), all);
  assert.deepEqual(parseHex('00\n01\tfe FF'), Uint8Array.of(0, 1, 254, 255));
  assert.throws(() => parseHex('0'), /半个字节/);
  assert.throws(() => parseHex('0x01'), /0x/);
  assert.throws(() => parseHex('01;ff'), /HEX/);
});
test('Base64 round-trips null bytes and rejects noncanonical or malformed values', () => {
  const bytes = Uint8Array.of(0, 255, 1, 128);
  assert.deepEqual(unbase64(base64(bytes)), bytes);
  for (const bad of ['A', 'AAAAA', 'AA?=', 'AB==', 'AAAA\n']) assert.throws(() => unbase64(bad));
});
test('CRC-16/MODBUS known vector and low-byte-first append', () => {
  const bytes = new TextEncoder().encode('123456789');
  assert.equal(crc16(bytes), 0x4b37);
  assert.deepEqual(finalizeBytes(bytes, { crc: 'modbus' }).slice(-2), Uint8Array.of(0x37, 0x4b));
  const payload = buildPayload({ input: '01 03 00 00 00 02', crc: 'modbus' });
  assert.equal(payload.bytes.length, 6);
  assert.equal(unbase64(payload.data_b64).length, 6, 'firmware must receive bytes before CRC so CRC is not appended twice');
  assert.equal(hex(payload.finalBytes), '01 03 00 00 00 02 C4 0B');
});
test('UTF-8 encoding, line ending rules, final length and bit width', () => {
  assert.equal(hex(buildPayload({ input: '中', mode: 'text', ending: 'crlf' }).bytes), 'E4 B8 AD 0D 0A');
  assert.equal(hex(buildPayload({ input: '01', mode: 'hex', ending: 'crlf' }).bytes), '01');
  assert.equal(buildPayload({ input: 'A'.repeat(1024), mode: 'text' }).finalBytes.length, 1024);
  assert.throws(() => buildPayload({ input: 'A'.repeat(1023), mode: 'text', crc: 'modbus' }), /1024/);
  assert.throws(() => buildPayload({ input: '80', dataBits: 7 }), /7 位/);
  assert.throws(() => buildPayload({ input: '01', dataBits: 7, crc: 'modbus' }), /8 位/);
  assert.throws(() => buildPayload({ input: '', crc: 'modbus' }), /请输入/);
});
test('configuration and repeat limits include serialized UART time and gap', () => {
  const c = validateConfig({ baud_rate: 1200, data_bits: 8, parity: 'E', stop_bits: 2, gap_us: 2000 });
  assert.equal(minimumInterval(100, c), 1002);
  assert.throws(() => validateInterval(1001, 100, c), /1002/);
  assert.equal(validateInterval(1002, 100, c), 1002);
  assert.throws(() => validateInterval(3600001, 1, c));
  assert.throws(() => validateConfig({ ...c, baud_rate: 115201 }));
  assert.throws(() => validateConfig({ ...c, gap_us: -1 }));
  assert.throws(() => validateConfig({ ...c, data_bits: 9 }));
});
test('RX UTF-8 decoder spans chunks and does not cross boot/config boundaries', () => {
  const store = new LogStore();
  store.add({ seq: 1, direction: 'rx', bytes: Uint8Array.of(0xe4), boot_id: 'a', config_revision: 1 });
  store.add({ seq: 2, direction: 'rx', bytes: Uint8Array.of(0xb8, 0xad), boot_id: 'a', config_revision: 1 });
  assert.equal(store.rows().map(row => row.text).join(''), '中');
  store.add({ seq: 3, direction: 'rx', bytes: Uint8Array.of(0xe4), boot_id: 'a', config_revision: 1 });
  store.add({ seq: 4, direction: 'rx', bytes: Uint8Array.of(0xb8, 0xad), boot_id: 'b', config_revision: 1 });
  assert.notEqual(store.rows().at(-1).text, '中');
  assert.deepEqual(store.rows()[0].bytes, Uint8Array.of(0xe4));
});
test('log limits evict oldest records and bound zero-byte system notes', () => {
  const store = new LogStore({ maxBytes: 5, maxRecords: 3 });
  store.add({ seq: 1, direction: 'rx', bytes: Uint8Array.of(1, 2, 3) });
  store.add({ seq: 2, direction: 'tx', bytes: Uint8Array.of(4, 5, 6) });
  assert.equal(store.bytes, 3); assert.equal(store.evicted, 1); assert.equal(store.evictedBytes, 3);
  for (let i = 0; i < 2000; i++) store.note(`note ${i}`);
  assert.equal(store.length, 3); assert.equal(store.bytes, 0); assert.equal(store.tail().length, 3);
  store.clear(); assert.equal(store.length, 0); assert.equal(store.evicted, 0);
});
test('history data length is validated before rendering', () => {
  const store = new LogStore();
  assert.throws(() => store.add({ direction: 'rx', data_b64: 'AA==', length: 2 }), /长度/);
  assert.throws(() => store.add({ direction: 'rx', bytes: new Uint8Array(1025) }), /1024/);
});
test('CSV export preserves raw HEX and escapes formula-leading text', () => {
  const store = new LogStore();
  store.add({ direction: 'rx', bytes: new TextEncoder().encode('=1+1,"x"\r\n'), boot_id: 'a', seq: 1, t_us: 1000 });
  const csv = exportLog(store.rows(), 'csv', 'test');
  assert.ok(csv.startsWith('\uFEFF')); assert.match(csv, /'\=1\+1/); assert.match(csv, /""x""/); assert.match(csv, /3D 31 2B 31/);
  assert.match(exportLog(store.rows(), 'txt'), /HEX 3D 31/);
});
test('macro imports validate length, schema and payload, without retaining extra properties', () => {
  const input = { name: '测试', input: '01 03', mode: 'hex', ending: 'none', crc: 'none', unwanted: true };
  assert.equal(validateMacros([input])[0].unwanted, undefined);
  assert.throws(() => validateMacros(Array(33).fill(input)), /32/);
  assert.throws(() => validateMacros([{ ...input, input: 'invalid' }]), /HEX/);
  assert.throws(() => validateMacros([{ ...input, name: '' }]));
});
