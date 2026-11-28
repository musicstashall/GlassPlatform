"""Network-unavailable OpenGlass/SecureUxTheme checks are retryable, not only HTTP 403."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/main.cpp').read_text(encoding='utf-8')
a=s.index('bool VendorMetadataUnavailable(')
b=s.index('\n}',a)+2
function=s[a:b]
assert s.count('VendorMetadataUnavailable(')==5
assert 'find(L"403")' not in s[b:]
handler=s[s.index('case kCatalogRetryReady:'):s.index('case kVendorMetadataReady:')]
assert 'PublishRetriedCatalog()' in handler
publish=s[s.index('    void PublishRetriedCatalog()'):s.index('    void RetryProductCatalog()')]
assert 'releaseCatalog_=std::move(catalog);' in publish and 'FetchCatalog' in publish
assert 'serverDownloadDeferred_' in publish
continue_apply=s[s.index('    void RequestComponentContinue()'):s.index('    void NoteDownloadsPrepared()')]
assert continue_apply.index('ServerMsiNeedsTransfer')<continue_apply.index('StartInstallation')
prepared=s[s.index('    void NoteDownloadsPrepared()'):s.index('    std::wstring NativeDownloadSummary()')]
assert 'Selected MSI is only on the server; download stays on the product page' in prepared
assert prepared.index('ServerMsiNeedsTransfer')<prepared.index('Selected packages are already local')
html=(root/'GlassSetup/res/slideshow.html').read_text(encoding='utf-8')
assert 'setCardCheckDonut' in html and 'item.catalogRetrying' in html
code=r'''
#include <cassert>
#include <string>
'''+function+r'''
int main(){
 assert(VendorMetadataUnavailable(L"GitHub HTTP error (403)"));
 assert(VendorMetadataUnavailable(L"GitHub request failed (12007)"));
 assert(VendorMetadataUnavailable(L"GitHub HTTP error (500)"));
 assert(!VendorMetadataUnavailable(L""));
 assert(!VendorMetadataUnavailable(L"OpenGlass discovery: invalid metadata"));
 assert(!VendorMetadataUnavailable(L"Local MSI rejected"));
}
'''
with tempfile.TemporaryDirectory(prefix='VendorMetadata-') as d:
    path=Path(d)/'test.cpp'
    exe=Path(d)/'test'
    path.write_text(code)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: vendor version retry covers transport failures, catalog retry publishes the snapshot, server-only MSI does not continue')
