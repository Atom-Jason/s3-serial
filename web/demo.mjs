import { t } from './i18n.mjs';
/* Explicit browser-only simulator. It is instantiated solely for ?demo=1.
 * No network or device I/O is performed by this module. */
import { base64, unbase64, finalizeBytes, validateConfig, validateInterval } from './core.mjs';

const began = performance.now(), clients = new Set();
let clientSequence = 0, sequence = 0, bytesInHistory = 0, currentJob = null, repeatTimer = null, repeatJob = null;
const queue = [], history = [];
const network = { ssid: '', sta_enabled: false, connected: false, sta_ip: '', ap_ssid: 'S3-Serial-DEMO', ap_ip: '192.168.4.1', error: '' };
const serial = { config: { baud_rate: 115200, data_bits: 8, parity: 'N', stop_bits: 1, gap_us: 2000 }, config_revision: 1, owner_id: 0, tx_busy: false, queued: 0, repeat: { active: false, interval_ms: 1000, skipped: 0 }, stats: { rx_bytes: 0, tx_bytes: 0, uart_overflows: 0, frame_errors: 0, parity_errors: 0, history_evicted_bytes: 0 } };
const boot = 'demo-browser-session';
const copy = value => JSON.parse(JSON.stringify(value));
const micros = () => Math.floor((performance.now() - began) * 1000);
function status() { serial.queued = queue.length; return { device: 'S3-Serial-DEMO', boot_id: boot, uptime_ms: Math.floor(performance.now() - began), free_heap: 172840, network: copy(network), serial: copy(serial), controller_id: serial.owner_id, client_count: clients.size }; }
function addHistory(direction, bytes, id = '') {
  const record = { seq: ++sequence, t_us: micros(), direction, data_b64: base64(bytes), length: bytes.length, config_revision: serial.config_revision, ...(id ? { id } : {}) };
  history.push(record); bytesInHistory += bytes.length;
  serial.stats[direction === 'rx' ? 'rx_bytes' : 'tx_bytes'] += bytes.length;
  while (history.length > 512 || bytesInHistory > 32768) { const removed = history.shift(); bytesInHistory -= removed.length; serial.stats.history_evicted_bytes += removed.length; }
  for (const client of clients) client.pump();
}
function broadcast(type, payload) { for (const client of clients) client.emit(type, payload); }
function stopRepeat() { clearInterval(repeatTimer); repeatTimer = null; repeatJob = null; serial.repeat.active = false; }
function releaseOwner() { stopRepeat(); queue.splice(0); serial.queued = 0; serial.owner_id = 0; broadcast('control.changed', { controller_id: 0 }); broadcast('repeat.changed', copy(serial.repeat)); }
function work() {
  if (currentJob || !queue.length) return;
  const job = queue.shift(); serial.queued = queue.length;
  if (job.client.closed || job.owner !== serial.owner_id) { work(); return; }
  currentJob = job; serial.tx_busy = true;
  job.client.emit('tx.started', {}, job.id);
  const c = serial.config, wireMs = job.bytes.length * (1 + c.data_bits + (c.parity === 'N' ? 0 : 1) + c.stop_bits) * 1000 / c.baud_rate;
  setTimeout(() => {
    addHistory('tx', job.bytes, job.id);
    job.client.emit('tx.done', { length: job.bytes.length }, job.id);
    setTimeout(() => addHistory('rx', job.bytes), 35);
    setTimeout(() => { currentJob = null; serial.tx_busy = false; work(); }, c.gap_us / 1000);
  }, Math.max(1, wireMs));
}
function finalPayload(payload) { return finalizeBytes(unbase64(payload.data_b64), { crc: payload.crc || 'none', dataBits: serial.config.data_bits }); }

