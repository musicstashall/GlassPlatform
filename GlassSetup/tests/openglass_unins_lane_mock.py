#!/usr/bin/env python3
"""Compile actual GlassUnins vendor dispatch with mocks; no Windows execution."""
from pathlib import Path
import subprocess,tempfile
import xml.etree.ElementTree as ET
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/GlassUnins/main.cpp').read_text()
a=s.index('    int RunOfficialOpenGlassRemoval(');b=s.index('    int RunOperation(',a)
method=s[a:b]
operation=s[b:s.index('        // Re-read presence',b)]
assert operation.index('RunOfficialOpenGlassRemoval(chosen)')<operation.index('if (chosen.cleanupSettings')
for token in ['StopAllForUninstall(', 'UninstallProduct(', 'PruneEmptyDirectories(', 'RefreshManagedArp(', 'RemoveOpenGlassService']:
    assert token not in method
for project,prefix in [('GlassSetup/GlassSetup.vcxproj','src\\InstallSupport\\'),('GlassSetup/GlassUnins/GlassUnins.vcxproj','..\\src\\InstallSupport\\')]:
    xml=ET.parse(root/project);ns={'m':'http://schemas.microsoft.com/developer/msbuild/2003'}
    assert sum(n.attrib.get('Include')==prefix+'GlassOpenGlassUninstall.cpp' for n in xml.findall('.//m:ClCompile',ns))==1
h=r'''
#include <string>
#include <atomic>
#include <cassert>
#include <initializer_list>
using DWORD=unsigned;using WPARAM=unsigned long;
constexpr int ERROR_SUCCESS=0,ERROR_INSTALL_FAILURE=1603,ERROR_SUCCESS_REBOOT_REQUIRED=3010,ERROR_INSTALL_PACKAGE_REJECTED=1625,ERROR_INSTALL_USEREXIT=1602,INSTALLSTATE_UNKNOWN=-1;
bool queryOk=true,same=true,receipt=false,registered=false,runnerOk=true,afterInstalled=false;int queryCount=0,runs=0,pins=0;DWORD code=0;
namespace GlassOpenGlassInstall{
struct Record{bool installed=true;std::wstring directory=L"D:\\OG",version=L"3.0.2.3749";};
bool Query(Record& r,std::wstring&){++queryCount;if(queryCount>1)r.installed=afterInstalled;return queryOk;}
bool Same(const Record&,const Record&){return same;}}
namespace Msi {constexpr auto kProductOpenGlass=L"og",kLegacyOpenGlass=L"old";}
int MsiQueryProductStateW(const wchar_t*){return registered?5:INSTALLSTATE_UNKNOWN;}
namespace GlassOpenGlassFiles{bool HasReceipt(const std::wstring&){return receipt;}}
namespace GlassOpenGlassPackage{bool RunUninstaller(const GlassOpenGlassInstall::Record&,DWORD& c,std::wstring&){++runs;c=code;return runnerOk;}}
namespace GlassMaintenanceReboot{bool Pinned(bool){++pins;return true;}}
struct Selection{bool x64=false,x86=false,settings=false,settings64=false,settings32=false,secureUxTheme=false,cleanupSettings=false;};
enum class OperationState{BeforeRemoval,Removing,CancelRequested};enum class Page{Removing};
constexpr int kWorkerHold=1;
void PostMessageW(int,int,WPARAM,int){}
struct App{
 int window=0;std::wstring openGlassDir_,error,status;GlassOpenGlassInstall::Record officialOpenGlass_;
 std::atomic<OperationState> operationState_{OperationState::BeforeRemoval};std::atomic_bool removalStarted_{false};
 std::wstring T(const wchar_t* ru,const wchar_t*){return ru;}void SetError(const std::wstring& e){error=e;}void SetStatus(const std::wstring& s,int){status=s;}
// METHOD
};
void reset(){queryOk=same=runnerOk=true;receipt=registered=afterInstalled=false;queryCount=runs=pins=0;code=0;}
int main(){
 for(unsigned exit:{0u,3010u}){reset();code=exit;App app;Selection choice;assert(app.RunOfficialOpenGlassRemoval(choice)==static_cast<int>(exit));assert(runs==1&&pins==(exit==3010)&&app.removalStarted_);}
 for(bool Selection::* f:{&Selection::x64,&Selection::x86,&Selection::settings,&Selection::settings64,&Selection::settings32,&Selection::secureUxTheme,&Selection::cleanupSettings}){
 reset();App app;Selection choice;choice.*f=true;assert(app.RunOfficialOpenGlassRemoval(choice)==1625&&runs==0);}
 for(bool* flag:{&receipt,&registered}){reset();*flag=true;App app;assert(app.RunOfficialOpenGlassRemoval({})==1625&&runs==0);}
 for(bool* flag:{&queryOk,&same}){reset();*flag=false;App app;assert(app.RunOfficialOpenGlassRemoval({})==1625&&runs==0);}
 reset();App cancelled;cancelled.operationState_=OperationState::CancelRequested;assert(cancelled.RunOfficialOpenGlassRemoval({})==1602&&runs==0);
 reset();runnerOk=false;App failed;assert(failed.RunOfficialOpenGlassRemoval({})==1603&&!failed.error.empty());
 reset();afterInstalled=true;code=3010;App remains;assert(remains.RunOfficialOpenGlassRemoval({})==1603&&pins==0);
 reset();code=2;App vendorFail;assert(vendorFail.RunOfficialOpenGlassRemoval({})==2);
}
'''.replace('// METHOD',method)
with tempfile.TemporaryDirectory() as tmp:
    cpp=Path(tmp)/'lane.cpp';exe=Path(tmp)/'lane';cpp.write_text(h)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: actual GlassUnins vendor lane: standalone policy/cancel boundary/inventory/result/reboot; shared build graph')
