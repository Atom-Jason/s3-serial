import { t, getLocale, setLocale, loadLocale, translatePage, describeMessage, renderMessage } from './i18n.mjs';
setLocale(loadLocale());
import { LIMITS, LogStore, buildPayload, validateConfig, validateInterval, minimumInterval, formatBytes, formatTime, visibleText, exportLog, validateMacros } from './core.mjs';
import { DemoTransport, demoApi } from './demo.mjs';

const $ = id => document.getElementById(id);
const demo = new URLSearchParams(location.search).get('demo') === '1';
const initialConfig = { baud_rate: 115200, data_bits: 8, parity: 'N', stop_bits: 1, gap_us: 2000 };
const state = { connected: false, logged: false, clientId: 0, controllerId: 0, boot: '', lastSeq: 0, device: 'S3-Serial', serial: { config: initialConfig, repeat: { active: false, interval_ms: 1000, skipped: 0 }, stats: {}, queued: 0, tx_busy: false }, network: {}, inputMode: 'hex', paused: false, dirty: true, configDirty: false, retry: 0 };
const log = new LogStore();
const pending = new Map(), outcomes = new Map(), rendered = new Map();
let transport, heartbeatTimer, reconnectTimer, generation = 0, requestCounter = 0;
let macros = [], forceLog = false;
const errorNames = { unauthorized: "m001", login_required: "m002", invalid_password: "m003", login_rate_limited: "m004", forbidden: "m005", not_controller: "m006", controller_busy: "m007", no_control: "m008", not_owner: "m006", busy: "m009", device_busy: "m010", queue_full: "m011", invalid_argument: "m012", invalid_parameter: "m012", invalid_config: "m013", invalid_interval: "m014", invalid_crc: "m015", invalid_base64: "m016", empty_data: "m017", too_large: "m018", byte_out_of_range: "m019", crc_requires_8_bits: "m020", not_ready: "m021", invalid_payload: "m022", tx_timeout: "m023", write_failed: "m024", partial_write: "m025", configure_failed: "m026", save_failed: "m027", too_many_clients: "m028", subnet_conflict: "m029", already_owned: "m007" };

function explain(error) { return t(errorNames[error]) || error || t("m030"); }
function notify(message, kind = '') { $('notice').textContent = message; $('notice').className = `notice ${kind}`; $('notice').hidden = !message; }
function showDialog(element) { if (!element.open) element.showModal(); }
function note(message) { log.note(message, state.boot); state.dirty = true; }
function isController() { return state.connected && state.clientId !== 0 && state.clientId === state.controllerId; }
function configText(c = state.serial.config) { return `${c.baud_rate} · ${c.data_bits}${c.parity}${c.stop_bits}`; }
function persist(key, value) { try { localStorage.setItem(`s3serial.${key}.v1`, JSON.stringify(value)); return true; } catch { notify(t("m031")); return false; } }
function restore(key, fallback) { try { return JSON.parse(localStorage.getItem(`s3serial.${key}.v1`)) ?? fallback; } catch { return fallback; } }
function cleanRecordId(id) { return typeof id === 'string' && id.length <= 40 ? id : ''; }

async function api(path, method = 'GET', data) {
  if (demo) return demoApi(path, method, data);
  const response = await fetch(`/api/v1/${path}`, { method, credentials: 'same-origin', cache: 'no-store', headers: data === undefined ? {} : { 'Content-Type': 'application/json' }, body: data === undefined ? undefined : JSON.stringify(data) });
  let result;
  try { result = await response.json(); } catch { throw new Error(t("m032", response.status)); }
  if (!response.ok) { const error = new Error(t(errorNames[result.error]) || result.message || explain(result.error) || `HTTP ${response.status}`); error.status = response.status; throw error; }
  return result;
}