export class DemoTransport {
  constructor(callbacks) {
    this.callbacks = callbacks; this.closed = false; this.clientId = ++clientSequence; this.cursor = 0; this.heartbeat = performance.now(); clients.add(this);
    setTimeout(() => {
      if (this.closed) return;
      callbacks.onOpen();
      this.emit('hello', { client_id: this.clientId, boot_id: boot, controller_id: serial.owner_id, serial: copy(serial), latest_seq: sequence });
      this.emit('status', status());
    }, 20);
    this.statusTimer = setInterval(() => {
      if (performance.now() - this.heartbeat > 15000) { this.close(); return; }
      this.emit('status', status()); this.pump();
    }, 1000);
  }
  emit(type, payload = {}, id = '') {
    if (this.closed) return;
    queueMicrotask(() => { if (!this.closed) this.callbacks.onMessage({ data: JSON.stringify({ v: 1, type, ...(id ? { id } : {}), payload }) }); });
  }
  pump() {
    if (this.closed) return;
    const records = history.filter(record => record.seq > this.cursor).slice(0, 4);
    if (!records.length) return;
    const oldest = history[0]?.seq || 0;
    const gap = this.cursor > 0 && this.cursor < oldest - 1;
    this.cursor = records.at(-1).seq;
    this.emit('history', { boot_id: boot, records: copy(records), oldest_seq: oldest, latest_seq: sequence, gap, evicted_bytes: serial.stats.history_evicted_bytes });
    if (this.cursor < sequence) setTimeout(() => this.pump(), 20);
  }
  send(raw) {
    if (this.closed) throw new Error(t("m147"));
    let m;
    try {
      if (new TextEncoder().encode(raw).length > 4096) throw new Error(t("m148"));
      m = JSON.parse(raw); const p = m.payload || {}, id = m.id;
      if (m.type === 'heartbeat') { this.heartbeat = performance.now(); this.emit('ack', { ok: true }, id); return; }
      if (m.type === 'history.get') { this.cursor = Math.max(0, Number(p.after_seq) || 0); this.pump(); this.emit('ack', { ok: true }, id); return; }
      if (m.type === 'control.claim') {
        if (serial.owner_id && serial.owner_id !== this.clientId) throw new Error(t("m149"));
        serial.owner_id = this.clientId; this.emit('ack', { ok: true }, id); broadcast('control.changed', { controller_id: this.clientId }); return;
      }
      if (serial.owner_id !== this.clientId) throw new Error(t("m008"));
      if (m.type === 'control.release') { releaseOwner(); this.emit('ack', { ok: true }, id); return; }
      if (m.type === 'uart.configure') {
        if (serial.tx_busy || queue.length || serial.repeat.active) throw new Error(t("m150"));
        serial.config = validateConfig(p); serial.config_revision++; this.emit('ack', { ok: true }, id); this.emit('config.applied', { config: copy(serial.config), config_revision: serial.config_revision }, id); return;
      }
      if (m.type === 'uart.save') { this.emit('ack', { ok: true }, id); this.emit('config.saved', {}, id); return; }
      if (m.type === 'tx.send') {
        const bytes = finalPayload(p);
        if (queue.length >= 4) throw new Error(t("m151"));
        queue.push({ client: this, owner: this.clientId, bytes, id }); this.emit('ack', { ok: true }, id); work(); return;
      }
      if (m.type === 'repeat.start') {
        if (serial.repeat.active) throw new Error(t("m152"));
        const bytes = finalPayload(p), interval = validateInterval(p.interval_ms, bytes.length, serial.config);
        repeatJob = { client: this, owner: this.clientId, bytes, id }; serial.repeat = { active: true, interval_ms: interval, skipped: 0 };
        const tick = () => {
          if (!repeatJob || repeatJob.owner !== serial.owner_id) return;
          if (currentJob || queue.length) { serial.repeat.skipped++; return; }
          queue.push({ ...repeatJob }); work();
        };
        repeatTimer = setInterval(tick, interval); this.emit('ack', { ok: true }, id); broadcast('repeat.changed', copy(serial.repeat)); tick(); return;
      }
      if (m.type === 'repeat.stop') { stopRepeat(); this.emit('ack', { ok: true }, id); broadcast('repeat.changed', copy(serial.repeat)); return; }
      throw new Error(t("m153"));
    } catch (error) { this.emit('error', { code: 'demo_error', message: error.message }, m?.id || ''); }
  }
  close() {
    if (this.closed) return;
    if (serial.owner_id === this.clientId) releaseOwner();
    this.closed = true; clearInterval(this.statusTimer); clients.delete(this); this.callbacks.onClose();
  }
}

export async function demoApi(path, method = 'GET', data = {}) {
  if (path === 'status') return status();
  if (path === 'network' && method === 'GET') return copy(network);
  if (path === 'session' && method === 'DELETE') { for (const client of [...clients]) client.close(); return { ok: true }; }
  if (path === 'session' && method === 'POST') return { ok: true };
  if (['network', 'password'].includes(path) && method === 'PUT') {
    if (!serial.owner_id || data.client_id !== serial.owner_id) throw new Error(t("m008"));
    if (path === 'network') {
      network.ssid = data.ssid || ''; network.sta_enabled = !!data.enabled; network.connected = !!data.enabled; network.sta_ip = data.enabled ? '192.168.1.88' : '';
      broadcast('status', status()); return { ok: true };
    }
    for (const client of [...clients]) client.close(); return { ok: true, reconnect_required: true };
  }
  throw new Error(t("m154"));
}

// Seed a compact sample only in the simulator's private memory.
addHistory('tx', finalizeBytes(Uint8Array.from([1, 3, 0, 0, 0, 2]), { crc: 'modbus' }), 'demo-seed-1');
addHistory('rx', finalizeBytes(Uint8Array.from([1, 3, 4, 0, 0x7b, 0, 0x2d]), { crc: 'modbus' }));
addHistory('tx', new TextEncoder().encode('Hello, S3 Serial!\r\n'), 'demo-seed-2');
addHistory('rx', new TextEncoder().encode('ESP32-S3 ready\r\n'));
