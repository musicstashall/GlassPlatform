"""Optional source/resource smoke test. Run: python tests/verify_resources.py"""
from pathlib import Path
import base64
import hashlib
import json
import re
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
header = (root / 'res/resource.h').read_text(encoding='utf-8')
source = (root / 'src/main.cpp').read_text(encoding='utf-8')
rc = (root / 'res/installer.rc').read_text(encoding='utf-8')
ids = dict(re.findall(r'#define\s+(\w+)\s+(\d+)', header))
assert len(ids.values()) == len(set(ids.values())), 'Duplicate resource/control IDs'
used = set(re.findall(r'\b(?:IDS_|IDR_|IDI_|IDC_)[A-Z_]+\b', source)) - {'IDC_ARROW'}
assert not used - ids.keys(), used - ids.keys()
for name in ids:
    if name.startswith('IDS_'):
        assert len(re.findall(r'\b' + name + r'\b', rc)) == 2, name
for filename in re.findall(r'(?:RCDATA|ICON|RT_MANIFEST) "([^"]+)"', rc):
    assert (root / 'res' / filename).is_file(), filename
ET.parse(root / 'res/app.manifest')
json.loads((root / 'CMakePresets.json').read_text(encoding='utf-8'))
html = (root / 'res/slideshow.html').read_text(encoding='utf-8')
images = re.findall(r'data:image/jpeg;base64,([^"\s]+)', html)
assert len(images) == 5
for image in images:
    raw = base64.b64decode(image, validate=True)
    assert raw.startswith(b'\xff\xd8') and raw.endswith(b'\xff\xd9')
assert 'id="counter"' not in html and 'id="footnote"' not in html
assert '<link rel="stylesheet"' not in html
assert 'data:font/ttf;base64,' in html
assert 'GetUserDefaultUILanguage()' in source
assert 'DrawThemeBackground(panelTheme_,dc,CPANEL_SMALLCOMMANDAREA' in source
assert 'case WM_ERASEBKGND: Erase(' in source
assert 'constexpr ULONGLONG kDuration = 30000;' in source
assert 'Resource(IDR_EULA,gLanguage)' in source
for filename, expected in json.loads((root / 'tests/user-resource-hashes.json').read_text()).items():
    assert hashlib.sha256((root / 'res' / filename).read_bytes()).hexdigest() == expected, filename
print('PASS: IDs, bilingual STRINGTABLE coverage, embedded files, XML, presets,')
print('      five JPEGs, offline font, latest navigation, native locale/theme wiring,')
print('      and unchanged user-provided PNG/EULA files.')
print('This smoke test does NOT compile C++ or execute Win32/WebView2.')