function markPendingUnknown() {
  for (const [id, item] of pending) {
    if (item.type === 'tx.send') setOutcome(id, 'unknown', t("m033"));
  }
  pending.clear();
}
function sendCommand(type, payload = {}, track = true) {
  if (!state.connected || !transport) throw new Error(t("m034"));
  if (!['heartbeat', 'control.claim', 'history.get'].includes(type) && !isController()) throw new Error(t("m008"));
  const id = `r${Date.now().toString(36)}-${(++requestCounter).toString(36)}`;
  if (track && type !== 'heartbeat' && type !== 'history.get') {
    if (pending.size >= 64) throw new Error(t("m035"));
    pending.set(id, { type, created: Date.now() });
  }
  try { transport.send(JSON.stringify({ v: 1, type, id, payload })); }
  catch (error) { pending.delete(id); throw error; }
  if (type === 'tx.send') setOutcome(id, '', t("m036"));
  return id;
}
function setOutcome(id, status, label) {
  if (!id) return;
  outcomes.set(id, { status, label });
  while (outcomes.size > 12) outcomes.delete(outcomes.keys().next().value);
  const fragment = document.createDocumentFragment();
  for (const [key, value] of outcomes) {
    const item = document.createElement('span'); item.className = `outcome ${value.status}`; item.textContent = `${key.slice(-5)} · ${value.label}`; item.title = key; fragment.append(item);
  }
  $('tx-outcomes').replaceChildren(fragment);
}
function handleBoot(id) {
  if (typeof id !== 'string' || !id || state.boot === id) return;
  const old = state.boot;
  state.boot = id; state.lastSeq = 0; log.resetDecoder();
  if (old) { markPendingUnknown(); note(t("m037")); }
}
function applySerial(serial, { forceConfig = false } = {}) {
  if (!serial || typeof serial !== 'object') return;
  const previousConfig = state.serial.config;
  state.serial = { ...state.serial, ...serial, config: { ...previousConfig, ...(serial.config || {}) }, repeat: { ...state.serial.repeat, ...(serial.repeat || {}) }, stats: { ...state.serial.stats, ...(serial.stats || {}) } };
  if (!state.configDirty || forceConfig) fillConfig(state.serial.config);
  else $('config-state').textContent = t("m038", configText());
  refreshControls(); preview();
}
function applyStatus(status) {
  if (!status || typeof status !== 'object') return;
  handleBoot(status.boot_id);
  if (typeof status.firmware === 'string') $('firmware-version').textContent = `v${status.firmware}`;
  if (typeof status.device === 'string') state.device = status.device;
  else if (status.device && typeof status.device.name === 'string') state.device = status.device.name;
  if (Number.isInteger(status.controller_id)) state.controllerId = status.controller_id;
  if (status.network) state.network = { ...state.network, ...status.network };
  applySerial(status.serial);
  $('device-name').textContent = state.device;
  const stats = state.serial.stats;
  $('rx-count').textContent = formatBytes(stats.rx_bytes);
  $('tx-count').textContent = formatBytes(stats.tx_bytes);
  const overflows = Number(stats.uart_overflows) || 0, frame = Number(stats.frame_errors) || 0, parity = Number(stats.parity_errors) || 0;
  $('error-count').textContent = String(overflows + frame + parity);
  $('error-count').classList.toggle('error-text', overflows + frame + parity > 0);
  $('error-detail').textContent = t("m039", overflows, frame, parity);
  if (status.uptime_ms !== undefined) $('uptime').textContent = formatTime(status.uptime_ms * 1000).split('.')[0];
  if (status.free_heap !== undefined) { state.freeHeap = status.free_heap; $('memory').textContent = t("m040", formatBytes(status.free_heap)); }
  const network = state.network;
  $('network-summary').textContent = network.connected ? t("m041", network.ap_ip || '192.168.4.1', network.sta_ip || t("m042")) : t("m043", network.ap_ip || '192.168.4.1');
  refreshControls();
}
function handleMessage(event) {
  let message;
  try { message = typeof event.data === 'string' ? JSON.parse(event.data) : event; }
  catch { notify(t("m044"), 'error'); return; }
  if (!message || message.v !== 1 || typeof message.type !== 'string' || !message.payload || typeof message.payload !== 'object') return;
  const p = message.payload, id = cleanRecordId(message.id), item = pending.get(id);
  switch (message.type) {
    case 'hello':
      state.connected = true; state.logged = true; state.retry = 0; state.clientId = Number(p.client_id) || 0; state.controllerId = Number(p.controller_id) || 0;
      handleBoot(p.boot_id); applySerial(p.serial); refreshControls();
      if ($('login-dialog').open) $('login-dialog').close();
      note(demo ? t("m045") : t("m046"));
      clearInterval(heartbeatTimer); heartbeatTimer = setInterval(() => { try { sendCommand('heartbeat', {}, false); } catch { /* close event drives reconnect */ } }, 5000);
      try { sendCommand('history.get', { after_seq: state.lastSeq }, false); } catch (error) { notify(error.message, 'error'); }
      break;
    case 'status': applyStatus(p); break;
    case 'control.changed': {
      const previousOwner = isController(); state.controllerId = Number(p.controller_id) || 0;
      if (previousOwner && !isController()) { markPendingUnknown(); note(t("m047")); }
      refreshControls(); break;
    }
    case 'history': {
      handleBoot(p.boot_id);
      if (!Array.isArray(p.records)) break;
      if (p.gap) { note(t("m048")); log.resetDecoder(); }
      for (const record of p.records) {
        if (!record || !Number.isInteger(record.seq) || record.seq <= state.lastSeq || !['rx', 'tx'].includes(record.direction)) continue;
        if (state.lastSeq && record.seq > state.lastSeq + 1) { note(t("m049", state.lastSeq + 1, record.seq - 1)); log.resetDecoder(); }
        try { log.add({ ...record, boot_id: state.boot }); state.lastSeq = record.seq; }
        catch (error) { note(t("m050", error.message)); state.lastSeq = record.seq; log.resetDecoder(); }
      }
      state.dirty = true; break;
    }
    case 'ack':
      if (item?.type === 'tx.send') setOutcome(id, '', t("m051"));
      else if (item && !['uart.configure', 'uart.save'].includes(item.type)) pending.delete(id);
      break;
    case 'error':
    case 'command.failed':
      notify(t(errorNames[p.code]) || p.message || explain(p.code), 'error');
      if (item?.type === 'tx.send') setOutcome(id, 'failed', t("m052"));
      pending.delete(id); refreshControls(); break;
    case 'tx.started': setOutcome(id, '', t("m053")); break;
    case 'tx.done': setOutcome(id, 'done', t("m054")); pending.delete(id); break;
    case 'tx.failed': setOutcome(id, 'failed', t("m055")); pending.delete(id); notify(t(errorNames[p.code]) || p.message || explain(p.code) || t("m056"), 'error'); break;
    case 'config.applied':
      pending.delete(id); state.configDirty = false; applySerial(p.config ? p : { config: p }, { forceConfig: true });
      note(t("m057", configText())); notify(t("m058"), 'success'); break;
    case 'config.saved': pending.delete(id); notify(t("m059"), 'success'); break;
    case 'repeat.changed': applySerial({ repeat: p.repeat || p }); pending.delete(id); break;
    case 'data.gap': note(p.message || t("m060")); log.resetDecoder(); break;
    default: break;
  }
}

