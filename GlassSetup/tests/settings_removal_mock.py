"""Execute real Unins selection functions with fake MSI discovery, not Windows."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/GlassUnins/main.cpp').read_text()
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
#include <cwchar>
#include <string>
#include "GlassProducts.h"
#include "GlassProductPlan.h"
namespace Msi {
const wchar_t *kProductPlatformX64=L"p64",*kProductPlatformX86=L"p32",*kProductSettingsX64=L"s64",*kProductSettingsX86=L"s32",*kProductOpenGlass=L"og",*kProductSecureUxThemeX64=L"u64",*kProductSecureUxThemeX86=L"u32";
bool p64=false,p32=false;
bool IsProductInstalled(const wchar_t* code){return std::wcscmp(code,kProductPlatformX64)==0?p64:std::wcscmp(code,kProductPlatformX86)==0?p32:false;}
}
namespace GlassInstallUtil {bool IsOs64Bit(){return true;}}
bool SameCode(const std::wstring& code,const wchar_t* expected){return code==expected;}
enum class Page {Select};
'''
methods='\n'.join(function(t) for t in ['bool RemovesAllInstalled(', 'void ChooseInitialProduct(', 'void NormalizeSelection('])
body=prefix+selection+'''\nstruct Test {
struct Args {std::wstring product;} args_;
Selection installed_,selected_;
'''+methods+'\n};\n'
main=r'''
int main(){
 for(auto code:{Msi::kProductSettingsX64,Msi::kProductSettingsX86}){
  Test t;t.installed_.settings64=t.installed_.settings32=t.installed_.settings=true;
  t.args_.product=code;t.ChooseInitialProduct();t.NormalizeSelection();
  assert(t.selected_.settings);
  assert(t.selected_.settings64==(code==Msi::kProductSettingsX64));
  assert(t.selected_.settings32==(code==Msi::kProductSettingsX86));
  assert(!t.selected_.x64&&!t.selected_.x86&&!t.selected_.secureUxTheme);
 }
 Test t;t.installed_.settings=true;t.installed_.settings32=true;t.args_.product=Msi::kProductSettingsX64;
 t.ChooseInitialProduct();t.NormalizeSelection();assert(!t.selected_.Any()); // Do not substitute the other Settings MSI.
}
'''
operation=function('int RunOperation(')
# Отдельной страницы риска нет: предупреждение показывается на карточках выбора.
assert 'SettingsRemovalRisk' not in s and 'Page::SettingsRisk' not in s
assert operation.index('if (product==P::Product::Platform64) {') < operation.index('if (product==P::Product::Settings64) {')
assert 'GlassMaintenanceExecutor::RunPhase' in operation
assert operation.index('if (product==P::Product::Platform32) {') < operation.index('if (product==P::Product::Settings32) {')
assert 'if (chosen.settings64) probes.push_back({Msi::kProductSettingsX64' in operation
assert 'if (chosen.settings32) probes.push_back({Msi::kProductSettingsX86' in operation
assert 'RequiresInteractiveSettingsRemoval' in function('int RunSilent(') # Existing SUT silent prohibition preserved.
with tempfile.TemporaryDirectory() as d:
    source=Path(d)/'test.cpp';source.write_text(body+main)
    binary=Path(d)/'test'
    subprocess.run(['g++','-std=c++17','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root/'GlassSetup/src'),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: independent Settings product selection, no cascade, no separate risk page, source order guards (fake discovery)')
