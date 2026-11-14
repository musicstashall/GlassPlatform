"""Real full-removal/registry-absence guards on fake discovery, plus UI source contracts."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/GlassUnins/main.cpp').read_text()
ui=(root/'GlassSetup/res/slideshow.html').read_text()
def function(token):
    start=s.index(token);brace=s.index('{',start);depth=0
    for i in range(brace,len(s)):
        if s[i]=='{':depth+=1
        elif s[i]=='}':
            depth-=1
            if not depth:return s[start:i+1]
    raise ValueError(token)
selection=s[s.index('struct Selection {'):s.index('enum class Page')]
prefix=r'''
#include <cassert>
#include <initializer_list>
#include <map>
#include <string>
using UINT=unsigned;using INSTALLSTATE=int;
bool fileProduct=false,rootsKnown=true;
namespace GlassInstallUtil {struct InstallRoots {std::wstring openGlass;};bool ResolveInstallRoots(InstallRoots&){return rootsKnown;}}
namespace GlassOpenGlassInstall {bool HasProduct(){return false;}}
namespace GlassOpenGlassFiles {bool HasReceipt(const std::wstring&){return fileProduct;}}
constexpr UINT ERROR_NO_MORE_ITEMS=259,ERROR_UNKNOWN_PRODUCT=1605,ERROR_SUCCESS=0,MSIINSTALLCONTEXT_ALL=7;
constexpr int INSTALLSTATE_UNKNOWN=-1,INSTALLSTATE_ABSENT=2;
namespace Msi {
const wchar_t *kProductPlatformX64=L"p64",*kProductPlatformX86=L"p32",*kProductSettingsX64=L"s64",*kProductSettingsX86=L"s32",*kProductOpenGlass=L"og",*kProductSecureUxThemeX64=L"u64",*kProductSecureUxThemeX86=L"u32";
}
std::map<std::wstring,UINT> enumerations;std::map<std::wstring,INSTALLSTATE> states;
UINT MsiEnumProductsExW(const wchar_t* code,const wchar_t* sid,UINT context,UINT index,wchar_t*,void*,void*,void*){
 assert(std::wstring(sid)==L"S-1-1-0"&&context==MSIINSTALLCONTEXT_ALL&&index==0);
 auto it=enumerations.find(code);return it==enumerations.end()?ERROR_NO_MORE_ITEMS:it->second;
}
INSTALLSTATE MsiQueryProductStateW(const wchar_t* code){auto it=states.find(code);return it==states.end()?INSTALLSTATE_UNKNOWN:it->second;}
'''
methods=function('bool RemovesAllInstalled(')+'\n'+function('bool AnyRegisteredProduct(')
main=r'''
Selection make(unsigned bits){Selection v;v.x64=bits&1;v.x86=bits&2;v.settings64=bits&4;v.settings32=bits&8;v.settings=v.settings64||v.settings32;v.openGlass=bits&16;v.secureUxTheme=bits&32;return v;}
int main(){
 for(unsigned installed=0;installed<64;++installed)for(unsigned selected=0;selected<64;++selected){
  Test t;t.installed_=make(installed);auto chosen=make(selected);
  assert(t.RemovesAllInstalled(chosen)==(installed!=0&&selected!=0&&(selected&installed)==installed));
 }
 Test t;assert(!t.AnyRegisteredProduct());
 fileProduct=true;assert(t.AnyRegisteredProduct());fileProduct=false;
 rootsKnown=false;assert(t.AnyRegisteredProduct());rootsKnown=true;
 Selection exclude;exclude.settings64=true;
 enumerations[Msi::kProductSettingsX64]=ERROR_SUCCESS;assert(!t.AnyRegisteredProduct(&exclude));
 enumerations[Msi::kProductSettingsX86]=ERROR_SUCCESS;assert(t.AnyRegisteredProduct(&exclude));
 enumerations.clear();
 for(const auto* code:{Msi::kProductPlatformX64,Msi::kProductPlatformX86,Msi::kProductSettingsX64,Msi::kProductSettingsX86,Msi::kProductOpenGlass,Msi::kProductSecureUxThemeX64,Msi::kProductSecureUxThemeX86}){
  enumerations[code]=ERROR_SUCCESS;assert(t.AnyRegisteredProduct());
  enumerations[code]=5;assert(t.AnyRegisteredProduct()); // Enumeration error is not absence.
  enumerations[code]=ERROR_UNKNOWN_PRODUCT;assert(!t.AnyRegisteredProduct());
  states[code]=5;assert(t.AnyRegisteredProduct());
  states[code]=-6;assert(t.AnyRegisteredProduct()); // Bad configuration fails closed.
  states[code]=INSTALLSTATE_ABSENT;assert(!t.AnyRegisteredProduct());
  enumerations.clear();states.clear();
 }
}
'''
components=function('std::wstring ComponentsJson(')
assert 'if (page_==Page::Cleanup && RemovesAllInstalled(selected_))' in components
setter=function('void SetComponent(')
assert 'page_==Page::Cleanup' in setter and 'page_!=Page::Select || id==L"cleanup"' in setter
start=function('void StartRemoval(')
assert start.index('if (!selected_.Any()) return;')<start.index('SetPage(Page::Cleanup)')<start.index('std::thread')
assert 'selected_.cleanupSettings=false;' in start
worker=function('int RunOperation(')
assert 'args_.silent || !cleanupReviewed_ || !RemovesAllInstalled(chosen) || AnyRegisteredProduct(&chosen)' in worker
assert worker.index('PreflightCleanup')<worker.index('StopAllForUninstall')
assert worker.index('if (AnyRegisteredProduct())')<worker.index('cleanupProfiles.Cleanup(detail)')
assert worker.index('if (!remove(Msi::kProductSettingsX86')<worker.index('cleanupProfiles.Cleanup(detail)')
assert 'cleanupReviewed_=false; selected_.cleanupSettings=false;' in function('void RequestClose(')
assert 'case Page::Cleanup: return L"cleanup-review";' in s
assert '#stage[data-mode="cleanup-review"] #components-panel' in ui
assert '#stage[data-mode="cleanup-review"] #components-select-all' in ui
with tempfile.TemporaryDirectory() as directory:
    source=Path(directory)/'test.cpp';source.write_text(prefix+selection+'struct Test { Selection installed_;\n'+methods+'\n};\n'+main)
    binary=Path(directory)/'test'
    subprocess.run(['g++','-std=c++17','-fsanitize=address,undefined','-fno-pie','-no-pie',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: 4096 full-removal combinations; absence/error guards for seven MSI identities; cleanup consent/page/preflight/order source contracts')