function refreshControls() {
  const owner = isController(), connected = state.connected;
  $('connection-badge').className = `badge${connected ? '' : ' muted'}`;
  $('connection-badge').replaceChildren();
  const dot = document.createElement('i'); $('connection-badge').append(dot, document.createTextNode(connected ? (demo ? t("m061") : t("m062")) : t("m063")));
  $('control-button').disabled = !connected || (!!state.controllerId && !owner);
  $('control-button').textContent = owner ? t("m064") : state.controllerId ? t("m065") : t("m066");
  $('control-button').className = `button ${owner ? '' : 'primary'}`;
  $('control-status').textContent = !connected ? t("m067") : owner ? t("m068") : state.controllerId ? t("m069") : t("m070");
  $('logout').hidden = !state.logged;
  $('active-config').textContent = configText();
  const busy = state.serial.tx_busy || state.serial.queued > 0 || state.serial.repeat.active;
  $('uart-apply').disabled = !owner || busy;
  $('uart-save').disabled = !owner || busy;
  $('network-save').disabled = !owner;
  $('password-save').disabled = !owner;
  $('queue-state').textContent = t("m071", Number(state.serial.queued) || 0, state.serial.tx_busy ? t("m072") : '');
  const repeat = state.serial.repeat;
  $('repeat-toggle').textContent = repeat.active ? t("m073") : t("m074");
  $('repeat-toggle').className = `button${repeat.active ? ' danger' : ''}`;
  $('repeat-interval').disabled = !!repeat.active;
  $('repeat-state').textContent = repeat.active ? t("m075", repeat.interval_ms, repeat.skipped || 0) : t("m076");
  preview();
}
function syncBaudPreset() {
  const custom = $('baud-preset').value === 'custom';
  $('custom-baud-field').hidden = !custom;
  $('baud-rate').disabled = !custom;
  if (!custom) $('baud-rate').value = $('baud-preset').value;
}
function fillConfig(config) {
  const baud = String(config.baud_rate);
  $('baud-preset').value = [...$('baud-preset').options].some(option => option.value === baud) ? baud : 'custom';
  $('baud-rate').value = config.baud_rate; $('data-bits').value = config.data_bits; $('parity').value = config.parity; $('stop-bits').value = config.stop_bits; $('gap-us').value = config.gap_us;
  syncBaudPreset();
  state.configDirty = false;
  $('config-state').textContent = t("m077", configText(config), config.gap_us);
}
function currentInput() { return { input: $('send-input').value, mode: state.inputMode, ending: $('ending').value, crc: $('crc-enabled').checked ? 'modbus' : 'none', dataBits: Number(state.serial.config.data_bits) }; }
function preview() {
  $('ending').disabled = state.inputMode !== 'text';
  try {
    const payload = buildPayload(currentInput());
    $('send-summary').textContent = t("m078", payload.finalBytes.length);
    $('payload-preview').textContent = Array.from(payload.finalBytes, byte => byte.toString(16).padStart(2, '0').toUpperCase()).join(' ');
    $('send-validation').textContent = t("m079", payload.finalBytes.length, minimumInterval(payload.finalBytes.length, state.serial.config), payload.crc === 'modbus' ? t("m080") : '');
    $('send-validation').className = 'helper';
    $('send-once').disabled = !isController();
    let intervalValid = true;
    try { validateInterval($('repeat-interval').value, payload.finalBytes.length, state.serial.config); } catch { intervalValid = false; }
    $('repeat-toggle').disabled = !isController() || (!state.serial.repeat.active && !intervalValid);
    return payload;
  } catch (error) {
    $('send-summary').textContent = t("m081"); $('payload-preview').textContent = '—';
    $('send-validation').textContent = error.message;
    $('send-validation').className = `helper${$('send-input').value ? ' error' : ''}`;
    $('send-once').disabled = true; $('repeat-toggle').disabled = !isController() || !state.serial.repeat.active;
    return null;
  }
}
function setInputMode(mode) {
  state.inputMode = mode;
  for (const name of ['hex', 'text']) { $(`mode-${name}`).classList.toggle('selected', mode === name); $(`mode-${name}`).setAttribute('aria-pressed', String(mode === name)); }
  $('send-input').placeholder = mode === 'hex' ? t("m082") : t("m083");
  preview(); savePreferences();
}
function savePreferences() { persist('preferences', { inputMode: state.inputMode, ending: $('ending').value, crc: $('crc-enabled').checked, view: $('view-mode').value, autoScroll: $('auto-scroll').checked }); }

