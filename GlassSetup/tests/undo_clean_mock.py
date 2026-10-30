#!/usr/bin/env python3
"""Exercise clean-install rollback control flow with fake Windows operations."""
from pathlib import Path
import os
import subprocess
import tempfile

repo=Path(__file__).resolve().parents[2]
source=(repo/'GlassSetup/src/InstallEngine.cpp').read_text(encoding='utf-8')
start=source.index('bool InstallEngine::UndoCleanInstall(');brace=source.index('{',start);depth=0
for end in range(brace,len(source)):
    if source[end]=='{':depth+=1
    if source[end]=='}':
        depth-=1
        if depth==0:break
undo=source[start:end+1]
prefix=r'''
#include <atomic>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
struct Fake {bool stop=true,uninstall=true,ambiguous=false,unexpected=false,createdSafe=true;int uninstalls=0,dirs=0,registry=0,services=0,tasks=0,shortcuts=0;} fake;
void AcquireSRWLockExclusive(int*){} void ReleaseSRWLockExclusive(int*){}
int RemoveDirectoryW(const wchar_t*){++fake.dirs;return 1;}
namespace InstallLog{void Line(const wchar_t*){} template<class... T>void LineFormat(const wchar_t*,T...) {}}
namespace WizardProgress{constexpr int kMaxUnits=10000;}
namespace GlassInstallMsi {
 constexpr wchar_t kProductPlatformX64[]=L"p64",kProductPlatformX86[]=L"p86",kProductSettingsX64[]=L"gs64",
  kProductSettingsX86[]=L"gs86",kProductOpenGlass[]=L"og",kLegacyPlatformX64[]=L"old64",
  kLegacyPlatformX86[]=L"old86",kLegacyOpenGlass[]=L"oldog";
 enum class ProductPathCheck {Safe,Unverifiable};
 ProductPathCheck CheckProductContexts(const wchar_t*,std::wstring&){return fake.ambiguous?ProductPathCheck::Unverifiable:ProductPathCheck::Safe;}
 bool IsProductInstalled(const wchar_t*){return fake.unexpected;}
}
namespace GlassInstallRollback {
 struct SessionState{std::vector<std::wstring> installedThisSession;};
 bool UninstallSessionProductSafely(SessionState&,const wchar_t*){++fake.uninstalls;return fake.uninstall;}
 bool RestoreRegistryValues(const SessionState&){++fake.registry;return true;}
}
namespace GlassInstallUtil {
 bool InvokeShellExtRegistration(bool,const std::wstring&,const std::wstring&){return true;}
 void DeleteStartMenuShortcut(){++fake.shortcuts;}
}
namespace GlassInstallHost {
 bool RemoveOpenGlassServiceAndTasks(bool){++fake.services;return true;}
 bool RemovePlatformAutorunTasks(bool,bool){++fake.tasks;return true;}
}
class InstallEngine {
public:
 struct Labels {std::wstring rollback=L"rollback",rollbackRemoved=L"removed",rollbackFailed=L"failed";} labels_;
 struct Prior {bool shortcutExisted=false;} prior_;
 GlassInstallRollback::SessionState rollbackSession_;
 std::atomic_bool progressReverse_{false};int statusLock_=0;
 std::wstring x64Dir_=L"x64",x86Dir_=L"x86",openGlassDir_=L"og",rollbackNote_,failureDetail_,error_;
 bool shellRegisteredBySetup_=false,ogServiceCreatedBySetup_=true,x64TaskCreatedBySetup_=true,x86TaskCreatedBySetup_=true;
 bool StopHostsForRollback(){return fake.stop;}
 bool RemoveCreatedHostsForRollback(){if(!fake.createdSafe)return false;
  if(ogServiceCreatedBySetup_)++fake.services;if(x64TaskCreatedBySetup_||x86TaskCreatedBySetup_)++fake.tasks;return true;}
 void SetStatus(const std::wstring&,int){}
 bool UndoCleanInstall() noexcept;
};
'''
main=r'''
int main(){auto reset=[](){fake=Fake{};};
 {reset();InstallEngine e;e.rollbackSession_.installedThisSession={L"p64",L"gs64"};assert(e.UndoCleanInstall());
  assert(fake.uninstalls==2 && fake.dirs==3 && fake.registry==1 && fake.services==1 && fake.tasks==1);}
 {reset();fake.uninstall=false;InstallEngine e;e.rollbackSession_.installedThisSession={L"p64",L"gs64"};
  assert(!e.UndoCleanInstall());assert(fake.uninstalls==1 && fake.dirs==0 && fake.registry==0 && fake.services==0 && fake.tasks==0);}
 {reset();fake.unexpected=true;InstallEngine e;assert(!e.UndoCleanInstall());
  assert(fake.dirs==0 && fake.registry==0 && fake.services==0 && fake.tasks==0);}
 {reset();fake.ambiguous=true;InstallEngine e;assert(!e.UndoCleanInstall());assert(fake.dirs==0 && fake.registry==0);}
 {reset();fake.createdSafe=false;InstallEngine e;assert(!e.UndoCleanInstall());
  assert(fake.dirs==0 && fake.registry==0 && fake.services==0 && fake.tasks==0);}
 {reset();fake.stop=false;InstallEngine e;e.rollbackSession_.installedThisSession={L"p64"};assert(!e.UndoCleanInstall());
  assert(fake.uninstalls==0 && fake.dirs==0 && fake.registry==0);}
 std::cout<<"PASS: clean rollback stops on unsafe/unexpected MSI and retains remaining state\n";
}
'''
with tempfile.TemporaryDirectory(prefix='glass-undo-clean-') as directory:
    cpp=Path(directory)/'test.cpp';exe=Path(directory)/'test'
    cpp.write_text(prefix+undo+'\n'+main,encoding='utf-8')
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
