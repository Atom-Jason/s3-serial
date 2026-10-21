# Languages and translation maintenance

The web application and repository README support English, Simplified Chinese, Traditional Chinese, Japanese and Korean. The first visit defaults to English, independent of navigator.language. The selector stores the choice under s3serial.locale.v1 in browser local storage; a blocked or unsupported preference falls back to English.

## Scope and state preservation

Static text, accessible labels, placeholders, dialogs, client validation, status and command feedback use the offline catalog. Changing language preserves the current payload, custom baud draft, active UART settings, control lease and running repeat job. It does not send a serial command or reload the page.

Historical log messages stay in their original language. Raw TX/RX bytes, user command names, protocol field names, CSV column names and device diagnostic strings are not automatically translated. Detailed engineering documents are currently Simplified Chinese. The five README editions provide localized setup, operating boundaries and license summaries.

## Files

- web/messages.mjs: 249 message IDs with complete en, zh-CN, zh-TW, ja and ko values.
- web/i18n.mjs: locale selection, interpolation and DOM text updates; no network requests.
- web/index.html: English initial markup and data-i18n attributes.
- web/app.mjs, core.mjs and demo.mjs: runtime messages using t().
- docs/readme/: four translated README files; root README.md is English.

When adding a message, supply every locale and preserve numbered placeholders such as {0}. Use textContent, not HTML interpolation. Do not translate serial bytes or user content. Include new resource modules in both tools/embed_web.py and main/CMakeLists.txt.

## Verification

```sh
node --test tests/*.test.mjs
python tools/check_release.py
idf.py build
```

The language regression checks initial English, complete catalogs, matching placeholders, preference persistence and blocked storage, localized validation, unchanged Modbus bytes, active-message rerendering, markup IDs and offline resource dependencies. Browser checks additionally cover draft preservation, reload persistence and responsive layout. Physical-phone and real RS485 validation remain separate.