function renderRow(record) {
  const row = document.createElement('div'); row.className = `log-row ${record.direction}`; row.dataset.id = record.local_id;
  const time = document.createElement('span'); time.className = 'log-time'; time.textContent = record.t_us == null ? '—' : formatTime(record.t_us); time.title = `${record.host_time} · seq ${record.seq ?? '—'} · boot ${record.boot_id || '—'}`;
  const chip = document.createElement('span'); chip.className = `direction-chip ${record.direction}`; chip.textContent = record.direction === 'system' ? 'SYS' : record.direction.toUpperCase();
  const data = document.createElement('div'); data.className = 'log-data';
  if (record.direction === 'system') data.textContent = record.text;
  else {
    const mode = $('view-mode').value;
    if (mode !== 'text') { const line = document.createElement('div'); line.className = 'hex-line'; line.textContent = record.hex; data.append(line); }
    if (mode !== 'hex') { const line = document.createElement('div'); line.className = mode === 'both' ? 'text-line' : ''; line.textContent = record.text ? visibleText(record.text) : t("m084"); data.append(line); }
  }
  row.append(time, chip, data); return row;
}
function renderLog() {
  $('log-count').textContent = t("m085", log.length.toLocaleString(), formatBytes(log.bytes), state.paused ? t("m086") : '');
  $('log-count').classList.toggle('paused-tag', state.paused);
  $('log-limit').textContent = log.evicted ? t("m087", log.evicted.toLocaleString(), formatBytes(log.evictedBytes)) : t("m088");
  if (state.paused) return;
  const rows = log.tail(LIMITS.domRows, $('direction-filter').value), desired = new Set(rows.map(row => row.local_id));
  if (forceLog) { rendered.clear(); $('log-rows').replaceChildren(); forceLog = false; }
  for (const [id, element] of rendered) if (!desired.has(id)) { element.remove(); rendered.delete(id); }
  const fragment = document.createDocumentFragment();
  for (const record of rows) if (!rendered.has(record.local_id)) { const element = renderRow(record); rendered.set(record.local_id, element); fragment.append(element); }
  $('log-rows').append(fragment);
  $('log-empty').hidden = rows.length > 0;
  if ($('auto-scroll').checked) $('log-viewport').scrollTop = $('log-viewport').scrollHeight;
}
function renderMacros() {
  $('macro-count').textContent = `${macros.length} / ${LIMITS.macros}`;
  $('macro-list').replaceChildren();
  if (!macros.length) { const empty = document.createElement('p'); empty.className = 'empty-small'; empty.textContent = t("m089"); $('macro-list').append(empty); }
  macros.forEach((macro, index) => {
    const row = document.createElement('div'); row.className = 'macro-item';
    const use = document.createElement('button'); use.className = 'macro-use'; use.type = 'button';
    const name = document.createElement('strong'); name.textContent = macro.name;
    const sample = document.createElement('small'); sample.textContent = `${macro.mode.toUpperCase()} · ${macro.input.slice(0, 60)}`; use.append(name, sample);
    use.addEventListener('click', () => { $('send-input').value = macro.input; $('ending').value = macro.ending; $('crc-enabled').checked = macro.crc === 'modbus'; setInputMode(macro.mode); $('send-input').focus(); });
    const remove = document.createElement('button'); remove.className = 'icon-button'; remove.type = 'button'; remove.textContent = '×'; remove.setAttribute('aria-label', t("m090", macro.name)); remove.addEventListener('click', () => { macros.splice(index, 1); persist('macros', macros); renderMacros(); });
    row.append(use, remove); $('macro-list').append(row);
  });
}
function download(name, content, type) {
  const url = URL.createObjectURL(new Blob([content], { type })); const anchor = document.createElement('a'); anchor.href = url; anchor.download = name; document.body.append(anchor); anchor.click(); anchor.remove(); setTimeout(() => URL.revokeObjectURL(url), 30000);
}

