"""Execute real Unins selection/risk functions with fake MSI discovery, not Windows."""
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
enum class Page {Select,SettingsRisk};
'''
methods='\n'.join(function(t) for t in ['bool RemovesAllInstalled(', 'void ChooseInitialProduct(', 'void NormalizeSelection(', 'unsigned SettingsRemovalRisk(', 'bool ConfirmSettingsRemovalRisk('])
body=prefix+selection+'''\nstruct Test {
struct Args {std::wstring product;bool acceptSettingsRemovalRisk=false;} args_;
Selection installed_,selected_;
unsigned pendingSettingsRisk_=0,confirmedSettingsRisk_=0;Page page_=Page::Select;
void SetPage(Page page){page_=page;}
'''+methods+'\n};\n'
main=r'''
int main(){
 for(unsigned bits=0;bits<64;++bits){
  bool s64=bits&1,s32=bits&2,p64=bits&4,p32=bits&8,r64=bits&16,r32=bits&32;
  unsigned expected=(s64&&p64&&!r64?1u:0u)|(s32&&p32&&!r32?2u:0u);
  assert(GlassProductPlan::SettingsRemovalRiskMask(s64,s32,p64,p32,r64,r32)==expected);
 }
 for(auto code:{Msi::kProductSettingsX64,Msi::kProductSettingsX86}){
  Test t;t.installed_.settings64=t.installed_.settings32=t.installed_.settings=true;
  t.args_.product=code;t.ChooseInitialProduct();t.NormalizeSelection();
  assert(t.selected_.settings);
  assert(t.selected_.settings64==(code==Msi::kProductSettingsX64));
  assert(t.selected_.settings32==(code==Msi::kProductSettingsX86));
  assert(!t.selected_.x64&&!t.selected_.x86&&!t.selected_.secureUxTheme);
  Msi::p64=true;Msi::p32=true;
  assert(!t.ConfirmSettingsRemovalRisk(t.selected_));assert(t.page_==Page::SettingsRisk);
  assert(t.confirmedSettingsRisk_==0); // Showing a warning isn't consent.
  t.confirmedSettingsRisk_=t.pendingSettingsRisk_;assert(t.ConfirmSettingsRemovalRisk(t.selected_));
  t.selected_.settings64=t.selected_.settings32=true;t.NormalizeSelection();
  assert(!t.ConfirmSettingsRemovalRisk(t.selected_)); // Other architecture needs its own consent.
  t.args_.acceptSettingsRemovalRisk=true;assert(t.ConfirmSettingsRemovalRisk(t.selected_));
 }
 Test t;t.installed_.settings=true;t.installed_.settings32=true;t.args_.product=Msi::kProductSettingsX64;
 t.ChooseInitialProduct();t.NormalizeSelection();assert(!t.selected_.Any()); // Do not substitute the other Settings MSI.
 t.installed_.settings64=true;t.args_.product=Msi::kProductSettingsX64;t.ChooseInitialProduct();t.NormalizeSelection();
 Msi::p64=false;Msi::p32=true;assert(t.SettingsRemovalRisk(t.selected_)==0); // Other Platform alone is irrelevant.
 Msi::p64=true;t.selected_.x64=true;assert(t.SettingsRemovalRisk(t.selected_)==0); // Ordered joint removal.
}
'''
operation=function('int RunOperation(')
assert operation.index('SettingsRemovalRisk(chosen)') < operation.index('ResolveInstallRoots')
assert operation.index('if (chosen.x64) {\n            if (!remove(') < operation.index('if (chosen.settings64) {\n            if (!remove(')
assert operation.index('if (chosen.x86) {\n            if (!remove(') < operation.index('if (chosen.settings32) {\n            if (!remove(')
assert 'if (chosen.settings64) probes.push_back({Msi::kProductSettingsX64' in operation
assert 'if (chosen.settings32) probes.push_back({Msi::kProductSettingsX86' in operation
assert 'RequiresInteractiveSettingsRemoval' in function('int RunSilent(') # Existing SUT silent prohibition preserved.
assert '--accept-settings-removal-risk' in function('Arguments ParseArguments(')
assert '--accept-settings-removal-risk' in function('int RelaunchFromPrivateTemp(')
assert function('void StartRemoval(').index('ConfirmSettingsRemovalRisk') < function('void StartRemoval(').index('std::thread')
with tempfile.TemporaryDirectory() as d:
    source=Path(d)/'test.cpp';source.write_text(body+main)
    binary=Path(d)/'test'
    subprocess.run(['g++','-std=c++17','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root/'GlassSetup/src'),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: 64 risk combinations, independent Settings product selection, no cascade, UI consent boundary and source order guards (fake discovery)')
