import { MESSAGES } from './messages.mjs';

export const LANGUAGES = Object.freeze([
  { code: 'en', name: 'English' },
  { code: 'zh-CN', name: '简体中文' },
  { code: 'zh-TW', name: '繁體中文' },
  { code: 'ja', name: '日本語' },
  { code: 'ko', name: '한국어' },
]);
export const DEFAULT_LOCALE = 'en';
export const LOCALE_STORAGE_KEY = 's3serial.locale.v1';
let locale = DEFAULT_LOCALE;
const renderedMessages = new Map();

export function getLocale() { return locale; }
export function loadLocale() {
  try {
    const saved = JSON.parse(globalThis.localStorage.getItem(LOCALE_STORAGE_KEY));
    return LANGUAGES.some(item => item.code === saved) ? saved : DEFAULT_LOCALE;
  } catch { return DEFAULT_LOCALE; }
}
export function setLocale(code, { persist = false } = {}) {
  locale = LANGUAGES.some(item => item.code === code) ? code : DEFAULT_LOCALE;
  if (persist) {
    try { globalThis.localStorage.setItem(LOCALE_STORAGE_KEY, JSON.stringify(locale)); }
    catch { /* A language change still works when browser storage is blocked. */ }
  }
  return locale;
}
export function t(id, ...values) {
  if (id == null) return '';
  const text = MESSAGES[id]?.[locale] ?? MESSAGES[id]?.en;
  if (text === undefined) return String(id);
  const translated = text.replace(/\{(\d+)\}/g, (_, index) => String(values[Number(index)] ?? `{${index}}`));
  renderedMessages.delete(translated);
  renderedMessages.set(translated, { id, values });
  while (renderedMessages.size > 512) renderedMessages.delete(renderedMessages.keys().next().value);
  return translated;
}
export function describeMessage(text) {
  const item = renderedMessages.get(text);
  return item ? { id: item.id, values: item.values.map(value => typeof value === 'string' && renderedMessages.has(value) && value !== text ? describeMessage(value) : value) } : { text };
}
export function renderMessage(item) {
  if (!item?.id) return item?.text ?? '';
  return t(item.id, ...item.values.map(value => value && typeof value === 'object' && ('id' in value || 'text' in value) ? renderMessage(value) : value));
}
export function translatePage(root) {
  root.documentElement.lang = locale;
  for (const element of root.querySelectorAll('[data-i18n]')) element.textContent = t(element.dataset.i18n);
  for (const attribute of ['placeholder', 'aria-label', 'title']) {
    for (const element of root.querySelectorAll(`[data-i18n-${attribute}]`)) element.setAttribute(attribute, t(element.getAttribute(`data-i18n-${attribute}`)));
  }
}