function disconnected() {
  const wasConnected = state.connected;
  state.connected = false; state.clientId = 0; state.controllerId = 0; state.serial.repeat.active = false;
  clearInterval(heartbeatTimer); markPendingUnknown(); refreshControls();
  if (wasConnected) { note(t("m091")); log.resetDecoder(); }
}
function scheduleReconnect() {
  if (!state.logged || reconnectTimer) return;
  const delay = [1000, 2000, 5000, 10000][Math.min(state.retry++, 3)];
  notify(t("m092", delay / 1000));
  reconnectTimer = setTimeout(() => { reconnectTimer = null; probeAndConnect(); }, delay);
}
function connectSocket() {
  const current = ++generation;
  clearTimeout(reconnectTimer); reconnectTimer = null; clearInterval(heartbeatTimer);
  if (transport) transport.close();
  const callbacks = {
    onMessage: event => { if (current === generation) handleMessage(event); },
    onClose: () => { if (current !== generation) return; disconnected(); scheduleReconnect(); },
    onOpen: () => { if (current === generation) notify(demo ? t("m093") : '', demo ? '' : 'success'); }
  };
  if (demo) transport = new DemoTransport(callbacks);
  else {
    const socket = new WebSocket(`${location.protocol === 'https:' ? 'wss:' : 'ws:'}//${location.host}/api/v1/ws`);
    transport = socket;
    socket.addEventListener('open', callbacks.onOpen); socket.addEventListener('message', callbacks.onMessage); socket.addEventListener('close', callbacks.onClose);
    socket.addEventListener('error', () => { if (current === generation) notify(t("m094"), 'error'); });
  }
}
async function probeAndConnect() {
  try {
    const status = await api('status'); state.logged = true; applyStatus(status); connectSocket();
  } catch (error) {
    if (error.status === 401 || error.status === 403) { state.logged = false; disconnected(); showDialog($('login-dialog')); }
    else { notify(error.message || t("m095"), 'error'); scheduleReconnect(); }
    refreshControls();
  }
}
function stopConnection() { ++generation; clearInterval(heartbeatTimer); clearTimeout(reconnectTimer); reconnectTimer = null; if (transport) transport.close(); transport = null; disconnected(); }

