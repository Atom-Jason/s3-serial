import { t } from './i18n.mjs';
export const LIMITS = Object.freeze({ txBytes: 1024, logBytes: 5 * 1024 * 1024, logRecords: 20000, macros: 32, domRows: 1000 });

export function parseHex(value) {
  const compact = String(value).replace(/\s/g, '');
  if (!/^[0-9a-f]*$/i.test(compact)) throw new Error(t("m122"));
  if (compact.length % 2) throw new Error(t("m123"));
  return Uint8Array.from(compact.match(/.{2}/g) || [], pair => Number.parseInt(pair, 16));
}

export function hex(bytes) { return Array.from(bytes, byte => byte.toString(16).padStart(2, '0').toUpperCase()).join(' '); }
export function base64(bytes) {
  let binary = '';
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary);
}
export function unbase64(value) {
  if (typeof value !== 'string' || value.length % 4 !== 0 || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(value)) throw new Error(t("m124"));
  const bytes = Uint8Array.from(atob(value), ch => ch.charCodeAt(0));
  if (base64(bytes) !== value) throw new Error(t("m125"));
  return bytes;
}
export function crc16(bytes) {
  let crc = 0xffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) crc = (crc & 1) ? (crc >>> 1) ^ 0xa001 : crc >>> 1;
  }
  return crc;
}
export function finalizeBytes(bytes, { crc = 'none', dataBits = 8 } = {}) {
  if (![5, 6, 7, 8].includes(dataBits)) throw new Error(t("m126"));
  if (!['none', 'modbus'].includes(crc)) throw new Error(t("m127"));
  if (!bytes.length) throw new Error(t("m128"));
  if (bytes.some(byte => byte >= 2 ** dataBits)) throw new Error(t("m129", dataBits));
  if (crc === 'modbus' && dataBits !== 8) throw new Error(t("m130"));
  const result = new Uint8Array(bytes.length + (crc === 'modbus' ? 2 : 0));
  result.set(bytes);
  if (crc === 'modbus') {
    const sum = crc16(bytes);
    result.set([sum & 255, sum >>> 8], bytes.length);
  }
  if (result.length > LIMITS.txBytes) throw new Error(t("m131", result.length, LIMITS.txBytes));
  return result;
}
export function buildPayload({ input, mode = 'hex', ending = 'none', crc = 'none', dataBits = 8 }) {
  if (!['hex', 'text'].includes(mode)) throw new Error(t("m132"));
  if (!['none', 'cr', 'lf', 'crlf'].includes(ending)) throw new Error(t("m133"));
  const suffix = { none: '', cr: '\r', lf: '\n', crlf: '\r\n' };
  const bytes = mode === 'hex' ? parseHex(input) : new TextEncoder().encode(String(input) + suffix[ending]);
  return { bytes, finalBytes: finalizeBytes(bytes, { crc, dataBits }), data_b64: base64(bytes), crc };
}
export function validateConfig(config) {
  const c = { baud_rate: Number(config.baud_rate), data_bits: Number(config.data_bits), parity: config.parity, stop_bits: Number(config.stop_bits), gap_us: Number(config.gap_us) };
  if (!Number.isInteger(c.baud_rate) || c.baud_rate < 1200 || c.baud_rate > 115200) throw new Error(t("m134"));
  if (![5, 6, 7, 8].includes(c.data_bits)) throw new Error(t("m135"));
  if (!['N', 'E', 'O'].includes(c.parity)) throw new Error(t("m136"));
  if (![1, 2].includes(c.stop_bits)) throw new Error(t("m137"));
  if (!Number.isInteger(c.gap_us) || c.gap_us < 0 || c.gap_us > 100000) throw new Error(t("m138"));
  return c;
}
export function minimumInterval(length, config) {
  const c = validateConfig(config);
  return Math.max(20, Math.ceil((length * (1 + c.data_bits + (c.parity === 'N' ? 0 : 1) + c.stop_bits) * 1000000 / c.baud_rate + c.gap_us) / 1000));
}
export function validateInterval(value, length, config) {
  const interval = Number(value), min = minimumInterval(length, config);
  if (!Number.isInteger(interval) || interval < min || interval > 3600000) throw new Error(t("m139", min));
  return interval;
}
export function formatBytes(value) {
  const count = Number(value) || 0;
  if (count < 1024) return `${count} B`;
  if (count < 1024 * 1024) return `${(count / 1024).toFixed(1)} KiB`;
  return `${(count / 1024 / 1024).toFixed(2)} MiB`;
}
export function formatTime(us) {
  const seconds = Math.max(0, Number(us) || 0) / 1000000;
  return `${Math.floor(seconds / 3600).toString().padStart(2, '0')}:${Math.floor(seconds / 60 % 60).toString().padStart(2, '0')}:${Math.floor(seconds % 60).toString().padStart(2, '0')}.${Math.floor(seconds * 1000 % 1000).toString().padStart(3, '0')}`;
}
export function visibleText(value) { return value.replace(/\r/g, '␍').replace(/\n/g, '␊\n').replace(/\t/g, '⇥').replace(/\0/g, '␀').replace(/[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]/g, c => `\\x${c.charCodeAt(0).toString(16).padStart(2, '0')}`); }

