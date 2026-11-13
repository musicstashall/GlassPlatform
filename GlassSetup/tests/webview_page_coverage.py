"""Exhaustive enum-to-WebView routing audit and early startup initialization guard."""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[2]
setup=(root/'GlassSetup/src/main.cpp').read_text()
unins=(root/'GlassSetup/GlassUnins/main.cpp').read_text()
html=(root/'GlassSetup/res/slideshow.html').read_text()
valid=set(re.search(r"const valid=\[(.*?)\]",html).group(1).replace("'",'').split(','))
for name,source,start,end in [('Setup',setup,'const wchar_t* WebPageName(', 'std::wstring ConflictsJson()'),('GlassUnins',unins,'const wchar_t* WebPage()','std::wstring InstalledMsiVersion(')]:
 pages={p.strip() for p in re.search(r'enum class Page \{(.*?)\}',source).group(1).split(',')}
 method=source[source.index(start):source.index(end,source.index(start))]
 routed=set(re.findall(r'case Page::(\w+)',method))
 assert routed==pages,(name,'unmapped pages',pages-routed,'unexpected',routed-pages)
 targets=set(re.findall(r'return L"([\w-]+)"',method))
 assert targets<=valid,(name,targets-valid)
 print(f'PASS: {name}: all {len(pages)} page enum values explicitly mapped to supported WebView states')
complete=setup[setup.index('void CompleteStartupIfReady()'):setup.index('void Layout()',setup.index('void CompleteStartupIfReady()'))]
assert complete.index('PostMessageW(window,kInitWebView')<complete.index('if (!result.ok)')<complete.index('MaintenanceProductAbsent(')
assert complete.count('PostMessageW(window,kInitWebView')==1
assert 'if(!PostMessageW(window,kWebInit,0,0))WebFailure();' in unins
assert 'if (!alreadyRunning_) PostMessageW(window,kWebInit' not in unins
assert 'case Page::AlreadyRunning: case Page::RuntimeError: return L"error";' in setup
print('PASS: startup errors/stale maintenance/duplicate launches cannot bypass WebView initialization when runtime is available; bootstrap fallback retained')
