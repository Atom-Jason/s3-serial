#!/usr/bin/env python3
"""Check public release links, version metadata and obvious private artifacts."""
from pathlib import Path
import re
import sys
from urllib.parse import unquote

root = Path(__file__).resolve().parent.parent
errors = []
version_match = re.search(r'set\(PROJECT_VER "([^"]+)"\)', (root / 'CMakeLists.txt').read_text(encoding='utf-8'))
version = version_match.group(1) if version_match else ''
html = (root / 'web/index.html').read_text(encoding='utf-8')
if not version or f'class="version">v{version}</span>' not in html:
    errors.append('CMake and initial page version differ')
if 'esp_app_get_description()->version' not in (root / 'main/web_service.c').read_text(encoding='utf-8'):
    errors.append('Status API must read the application version')
required = ['LICENSE', '.github/workflows/ci.yml', 'docs/assets/banner.svg',
            'docs/assets/wiring.svg', 'docs/assets/desktop.jpg',
            'docs/assets/mobile.jpg', 'docs/assets/mobile-settings.jpg', 'docs/VERIFICATION.md',
            'web/i18n.mjs', 'web/messages.mjs', 'docs/LANGUAGES.md',
            'docs/readme/README.zh-CN.md', 'docs/readme/README.zh-TW.md',
            'docs/readme/README.ja.md', 'docs/readme/README.ko.md']
for name in required:
    if not (root / name).is_file():
        errors.append(f'Missing {name}')
checked = 0
for path in root.rglob('*.md'):
    if any(part in {'build', 'firmware', '.git', 'LICENSES', '.local'} for part in path.relative_to(root).parts):
        continue
    text = path.read_text(encoding='utf-8')
    links = re.findall(r'!?\[[^\]]*\]\(([^\s)]+)(?:\s+[^)]*)?\)', text)
    links += re.findall(r'(?:src|href)="([^"]+)"', text)
    for link in links:
        if re.match(r'^(?:[a-zA-Z][\w+.-]*:|#|//)', link):
            continue
        target = unquote(link.split('#', 1)[0])
        if target and not (path.parent / target).resolve().exists():
            errors.append(f'{path.relative_to(root)}: missing target {target}')
        checked += 1
    if re.search(r'[A-Z]:[/\\]Users[/\\]', text, re.I):
        errors.append(f'Private user path in {path.relative_to(root)}')
    if re.search(r'Device / Wi-Fi password:\s*(?!\[REDACTED\])\S{12,}', text):
        errors.append(f'Possible device credential in {path.relative_to(root)}')
for directory in ('work', 'device-backup'):
    if (root / directory).exists():
        errors.append(f'Private directory included: {directory}')
if errors:
    print('\n'.join(errors), file=sys.stderr)
    raise SystemExit(1)
print(f'Public release {version}: {checked} local links, required assets and version checks passed.')
print('This check does not replace review of credentials, dependency licenses or hardware behavior.')
