#!/usr/bin/env python3
"""Check real rollback task/service cleanup refuses changed backends."""
from pathlib import Path
import os
import subprocess
import tempfile

source=(Path(__file__).resolve().parents[2]/'GlassSetup/src/InstallEngine.cpp').read_text(encoding='utf-8')
start=source.index('bool InstallEngine::RemoveCreatedHostsForRollback(');brace=source.index('{',start);depth=0
for end in range(brace,len(source)):
    if source[end]=='{':depth+=1
    if source[end]=='}':
        depth-=1
        if depth==0:break
body=source[start:end+1]
prefix=r'''
#include <cassert>
#include <iostream>
#include <string>
struct Fake{bool task64=true,task86=true,service=true;int taskChecks=0,serviceChecks=0,taskDeletes=0,serviceDeletes=0;} fake;
constexpr int LANG_RUSSIAN=25;int GetUserDefaultUILanguage(){return LANG_RUSSIAN;}int PRIMARYLANGID(int x){return x;}
namespace GP{enum class PlatformArch{X64,X86};void InvalidateGlassAutorunTaskCache(){}}
namespace InstallLog {template<class... T>void LineFormat(const wchar_t*,T...) {}}
namespace GlassInstallRemoval {
 bool ValidatePlatformTask(GP::PlatformArch arch,const std::wstring&,bool,bool,bool,std::wstring&){++fake.taskChecks;
  return arch==GP::PlatformArch::X64?fake.task64:fake.task86;}
 bool ValidateOpenGlassPaths(const std::wstring&,void*,bool,std::wstring&){++fake.serviceChecks;return fake.service;}
}
namespace GlassInstallHost {
 bool RemovePlatformAutorunTasks(bool,bool){++fake.taskDeletes;return true;}
 bool RemoveOpenGlassServiceAndTasks(bool){++fake.serviceDeletes;return true;}
}
class InstallEngine{public:
 bool x64TaskCreatedBySetup_=true,x86TaskCreatedBySetup_=true,ogServiceCreatedBySetup_=true;
 std::wstring x64Dir_=L"64",x86Dir_=L"86",openGlassDir_=L"og";
 bool RemoveCreatedHostsForRollback() noexcept;
};
'''
main=r'''
int main(){auto reset=[](){fake=Fake{};};
 {reset();InstallEngine e;assert(e.RemoveCreatedHostsForRollback());
  assert(fake.taskChecks==2 && fake.serviceChecks==1 && fake.taskDeletes==1 && fake.serviceDeletes==1);}
 {reset();fake.task64=false;InstallEngine e;assert(!e.RemoveCreatedHostsForRollback());
  assert(fake.taskDeletes==0 && fake.serviceDeletes==0);}
 {reset();fake.service=false;InstallEngine e;assert(!e.RemoveCreatedHostsForRollback());
  assert(fake.taskChecks==2 && fake.serviceChecks==1 && fake.taskDeletes==0 && fake.serviceDeletes==0);}
 {reset();InstallEngine e;e.x64TaskCreatedBySetup_=e.x86TaskCreatedBySetup_=e.ogServiceCreatedBySetup_=false;
  assert(e.RemoveCreatedHostsForRollback() && fake.taskDeletes==0 && fake.serviceDeletes==0);}
 std::cout<<"PASS: rollback validates created tasks and service immediately before deleting them\n";
}
'''
with tempfile.TemporaryDirectory(prefix='glass-rollback-hosts-') as directory:
    cpp=Path(directory)/'test.cpp';exe=Path(directory)/'test'
    cpp.write_text(prefix+body+'\n'+main,encoding='utf-8')
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