export class LogStore {
  constructor({ maxBytes = LIMITS.logBytes, maxRecords = LIMITS.logRecords } = {}) {
    this.maxBytes = maxBytes; this.maxRecords = maxRecords; this.clear();
  }
  clear() { this.entries = []; this.head = 0; this.bytes = 0; this.evicted = 0; this.evictedBytes = 0; this.serial = 0; this.resetDecoder(); }
  resetDecoder() { this.decoder = new TextDecoder('utf-8'); this.rxBoundary = ''; }
  get length() { return this.entries.length - this.head; }
  add(record) {
    const bytes = record.bytes instanceof Uint8Array ? record.bytes.slice() : unbase64(record.data_b64);
    if (bytes.length > LIMITS.txBytes) throw new Error(t("m140"));
    if (record.length !== undefined && record.length !== bytes.length) throw new Error(t("m141"));
    const boundary = `${record.boot_id || ''}:${record.config_revision || 0}`;
    if (record.direction === 'rx' && this.rxBoundary !== boundary) { this.decoder = new TextDecoder('utf-8'); this.rxBoundary = boundary; }
    const text = record.direction === 'rx' ? this.decoder.decode(bytes, { stream: true }) : new TextDecoder().decode(bytes);
    const item = { ...record, bytes, text, hex: hex(bytes), host_time: record.host_time || new Date().toISOString(), local_id: ++this.serial };
    this.entries.push(item); this.bytes += bytes.length; this.trim(); return item;
  }
  note(text, boot_id = '') {
    const item = { direction: 'system', text, bytes: new Uint8Array(), hex: '', t_us: null, seq: null, boot_id, host_time: new Date().toISOString(), local_id: ++this.serial };
    this.entries.push(item); this.trim(); return item;
  }
  trim() {
    while (this.length && (this.bytes > this.maxBytes || this.length > this.maxRecords)) {
      const removed = this.entries[this.head++]; this.bytes -= removed.bytes.length; this.evicted++; this.evictedBytes += removed.bytes.length;
    }
    if (this.head > 1024 && this.head > this.entries.length / 2) { this.entries = this.entries.slice(this.head); this.head = 0; }
  }
  rows() { return this.entries.slice(this.head); }
  tail(limit = LIMITS.domRows, direction = 'all') {
    const result = [];
    for (let i = this.entries.length - 1; i >= this.head && result.length < limit; i--) {
      const row = this.entries[i];
      if (direction === 'all' || row.direction === direction || row.direction === 'system') result.push(row);
    }
    return result.reverse();
  }
}

function csvCell(value) {
  let text = value == null ? '' : String(value);
  if (/^[\s]*[=+\-@]/.test(text)) text = `'${text}`;
  return `"${text.replace(/"/g, '""')}"`;
}
export function exportLog(records, format, device = 'S3-Serial') {
  if (format === 'csv') {
    const rows = [['device', 'boot_id', 'seq', 'device_us', 'host_time', 'direction', 'config_revision', 'request_id', 'hex', 'text']];
    for (const r of records) rows.push([device, r.boot_id, r.seq, r.t_us, r.host_time, r.direction, r.config_revision, r.id, r.hex, r.text]);
    return '\uFEFF' + rows.map(row => row.map(csvCell).join(',')).join('\r\n');
  }
  return [t("m142", device), t("m143"), ...records.map(r => `${r.host_time} | boot=${r.boot_id || '-'} seq=${r.seq ?? '-'} | ${r.t_us == null ? '-' : formatTime(r.t_us)} | ${r.direction.toUpperCase()} | HEX ${r.hex || '-'} | ${visibleText(r.text).replace(/\n/g, '\\n')}`)].join('\n');
}
export function validateMacros(value) {
  if (!Array.isArray(value) || value.length > LIMITS.macros) throw new Error(t("m144", LIMITS.macros));
  return value.map((item, index) => {
    if (!item || typeof item !== 'object' || typeof item.name !== 'string' || !item.name.trim() || item.name.length > 48) throw new Error(t("m145", index + 1));
    if (typeof item.input !== 'string' || item.input.length > 4096 || !['hex', 'text'].includes(item.mode) || !['none', 'cr', 'lf', 'crlf'].includes(item.ending) || !['none', 'modbus'].includes(item.crc)) throw new Error(t("m146", index + 1));
    buildPayload({ ...item, dataBits: 8 });
    return { name: item.name.trim(), input: item.input, mode: item.mode, ending: item.ending, crc: item.crc };
  });
}