$('login-form').addEventListener('submit', async event => {
  event.preventDefault(); $('login-error').textContent = ''; $('login-submit').disabled = true;
  try { await api('session', 'POST', { password: $('login-password').value }); $('login-password').value = ''; state.logged = true; $('login-dialog').close(); await probeAndConnect(); }
  catch (error) { $('login-error').textContent = error.message; }
  finally { $('login-submit').disabled = false; }
});
$('login-close').addEventListener('click', () => $('login-dialog').close());
$('reconnect').addEventListener('click', () => { if (state.logged) { stopConnection(); probeAndConnect(); } else showDialog($('login-dialog')); });
$('logout').addEventListener('click', async () => {
  try { await api('session', 'DELETE'); } catch (error) { notify(t("m096", error.message), 'error'); }
  state.logged = false; stopConnection(); showDialog($('login-dialog'));
});
$('control-button').addEventListener('click', () => { try { sendCommand(isController() ? 'control.release' : 'control.claim'); } catch (error) { notify(error.message, 'error'); } });
$('baud-preset').addEventListener('change', syncBaudPreset);
$('uart-form').addEventListener('input', () => { state.configDirty = true; $('config-state').textContent = t("m038", configText()); });
$('uart-form').addEventListener('submit', event => {
  event.preventDefault();
  try { const config = validateConfig({ baud_rate: $('baud-rate').value, data_bits: $('data-bits').value, parity: $('parity').value, stop_bits: $('stop-bits').value, gap_us: $('gap-us').value }); sendCommand('uart.configure', config); notify(t("m097")); }
  catch (error) { notify(error.message, 'error'); }
});
$('uart-save').addEventListener('click', () => { try { sendCommand('uart.save'); notify(t("m098")); } catch (error) { notify(error.message, 'error'); } });
for (const mode of ['hex', 'text']) $(`mode-${mode}`).addEventListener('click', () => setInputMode(mode));
for (const id of ['send-input', 'repeat-interval', 'ending', 'crc-enabled']) $(id).addEventListener('input', () => { preview(); if (['ending', 'crc-enabled'].includes(id)) savePreferences(); });
$('send-once').addEventListener('click', () => { try { const payload = buildPayload(currentInput()); sendCommand('tx.send', { data_b64: payload.data_b64, crc: payload.crc }); } catch (error) { notify(error.message, 'error'); } });
$('send-input').addEventListener('keydown', event => { if (event.key === 'Enter' && (event.ctrlKey || event.metaKey) && !$('send-once').disabled) { event.preventDefault(); $('send-once').click(); } });
$('repeat-toggle').addEventListener('click', () => {
  try {
    if (state.serial.repeat.active) sendCommand('repeat.stop');
    else { const payload = buildPayload(currentInput()), interval_ms = validateInterval($('repeat-interval').value, payload.finalBytes.length, state.serial.config); sendCommand('repeat.start', { data_b64: payload.data_b64, crc: payload.crc, interval_ms }); notify(t("m099")); }
  } catch (error) { notify(error.message, 'error'); }
});
$('pause-log').addEventListener('click', () => { state.paused = !state.paused; $('pause-log').textContent = state.paused ? t("m100") : t("m101"); $('pause-log').classList.toggle('danger', state.paused); state.dirty = true; });
$('clear-log').addEventListener('click', () => { log.clear(); forceLog = true; state.dirty = true; });
for (const id of ['view-mode', 'direction-filter']) $(id).addEventListener('change', () => { forceLog = true; state.dirty = true; savePreferences(); });
$('auto-scroll').addEventListener('change', () => { state.dirty = true; savePreferences(); });
for (const type of ['txt', 'csv']) $(`export-${type}`).addEventListener('click', () => { const stamp = new Date().toISOString().replace(/[:.]/g, '-'); download(`s3-serial-${demo ? 'DEMO-' : ''}${stamp}.${type}`, exportLog(log.rows(), type, state.device), type === 'csv' ? 'text/csv;charset=utf-8' : 'text/plain;charset=utf-8'); });
$('macro-add').addEventListener('click', () => {
  try { buildPayload(currentInput()); if (macros.length >= LIMITS.macros) throw new Error(t("m102")); $('macro-error').textContent = ''; $('macro-name').value = ''; showDialog($('macro-dialog')); $('macro-name').focus(); }
  catch (error) { notify(error.message, 'error'); }
});
$('macro-close').addEventListener('click', () => $('macro-dialog').close());
$('macro-form').addEventListener('submit', event => {
  event.preventDefault();
  try { const input = currentInput(), next = validateMacros([...macros, { name: $('macro-name').value, input: input.input, mode: input.mode, ending: input.ending, crc: input.crc }]); macros = next; persist('macros', macros); renderMacros(); $('macro-dialog').close(); }
  catch (error) { $('macro-error').textContent = error.message; }
});
$('macro-export').addEventListener('click', () => download('s3-serial-commands.json', JSON.stringify(macros, null, 2), 'application/json'));
$('macro-import').addEventListener('click', () => $('macro-file').click());
$('macro-file').addEventListener('change', async event => {
  const file = event.target.files?.[0]; event.target.value = ''; if (!file) return;
  try { if (file.size > 160 * 1024) throw new Error(t("m103")); const imported = validateMacros(JSON.parse(await file.text())); if (macros.length && !window.confirm(t("m104", imported.length, macros.length))) return; macros = imported; persist('macros', macros); renderMacros(); notify(t("m105", macros.length), 'success'); }
  catch (error) { notify(t("m106", error.message), 'error'); }
});
$('network-open').addEventListener('click', async () => {
  if (!state.logged) { showDialog($('login-dialog')); return; }
  $('network-error').textContent = ''; $('sta-password').value = ''; $('new-password').value = ''; $('confirm-password').value = '';
  showDialog($('network-dialog'));
  try {
    const network = await api('network'); state.network = network;
    $('sta-enabled').checked = !!network.sta_enabled; $('sta-ssid').value = network.ssid || '';
    $('network-addresses').textContent = `${network.ap_ssid || t("m107")} · ${network.ap_ip || '192.168.4.1'}`;
    $('network-state').textContent = network.error ? explain(network.error) : network.connected ? t("m108", network.sta_ip) : t("m109");
    refreshControls();
  } catch (error) { $('network-error').textContent = error.message; }
});
$('network-close').addEventListener('click', () => $('network-dialog').close());
$('network-form').addEventListener('submit', async event => {
  event.preventDefault(); $('network-error').textContent = '';
  try {
    if (!isController()) throw new Error(t("m110"));
    const ssid = $('sta-ssid').value, password = $('sta-password').value, enabled = $('sta-enabled').checked;
    if (enabled && (!ssid.trim() || new TextEncoder().encode(ssid).length > 32)) throw new Error(t("m111"));
    if (enabled && password && (new TextEncoder().encode(password).length < 8 || new TextEncoder().encode(password).length > 63)) throw new Error(t("m112"));
    $('network-save').disabled = true; await api('network', 'PUT', { client_id: state.clientId, ssid, password, enabled }); $('sta-password').value = ''; $('network-state').textContent = t("m113");
  } catch (error) { $('network-error').textContent = error.message; }
  finally { refreshControls(); }
});
$('password-form').addEventListener('submit', async event => {
  event.preventDefault(); $('network-error').textContent = '';
  try {
    if (!isController()) throw new Error(t("m114"));
    const password = $('new-password').value;
    if (!/^[\x21-\x7e]{12,63}$/.test(password)) throw new Error(t("m115"));
    if (password !== $('confirm-password').value) throw new Error(t("m116"));
    $('password-save').disabled = true; const result = await api('password', 'PUT', { client_id: state.clientId, password });
    state.logged = false; stopConnection(); $('network-dialog').close(); $('new-password').value = ''; $('confirm-password').value = '';
    notify(result.network?.error ? t("m117", result.network.error) : t("m118"), result.network?.error ? 'error' : 'success'); showDialog($('login-dialog'));
  } catch (error) { $('network-error').textContent = error.message; }
  finally { refreshControls(); }
});

