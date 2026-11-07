"""Actual native Setup capability/selection IPC, fake inspection; no WebView/Windows."""
from pathlib import Path
import subprocess, shutil, tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'GlassSetup/src/main.cpp').read_text()
def function(token):
    a=source.index(token);b=source.index('{',a);depth=0
    for i in range(b,len(source)):
        if source[i]=='{':depth+=1
        if source[i]=='}':
            depth-=1
            if not depth:return source[a:i+1]
    raise ValueError(token)
capability=function('[[nodiscard]] bool SecureUxThemeRemovalSupportedLocked()')
blocked=function('[[nodiscard]] bool SecureUxThemeSelectionBlocked()')
clamp=function('void ClampSecureUxThemeSelection()')
caption=function('[[nodiscard]] UINT ProductApplyTextId()')
a=source.index('            if(message==L"components:select-all")')
selection=source[a:source.index('            EnableWindow(next_',a)]
harness=r'''
#include "GlassProducts.h"
#include <atomic>
#include <cassert>
#include <iostream>
#include <mutex>
#include <string>
using UINT=unsigned;constexpr UINT IDS_APPLY=1909,IDS_INSTALL=103;
namespace GlassInstallUtil {enum class SecureUxThemeState {Unavailable,Inactive,Active,ActivationPending,DeactivationPending,External,Conflict};}
struct Wizard {
 GlassProducts::Selection selection_{};
 std::atomic_bool inspectReady_{true};std::mutex inspectMutex_;
 struct {struct {bool installed=true;}secureUxTheme;std::wstring officialSecureUxThemeCode=L"official";}inspect_;
 GlassInstallUtil::SecureUxThemeState secureUxThemeState_=GlassInstallUtil::SecureUxThemeState::Inactive;
 std::wstring secureUxRemovalCode_;bool maintenanceMode_=false;
// CAPTION
// CAPABILITY
// BLOCKED
// CLAMP
 void Receive(const std::wstring& message){const bool os64=true;
// SELECTION
 }
};
int main(){
 {Wizard w;assert(w.ProductApplyTextId()==IDS_INSTALL);w.maintenanceMode_=true;assert(w.ProductApplyTextId()==IDS_APPLY);}
 {Wizard w;w.selection_.secureUxTheme=true;w.Receive(L"component:secureUxTheme:remove");
  assert(w.selection_.secureUxThemeRemove&&!w.selection_.secureUxTheme&&w.secureUxRemovalCode_==L"official");assert(w.ProductApplyTextId()==IDS_APPLY);
  w.Receive(L"components:select-all");assert(w.selection_.secureUxThemeRemove&&!w.selection_.secureUxTheme);
  w.Receive(L"component:secureUxTheme:0");assert(!w.selection_.secureUxThemeRemove&&!w.selection_.secureUxTheme&&w.secureUxRemovalCode_.empty());}
 for(int state:{0,2,3,4,5,6}){
  Wizard w;w.secureUxThemeState_=static_cast<GlassInstallUtil::SecureUxThemeState>(state);
  w.Receive(L"component:secureUxTheme:remove");assert(!w.selection_.secureUxThemeRemove&&w.secureUxRemovalCode_.empty());
 }
 {Wizard w;w.inspectReady_=false;w.Receive(L"component:secureUxTheme:remove");assert(!w.selection_.secureUxThemeRemove);}
 {Wizard w;w.inspect_.officialSecureUxThemeCode.clear();w.Receive(L"component:secureUxTheme:remove");assert(!w.selection_.secureUxThemeRemove);}
 {Wizard w;w.inspect_.secureUxTheme.installed=false;w.Receive(L"component:secureUxTheme:remove");assert(!w.selection_.secureUxThemeRemove);
  w.Receive(L"component:secureUxTheme:1");assert(w.selection_.secureUxTheme);}
 {Wizard w;w.Receive(L"component:secureUxTheme:1");assert(!w.selection_.secureUxTheme);
  w.Receive(L"components:select-all");assert(!w.selection_.secureUxTheme&&!w.selection_.secureUxThemeRemove);}
 {Wizard w;w.Receive(L"component:secureUxTheme:remove");w.secureUxThemeState_=GlassInstallUtil::SecureUxThemeState::Conflict;
  w.Receive(L"component:secureUxTheme:0");assert(!w.selection_.secureUxThemeRemove&&!w.selection_.secureUxTheme);}
 std::cout<<"PASS: actual native removal IPC/capability: inspected single vendor/inactive only, exact ProductCode captured, explicit cancellation, select-all preservation, no unsupported repair\n";
}
'''.replace('// CAPABILITY',capability).replace('// BLOCKED',blocked).replace('// CLAMP',clamp).replace('// SELECTION',selection).replace('// CAPTION',caption)
compiler=shutil.which('clang++') or shutil.which('g++')
if not compiler:raise SystemExit('clang++ or g++ required')
with tempfile.TemporaryDirectory(prefix='GlassSutRemoveIpc-') as directory:
    path=Path(directory)/'ipc.cpp';exe=Path(directory)/'ipc'
    path.write_text(harness)
    subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pedantic','-I'+str(root/'GlassSetup/src'),str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
