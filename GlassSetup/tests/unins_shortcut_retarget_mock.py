"""Actual Unins retarget commit/finalizer with fake MSI/shortcut APIs.
No COM, Windows ACL/locking, real MSI rollback or crash recovery is tested.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
s = (root / 'GlassSetup/GlassUnins/main.cpp').read_text()
operation = s[s.index('int RunOperation('):s.index('    void StartRemoval()')]


def block(token, text=operation):
    a = text.index(token)
    brace = text.index('{', a)
    depth = 1
    b = brace + 1
    while depth:
        depth += (text[b] == '{') - (text[b] == '}')
        b += 1
    return text[a:b]


commit = block('        if(keptSettingsProbe) {', operation[operation.index('removalStarted_.store'):])
finish = block('        const auto finishSettingsShortcut=') + ';'
check = block('        const auto recheckKeptSettings=')
check_body = check[check.index('{'):]
workspace = block('        struct ProbeWorkspace') + ';'
assert operation.index('PrepareSettingsShortcutRetarget') < operation.index('StopAllForUninstall')
assert operation.index('operationState_.compare_exchange_strong') < operation.index('GlassInstallRemoval::Commit(settingsShortcut')
assert operation.index('GlassInstallRemoval::Commit(settingsShortcut') < operation.index('Msi::UninstallProduct')
assert 'chosen.settings64?x86Dir_:x64Dir_' in operation
assert 'workspace.preserve=true;\n                if(!GlassInstallRemoval::Commit' in operation
assert 'catch(...) { (void)finishSettingsShortcut();throw; }' in operation
assert 'if(!finishSettingsShortcut())return ERROR_INSTALL_FAILURE;' in operation
assert operation.index('settingsMsiAttempted=true;', operation.index('if (product==P::Product::Settings64)')) < operation.index('remove(Msi::kProductSettingsX64', operation.index('if (product==P::Product::Settings64)'))

harness = r'''
#include <atomic>
#include <cassert>
#include <string>
#include <vector>
constexpr int ERROR_INSTALL_PACKAGE_REJECTED=1625,ERROR_INSTALL_FAILURE=1603;
int deleted=0,removedDirs=0;
void DeleteFileW(const wchar_t*){++deleted;}
void RemoveDirectoryW(const wchar_t*){++removedDirs;}
// WORKSPACE
struct State {bool keptOk=true,removedPresent=true,commitOk=true,restoreOk=true,linkExists=true;
 int commits=0,restores=0,rechecks=0;bool expectedPresent=false;} f;
namespace Msi {bool IsProductInstalled(const wchar_t*){return f.removedPresent;}}
namespace GlassInstallRemoval {
struct Session {bool shortcutRetargetAttempted=false;};
bool Commit(Session& s,bool,std::wstring&){++f.commits;s.shortcutRetargetAttempted=f.linkExists;return f.commitOk;}
bool Restore(Session& s,bool,std::wstring& detail){++f.restores;if(f.restoreOk)s.shortcutRetargetAttempted=false;else detail=L"restore failure";return f.restoreOk;}
}
namespace GlassRegisteredMsi {
bool Recheck(const wchar_t*,const std::wstring&,bool expected,const std::wstring&,const std::wstring&,std::wstring& detail,const std::atomic_bool* cancel){
 assert(cancel==nullptr);++f.rechecks;f.expectedPresent=expected;if(!f.keptOk)detail=L"changed MSI baseline";return f.keptOk;
}
}
struct Test {
 struct Probe {const wchar_t* code=L"kept";std::wstring root=L"kept-root",cached=L"kept.msi";bool installed=true;} probe;
 const Probe* keptSettingsProbe=&probe;const wchar_t* removedSettingsCode=L"removed";
 ProbeWorkspace workspace;GlassInstallRemoval::Session settingsShortcut;
 bool shortcutCommitted=false,settingsMsiAttempted=false;int restoredHosts=0;std::wstring error,log;
 Test(){workspace.directory=L"secured-workspace";}
 bool IsRu(){return false;}void SetError(const std::wstring& text){error=text;}
 std::wstring T(const wchar_t*,const wchar_t* en){return en;}
 void LogLine(const std::wstring& text){log=text;}bool restoreRemaining(){++restoredHosts;return true;}
 bool recheckKeptSettings(std::wstring& detail,const std::atomic_bool* cancel) // CHECK_BODY
 int Commit(){
 // COMMIT
 return 0;
 }
 bool Finish(){
 // FINISH
 return finishSettingsShortcut();
 }
};
int main(){
 {f={};Test t;t.keptSettingsProbe=nullptr;assert(t.Commit()==0&&t.Finish());assert(f.commits==0&&f.rechecks==0);}
 {f={};Test t;t.probe.installed=false;assert(t.Commit()==0);t.settingsMsiAttempted=true;assert(t.Finish());
  assert(f.commits==0&&f.rechecks==2&&!f.expectedPresent);}
 {f={};Test t;f.keptOk=false;assert(t.Commit()==ERROR_INSTALL_PACKAGE_REJECTED);assert(f.commits==0&&t.restoredHosts==1);assert(t.Finish());}
 {f={};Test t;f.commitOk=false;assert(t.Commit()==ERROR_INSTALL_FAILURE);assert(t.workspace.preserve&&t.restoredHosts==1);
  assert(t.Finish()&&f.restores==1&&!t.workspace.preserve&&!t.settingsShortcut.shortcutRetargetAttempted);}
 {f={};Test t;f.commitOk=false;f.removedPresent=false;assert(t.Commit()==ERROR_INSTALL_FAILURE);
  assert(t.Finish()&&f.restores==1); // Partial link write before any MSI, even when selected product was already absent.
 }
 {f={};Test t;assert(t.Commit()==0);assert(t.workspace.preserve); // Failure/cancel before selected Settings MSI.
  assert(t.Finish()&&f.restores==1&&!t.workspace.preserve);}
 {f={};Test t;assert(t.Commit()==0);t.settingsMsiAttempted=true; // MSI failed/rolled back, product still present.
  assert(t.Finish()&&f.restores==1);}
 {f={};Test t;assert(t.Commit()==0);t.settingsMsiAttempted=true;f.removedPresent=false;
  assert(t.Finish()&&f.restores==0&&!t.workspace.preserve); // Also when later ARP/cleanup failed: never resurrect broken target.
 }
 {f={};Test t;assert(t.Commit()==0);t.settingsMsiAttempted=true;f.restoreOk=false;
  assert(!t.Finish()&&t.workspace.preserve);assert(t.error.find(L"secured-workspace")!=std::wstring::npos);}
 {f={};Test t;assert(t.Commit()==0);t.settingsMsiAttempted=true;f.removedPresent=false;f.keptOk=false;
  assert(!t.Finish()&&t.workspace.preserve&&f.restores==0);assert(t.error.find(L"changed MSI baseline")!=std::wstring::npos);}
 {f={};Test t;f.linkExists=false;assert(t.Commit()==0);t.settingsMsiAttempted=true;f.removedPresent=false;
  assert(t.Finish()&&f.restores==0&&!t.workspace.preserve);}
 deleted=removedDirs=0;
 {ProbeWorkspace w;w.directory=L"private";w.files={L"cached.msi",L"old.lnk"};w.preserve=true;}
 assert(deleted==0&&removedDirs==0);
 {ProbeWorkspace w;w.directory=L"private";w.files={L"cached.msi",L"old.lnk"};}
 assert(deleted==2&&removedDirs==1);
}
'''.replace('// WORKSPACE', workspace).replace('// CHECK_BODY', check_body).replace('// COMMIT', commit).replace('// FINISH', finish)
with tempfile.TemporaryDirectory(prefix='GlassUninsShortcut-') as directory:
    cpp = Path(directory) / 'test.cpp'
    exe = Path(directory) / 'test'
    cpp.write_text(harness)
    subprocess.run([shutil.which('clang++') or shutil.which('g++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-pedantic', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS: actual Unins retarget boundary/finalizer: survivor/absence checks, pre-MSI commit, failed/partial MSI, restore refusal, retained target after later failure, secured recovery retention')
