"""Actual single-desktop-profile style gate/commit; fake registry/session/theme APIs.
No Windows style, registry, offline hive or MSI is modified by this test.
"""
from pathlib import Path
import shutil,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassProfileMaintenance.h').read_text()
def fn(token):
 a=s.index(token);b=s.index('{',a);d=0
 for i in range(b,len(s)):
  if s[i]=='{':d+=1
  elif s[i]=='}':
   d-=1
   if not d:return s[a:i+1]
 raise ValueError(token)
harness=r'''
#include <cassert>
#include <cwchar>
#include <string>
#include <vector>
#include <iostream>
using HKEY=void*;using DWORD=unsigned;using UINT=unsigned;using BYTE=unsigned char;
constexpr int KEY_READ=1,KEY_WRITE=2,REG_SZ=1,ERROR_SUCCESS=0;
#define _wcsicmp wcscasecmp
#define FAILED(x) ((x)<0)
namespace Shared::SystemVisualStyle {
 enum class State {Stock,ResetRequired,Unknown};State state=State::ResetRequired;
 State CurrentForMaintenance(){return state;}
 State ClassifyPath(const wchar_t* p){return std::wstring(p)==L"C:\\Windows\\Resources\\Themes\\Aero\\aero.msstyles"?State::Stock:State::ResetRequired;}
}
bool other=false,desktop=true,openAllOk=true,inventoryOk=true,keyOk=true,readOk=true,writeOk=true,verifyWrite=true,windowsOk=true,actualOk=true,pathSafe=true;
int profileCount=1,openKeys=0,writes=0,verifyCalls=0,inventoryFailAt=0;
std::wstring current=L"C:\\Users\\A\\custom.theme",actual=L"C:\\Windows\\Resources\\Themes\\Aero\\aero.msstyles";
bool OtherUserSessions(std::wstring& detail){if(other)detail=L"Other session";return other;}
bool IsDesktopProfile(const std::wstring&){return desktop;}
bool SafePath(const std::wstring&){return pathSafe;}
int RegOpenKeyExW(HKEY,const wchar_t*,int,int,HKEY* out){if(!keyOk)return 5;*out=reinterpret_cast<HKEY>(2);++openKeys;return 0;}
int RegCloseKey(HKEY){--openKeys;return 0;}
bool dllStock=true;
bool ReadString(HKEY,const wchar_t* name,std::wstring& out){if(std::wstring(name)==L"DllName"){out=dllStock?L"C:\\Windows\\Resources\\Themes\\Aero\\aero.msstyles":L"custom.msstyles";return readOk;}out=current;if(writes&&!verifyWrite)out=L"concurrent.theme";return readOk;}
int RegSetValueExW(HKEY,const wchar_t* name,int,int type,const BYTE* value,DWORD bytes){assert(std::wstring(name)==L"CurrentTheme"&&type==REG_SZ);++writes;
 if(!writeOk)return 5;
 current.assign(reinterpret_cast<const wchar_t*>(value),bytes/sizeof(wchar_t)-1);return 0;
}
UINT GetWindowsDirectoryW(wchar_t* out,UINT){if(!windowsOk)return 0;std::wcscpy(out,L"C:\\Windows");return 10;}
int GetCurrentThemeName(wchar_t* out,int,wchar_t*,int,wchar_t*,int){if(!actualOk)return -1;std::wcscpy(out,actual.c_str());return 0;}
class Inventory {
 struct Profile {std::wstring sid=L"sid";HKEY hive=reinterpret_cast<HKEY>(1);};
 std::vector<Profile> profiles_;
 bool removalStylePrepared_=false,removalStyleCommitted_=false;std::wstring removalTheme_;
 bool OpenAll(std::wstring&){profiles_.assign(profileCount,Profile{});return openAllOk;}
 bool VerifyInventory(std::wstring&)const{return inventoryOk&&++verifyCalls!=inventoryFailAt;}
public:
// METHODS
};
void reset(){assert(openKeys==0);dllStock=true;other=false;desktop=openAllOk=inventoryOk=keyOk=readOk=writeOk=verifyWrite=windowsOk=actualOk=pathSafe=true;
 profileCount=1;writes=verifyCalls=inventoryFailAt=0;current=L"C:\\Users\\A\\custom.theme";actual=L"C:\\Windows\\Resources\\Themes\\Aero\\aero.msstyles";Shared::SystemVisualStyle::state=Shared::SystemVisualStyle::State::ResetRequired;
}
int main(){std::wstring detail;
 {reset();Inventory i;assert(!i.CommitLoadedRemovalStyle(detail));assert(writes==0);}
 for(int failure=0;failure<10;++failure){reset();Inventory i;
  if(failure==0)other=true;
  if(failure==1)openAllOk=false;
  if(failure==2)profileCount=0;
  if(failure==3)profileCount=2;
  if(failure==4)desktop=false;
  if(failure==5)Shared::SystemVisualStyle::state=Shared::SystemVisualStyle::State::Unknown;
  if(failure==6)keyOk=false;
  if(failure==7)readOk=false;
  if(failure==8)inventoryOk=false;
  if(failure==9)inventoryFailAt=1;
  assert(!i.PreflightLoadedRemoval(detail));assert(!i.CommitLoadedRemovalStyle(detail));assert(writes==0);
 }
 for(int failure=0;failure<14;++failure){reset();Inventory i;assert(i.PreflightLoadedRemoval(detail));assert(writes==0);
  Shared::SystemVisualStyle::state=Shared::SystemVisualStyle::State::Stock;
  if(failure==0)other=true;
  if(failure==1)desktop=false;
  if(failure==2)inventoryOk=false;
  if(failure==3)Shared::SystemVisualStyle::state=Shared::SystemVisualStyle::State::Unknown;
  if(failure==4)current=L"changed.theme";
  if(failure==5)keyOk=false;
  if(failure==6)windowsOk=false;
  if(failure==7)actualOk=false;
  if(failure==8)actual=L"C:\\ThirdParty\\aero.msstyles";
  if(failure==9)pathSafe=false;
  if(failure==10)writeOk=false;
  if(failure==11)verifyWrite=false;
  if(failure==12)inventoryFailAt=verifyCalls+2;
  if(failure==13)dllStock=false;
  assert(!i.CommitLoadedRemovalStyle(detail));assert(writes==((failure>=10&&failure<=12)?1:0));
  assert(!i.CommitLoadedRemovalStyle(detail));
 }
 {reset();Inventory i;assert(i.PreflightLoadedRemoval(detail));assert(i.RecheckLoadedRemoval(detail));
  Shared::SystemVisualStyle::state=Shared::SystemVisualStyle::State::Stock;
  assert(i.CommitLoadedRemovalStyle(detail));assert(writes==1&&current==L"C:\\Windows\\Resources\\Themes\\aero.theme");
  assert(i.RecheckLoadedRemoval(detail));assert(!i.CommitLoadedRemovalStyle(detail));
  actual=L"C:\\ThirdParty\\custom.msstyles";assert(!i.RecheckLoadedRemoval(detail));
 }
 assert(openKeys==0);std::cout<<"PASS: actual loaded SUT profile/style gate: single desktop only, no preflight writes, rechecks, safe stock selector, one-shot and partial-failure refusal\n";
}
'''.replace('// METHODS','\n'.join(fn(t) for t in ['bool PreflightLoadedRemoval(','bool VerifyLoadedRemovalStock(','bool RecheckLoadedRemoval(','bool CommitLoadedRemovalStyle(']))
compiler=shutil.which('clang++') or shutil.which('g++')
with tempfile.TemporaryDirectory(prefix='GlassLoadedStyle-') as directory:
 p=Path(directory)/'test.cpp';exe=Path(directory)/'test';p.write_text(harness)
 subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pedantic','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
