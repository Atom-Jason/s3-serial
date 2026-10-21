import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { LANGUAGES, DEFAULT_LOCALE, getLocale, setLocale, loadLocale, t, describeMessage, renderMessage } from '../web/i18n.mjs';
import { MESSAGES } from '../web/messages.mjs';
import { buildPayload, hex, validateConfig } from '../web/core.mjs';

test('English is the initial locale, independent of browser language', () => {
  assert.equal(DEFAULT_LOCALE, 'en');
  assert.equal(getLocale(), 'en');
  assert.equal(loadLocale(), 'en');
  assert.equal(t('m172'), 'Serial settings');
  assert.equal(setLocale('unknown'), 'en');
});
test('all five catalogs have every message and identical placeholder sets', () => {
  assert.deepEqual(LANGUAGES.map(item => item.code), ['en', 'zh-CN', 'zh-TW', 'ja', 'ko']);
  for (const [id, row] of Object.entries(MESSAGES)) {
    const parameters = [...row.en.matchAll(/\{\d+\}/g)].map(match => match[0]).sort();
    for (const { code } of LANGUAGES) {
      assert.equal(typeof row[code], 'string', `${id}/${code}`);
      assert.ok(row[code].trim(), `${id}/${code}`);
      assert.deepEqual([...row[code].matchAll(/\{\d+\}/g)].map(match => match[0]).sort(), parameters, `${id}/${code}`);
    }
    assert.doesNotMatch(row.en, /[\u3400-\u9fff]/u, `${id}/en`);
  }
});
test('language selection persists, and blocked storage does not stop switching', () => {
  const old = Object.getOwnPropertyDescriptor(globalThis, 'localStorage');
  let saved = null;
  try {
    Object.defineProperty(globalThis, 'localStorage', { configurable: true, value: { setItem(key, value) { saved = value; }, getItem() { return saved; } } });
    setLocale('ja', { persist: true });
    assert.equal(loadLocale(), 'ja');
    saved = '"unsupported"'; assert.equal(loadLocale(), 'en');
    Object.defineProperty(globalThis, 'localStorage', { configurable: true, get() { throw new Error('blocked'); } });
    assert.equal(loadLocale(), 'en');
    assert.equal(setLocale('ko', { persist: true }), 'ko');
  } finally {
    if (old) Object.defineProperty(globalThis, 'localStorage', old); else delete globalThis.localStorage;
    setLocale('en');
  }
});
test('changing locale preserves the payload bytes and localizes validation', () => {
  for (const { code } of LANGUAGES) {
    setLocale(code);
    assert.equal(hex(buildPayload({ input: '01 03 00 00 00 02', crc: 'modbus' }).finalBytes), '01 03 00 00 00 02 C4 0B');
    assert.throws(() => validateConfig({ baud_rate: 0 }), error => error.message === t('m134'));
  }
  setLocale('en');
});
test('active UI messages can rerender with reordered parameters; user data stays literal', () => {
  setLocale('en');
  const description = describeMessage(t('m104', 4, 2));
  const nested = describeMessage(t('m106', t('m103')));
  setLocale('ja');
  assert.equal(renderMessage(description), t('m104', 4, 2));
  assert.equal(renderMessage(nested), t('m106', t('m103')));
  assert.equal(t('m050', '<payload>{0}$&'), '記録を保存できません：<payload>{0}$&');
  setLocale('en');
});
test('markup defaults to English and all static and runtime translation IDs exist', () => {
  const html = fs.readFileSync(new URL('../web/index.html', import.meta.url), 'utf8');
  assert.match(html, /<html lang="en">/);
  for (const match of html.matchAll(/data-i18n(?:-[\w-]+)?="([^"]+)"/g)) assert.ok(MESSAGES[match[1]], match[1]);
  for (const name of ['app.mjs', 'core.mjs', 'demo.mjs']) {
    const source = fs.readFileSync(new URL(`../web/${name}`, import.meta.url), 'utf8');
    for (const match of source.matchAll(/\bt\(['"](m\d+)['"]/g)) assert.ok(MESSAGES[match[1]], `${name}/${match[1]}`);
  }
  const embed = fs.readFileSync(new URL('../tools/embed_web.py', import.meta.url), 'utf8');
  const cmake = fs.readFileSync(new URL('../main/CMakeLists.txt', import.meta.url), 'utf8');
  for (const asset of ['i18n.mjs', 'messages.mjs']) {
    assert.ok(embed.includes(`"${asset}"`), `Offline embed missing ${asset}`);
    assert.ok(cmake.includes(`/${asset}`), `CMake dependency missing ${asset}`);
  }
});
