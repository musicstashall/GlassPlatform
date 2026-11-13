"""Actual native Setup capability/selection IPC, fake inspection; no WebView/Windows."""
from pathlib import Path
import subprocess, shutil, tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'GlassSetup/src/main.cpp').read_text()
# Healthy/active SUT must not receive a generic warning merely because removal
# prepares stock selectors. Runtime/reboot labels and worker behavior stay intact.
start=source.index('        const bool sutRequiresSettings=false;')
end=source.index('    std::wstring LaunchCardsJson()',start)
card=source[start:end]
assert 'JsStringLiteral(L"")' in card and 'sutRemovalNotice' not in card
assert 'Перед удалением загруженного SecureUxTheme' not in source
assert 'Text(sutStateId)' in card

def function(token):
    a=source.index(token);b=source.index('{',a);depth=0
    for i in range(b,len(source)):
        if source[i]=='{':depth+=1
        if source[i]=='}':
            depth-=1
            if not depth:return source[a:i+1]
    raise ValueError(token)
reason=function('[[nodiscard]] const wchar_t* SecureUxThemeRemovalBlockReasonLocked()')
capability=function('[[nodiscard]] bool SecureUxThemeRemovalSupportedLocked()')
blocked=function('[[nodiscard]] bool SecureUxThemeSelectionBlocked()')
dependency='\n'.join(function(token) for token in ['bool NoSettingsInstalledLocked()', 'bool SecureUxRemovalNeedsSettingsUpdateLocked()', 'bool SecureUxRemovalDependencySatisfiedLocked()'])
clamp=function('void ClampSecureUxThemeSelection()')
caption=function('[[nodiscard]] UINT ProductApplyTextId()')
a=source.index('            if(message==L"components:select-all")')
selection=source[a:source.index('            EnableWindow(next_',a)]
harness=r'''
#include "GlassProducts.h"
#include <atomic>
#include <optional>
#include <array>
#include <cassert>
#include <iostream>
#include <mutex>
#include <string>
constexpr int LANG_RUSSIAN=25;int gLanguage=0;
#define PRIMARYLANGID(x) (x)
using UINT=unsigned;constexpr UINT IDS_APPLY=1909,IDS_INSTALL=103;
bool VersionOlder(const std::wstring& a,const std::wstring& b){return !a.empty()&&a<b;}
namespace GlassInstallUtil {bool os64=true;bool IsOs64Bit(){return os64;}enum class SecureUxThemeState {Unavailable,Inactive,Active,ActivationPending,DeactivationPending,External,Conflict};}
struct Wizard {
 GlassProducts::Selection selection_{};
 GlassProducts::Selection installedSelection_{};
 struct Asset {bool downloadable=true;struct {std::array<int,4> parts{3,0,0,0};}version;};
 struct {std::array<std::optional<Asset>,5> products;}releaseCatalog_;

 std::atomic_bool inspectReady_{true};std::mutex inspectMutex_;
 struct {struct Settings {bool installed=true,sourceVerified=true,damaged=false,nativeSourceDiffers=false;std::wstring installedPackageVersion=L"2.0.0",targetVersion=L"2.0.0";}settings64,settings32;struct {bool installed=true;}secureUxTheme;std::wstring officialSecureUxThemeCode=L"official";}inspect_;
 GlassInstallUtil::SecureUxThemeState secureUxThemeState_=GlassInstallUtil::SecureUxThemeState::Inactive;
 std::wstring secureUxRemovalCode_;bool maintenanceMode_=false;
// DEPENDENCY
// CAPTION
// REASON
// CAPABILITY
// BLOCKED
// CLAMP
 void Receive(const std::wstring& message){const bool os64=true;
// SELECTION
 }
};
int main(){
 {Wizard w;w.selection_.independentSettings=true;w.inspect_.settings64.nativeSourceDiffers=true;
  assert(w.SecureUxRemovalNeedsSettingsUpdateLocked());w.Receive(L"component:secureUxTheme:remove");assert(!w.selection_.secureUxThemeRemove);
  w.selection_.settings64=true;w.Receive(L"component:secureUxTheme:remove");assert(w.selection_.secureUxThemeRemove);
 }

 {Wizard w;w.selection_.independentSettings=true;w.inspect_.settings64.targetVersion=L"3.0.0";
  assert(w.SecureUxRemovalNeedsSettingsUpdateLocked());w.Receive(L"component:secureUxTheme:remove");assert(!w.selection_.secureUxThemeRemove);
  w.selection_.settings32=true;assert(!w.SecureUxRemovalDependencySatisfiedLocked());
  w.selection_.settings64=true;w.Receive(L"component:secureUxTheme:remove");assert(w.selection_.secureUxThemeRemove);
  w.selection_.settings64=false;w.Receive(L"unrelated");assert(!w.selection_.secureUxThemeRemove);
 }
 {Wizard w;w.selection_.glassSettings=false;w.installedSelection_.glassSettings=false;w.inspect_.settings64.installed=w.inspect_.settings32.installed=false;
  w.secureUxThemeState_=GlassInstallUtil::SecureUxThemeState::Unavailable;w.Receive(L"component:secureUxTheme:remove");
  assert(w.selection_.secureUxThemeRemove&&w.selection_.secureUxRemovalWithoutSettings);
 }
 {Wizard w;w.inspect_.settings64.installed=false;assert(w.SecureUxRemovalNeedsSettingsUpdateLocked());}
 {Wizard w;w.releaseCatalog_.products[2]=Wizard::Asset{};assert(w.SecureUxRemovalNeedsSettingsUpdateLocked());}
 {Wizard w;w.inspect_.settings32.targetVersion=L"3.0.0";assert(!w.SecureUxRemovalNeedsSettingsUpdateLocked());
  GlassInstallUtil::os64=false;assert(w.SecureUxRemovalNeedsSettingsUpdateLocked());GlassInstallUtil::os64=true;
 }

 for(int language:{0,LANG_RUSSIAN}){gLanguage=language;
  for(int state=0;state<=6;++state){Wizard w;w.secureUxThemeState_=static_cast<GlassInstallUtil::SecureUxThemeState>(state);
   assert(w.SecureUxThemeRemovalSupportedLocked()==(state>=1&&state<=4));
   const auto* reason=w.SecureUxThemeRemovalBlockReasonLocked();assert((reason==nullptr)==(state>=1&&state<=4));
   if(reason)assert(std::wstring(reason).size()>15);
  }
  Wizard w;w.inspectReady_=false;assert(w.SecureUxThemeRemovalBlockReasonLocked());
  w.inspectReady_=true;w.inspect_.secureUxTheme.installed=false;assert(w.SecureUxThemeRemovalBlockReasonLocked());
  w.inspect_.secureUxTheme.installed=true;w.inspect_.officialSecureUxThemeCode.clear();assert(w.SecureUxThemeRemovalBlockReasonLocked());
 }

 {Wizard w;assert(w.ProductApplyTextId()==IDS_INSTALL);w.maintenanceMode_=true;assert(w.ProductApplyTextId()==IDS_APPLY);}
 {Wizard w;w.selection_.secureUxTheme=true;w.Receive(L"component:secureUxTheme:remove");
  assert(w.selection_.secureUxThemeRemove&&!w.selection_.secureUxTheme&&w.secureUxRemovalCode_==L"official");assert(w.ProductApplyTextId()==IDS_APPLY);
  w.Receive(L"components:select-all");assert(w.selection_.secureUxThemeRemove&&!w.selection_.secureUxTheme);
  w.Receive(L"component:secureUxTheme:0");assert(!w.selection_.secureUxThemeRemove&&!w.selection_.secureUxTheme&&w.secureUxRemovalCode_.empty());}
 for(int state:{0,5,6}){
  Wizard w;w.secureUxThemeState_=static_cast<GlassInstallUtil::SecureUxThemeState>(state);
  w.Receive(L"component:secureUxTheme:remove");assert(!w.selection_.secureUxThemeRemove&&w.secureUxRemovalCode_.empty());
 }
 {Wizard w;w.secureUxThemeState_=GlassInstallUtil::SecureUxThemeState::ActivationPending;
  w.Receive(L"component:secureUxTheme:remove");assert(w.selection_.secureUxThemeRemove&&!w.selection_.secureUxTheme&&w.secureUxRemovalCode_==L"official");}
 {Wizard w;w.inspectReady_=false;w.Receive(L"component:secureUxTheme:remove");assert(!w.selection_.secureUxThemeRemove);}
 {Wizard w;w.inspect_.officialSecureUxThemeCode.clear();w.Receive(L"component:secureUxTheme:remove");assert(!w.selection_.secureUxThemeRemove);}
 {Wizard w;w.inspect_.secureUxTheme.installed=false;w.Receive(L"component:secureUxTheme:remove");assert(!w.selection_.secureUxThemeRemove);
  w.Receive(L"component:secureUxTheme:1");assert(w.selection_.secureUxTheme);}
 {Wizard w;w.Receive(L"component:secureUxTheme:1");assert(!w.selection_.secureUxTheme);
  w.Receive(L"components:select-all");assert(!w.selection_.secureUxTheme&&!w.selection_.secureUxThemeRemove);}
 {Wizard w;w.Receive(L"component:secureUxTheme:remove");w.secureUxThemeState_=GlassInstallUtil::SecureUxThemeState::Conflict;
  w.Receive(L"component:secureUxTheme:0");assert(!w.selection_.secureUxThemeRemove&&!w.selection_.secureUxTheme);}
 std::cout<<"PASS: actual native removal IPC/capability: inspected single vendor/runtime known; loaded preflight deferred to Apply, exact ProductCode captured, explicit cancellation, select-all preservation, no unsupported repair\n";
}
'''.replace('// REASON',reason).replace('// CAPABILITY',capability).replace('// BLOCKED',blocked).replace('// CLAMP',clamp).replace('// SELECTION',selection).replace('// CAPTION',caption).replace('// DEPENDENCY',dependency)
compiler=shutil.which('clang++') or shutil.which('g++')
if not compiler:raise SystemExit('clang++ or g++ required')
with tempfile.TemporaryDirectory(prefix='GlassSutRemoveIpc-') as directory:
    path=Path(directory)/'ipc.cpp';exe=Path(directory)/'ipc'
    path.write_text(harness)
    subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pedantic','-I'+str(root/'GlassSetup/src'),str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