window.addEventListener('pagehide', () => { state.logged = false; stopConnection(); });
window.addEventListener('pageshow', event => { if (event.persisted) probeAndConnect(); });
window.addEventListener('online', () => { if (state.logged && !state.connected) { clearTimeout(reconnectTimer); reconnectTimer = null; probeAndConnect(); } });
setInterval(() => { if (state.dirty) { renderLog(); state.dirty = false; } }, 50);
setInterval(() => {
  for (const [id, item] of pending) if (Date.now() - item.created > 30000) { if (item.type === 'tx.send') setOutcome(id, 'unknown', t("m033")); pending.delete(id); notify(t("m119")); }
}, 1000);

function applyLanguage(code, persist = true) {
  const messages = ['notice', 'login-error', 'network-error', 'macro-error', 'network-state'].map(id => [$(id), describeMessage($(id).textContent)]);
  const outcomeMessages = [...outcomes].map(([id, value]) => [id, value.status, describeMessage(value.label)]);
  setLocale(code, { persist });
  translatePage(document);
  $('language-select').value = getLocale();
  for (const [element, message] of messages) element.textContent = renderMessage(message);
  for (const [id, status, message] of outcomeMessages) setOutcome(id, status, renderMessage(message));
  $('pause-log').textContent = state.paused ? t('m100') : t('m101');
  $('config-state').textContent = state.configDirty ? t('m038', configText()) : t('m077', configText(), state.serial.config.gap_us);
  const stats = state.serial.stats;
  $('error-detail').textContent = t('m039', Number(stats.uart_overflows) || 0, Number(stats.frame_errors) || 0, Number(stats.parity_errors) || 0);
  if (state.freeHeap !== undefined) $('memory').textContent = t('m040', formatBytes(state.freeHeap));
  $('network-summary').textContent = state.network.connected ? t('m041', state.network.ap_ip || '192.168.4.1', state.network.sta_ip || t('m042')) : t('m043', state.network.ap_ip || '192.168.4.1');
  setInputMode(state.inputMode);
  renderMacros(); refreshControls(); forceLog = true; state.dirty = true;
}
$('language-select').addEventListener('change', event => applyLanguage(event.target.value));
translatePage(document);
$('language-select').value = getLocale();

const prefs = restore('preferences', {});
if (['hex', 'text'].includes(prefs.inputMode)) state.inputMode = prefs.inputMode;
if (['none', 'cr', 'lf', 'crlf'].includes(prefs.ending)) $('ending').value = prefs.ending;
$('crc-enabled').checked = prefs.crc === true;
if (['both', 'hex', 'text'].includes(prefs.view)) $('view-mode').value = prefs.view;
$('auto-scroll').checked = prefs.autoScroll !== false;
try { macros = validateMacros(restore('macros', [])); } catch { macros = []; }
if (demo && !macros.length) macros = [{ name: t("m120"), input: '01 03 00 00 00 02', mode: 'hex', ending: 'none', crc: 'modbus' }, { name: t("m121"), input: 'Hello, S3 Serial!', mode: 'text', ending: 'crlf', crc: 'none' }];
if (demo) { $('demo-banner').hidden = false; $('send-input').value = '01 03 00 00 00 02'; $('crc-enabled').checked = true; }
renderMacros(); setInputMode(state.inputMode); refreshControls(); probeAndConnect();
