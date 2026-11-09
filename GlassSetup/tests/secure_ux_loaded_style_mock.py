"""Actual loaded-user registry-only removal methods with fake registry/theme APIs."""
from pathlib import Path
import subprocess,tempfile
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
#include <map>
#include <iostream>
using HKEY=void*;using LONG=long;using DWORD=unsigned;using UINT=unsigned;using BYTE=unsigned char;
constexpr int KEY_READ=1,KEY_WRITE=2,REG_SZ=1,ERROR_SUCCESS=0,ERROR_FILE_NOT_FOUND=2;
#define _wcsicmp wcscasecmp
#define FAILED(x) ((x)<0)
bool missingKeys=false;bool openAllOk=true,inventoryOk=true,windowsOk=true,pathSafe=true,actualOk=true,readOk=true;
int profileCount=3,openKeys=0,writes=0,failWriteAt=0,denyProfile=0,failCreateAt=0,creates=0,inventoryCalls=0,failInventoryAt=0;
std::wstring actual=L"C:\\Windows\\Resources\\Themes\\Aero\\aero.msstyles";
std::map<std::pair<int,std::wstring>,std::wstring> registry;
int number(HKEY key){return static_cast<int>(reinterpret_cast<size_t>(key));}
bool SafePath(const std::wstring&){return pathSafe;}
LONG RegOpenKeyExW(HKEY hive,const wchar_t*,int,int,HKEY* out){if(missingKeys)return ERROR_FILE_NOT_FOUND;if(number(hive)==denyProfile)return 5;*out=hive;++openKeys;return 0;}
LONG RegCreateKeyExW(HKEY hive,const wchar_t*,int,void*,int,int,void*,HKEY* out,void*){if(++creates==failCreateAt)return 5;if(number(hive)==denyProfile)return 5;*out=hive;++openKeys;return 0;}
int RegCloseKey(HKEY){--openKeys;return 0;}
bool ReadString(HKEY key,const wchar_t* name,std::wstring& out){out=registry[{number(key),name}];return readOk&&!out.empty();}
LONG RegSetValueExW(HKEY key,const wchar_t* name,int,int type,const BYTE* value,DWORD bytes){assert(type==REG_SZ);if(++writes==failWriteAt)return 5;
 assert(std::wstring(name)==L"CurrentTheme"||std::wstring(name)==L"DllName");
 registry[{number(key),name}].assign(reinterpret_cast<const wchar_t*>(value),bytes/sizeof(wchar_t)-1);return 0;
}
UINT GetWindowsDirectoryW(wchar_t* out,UINT){if(!windowsOk)return 0;std::wcscpy(out,L"C:\\Windows");return 10;}
int GetCurrentThemeName(wchar_t* out,int,wchar_t*,int,wchar_t*,int){if(!actualOk)return -1;std::wcscpy(out,actual.c_str());return 0;}
class Inventory {
 struct Profile {std::wstring sid;HKEY hive;};
 std::vector<Profile> profiles_;
 bool removalStylePrepared_=false,removalStyleCommitted_=false;
 bool OpenAll(std::wstring&,bool skipDeleted=false){assert(skipDeleted);profiles_.clear();for(int i=1;i<=profileCount;++i)profiles_.push_back({L"SID-"+std::to_wstring(i),reinterpret_cast<HKEY>(static_cast<size_t>(i))});return openAllOk;}
 bool VerifyInventory(std::wstring&)const{return inventoryOk&&++inventoryCalls!=failInventoryAt;}
public:
// METHODS
};
void reset(){assert(openKeys==0);missingKeys=false;openAllOk=inventoryOk=windowsOk=pathSafe=actualOk=readOk=true;profileCount=3;writes=creates=inventoryCalls=0;failWriteAt=denyProfile=failCreateAt=failInventoryAt=0;
 actual=L"C:\\Windows\\Resources\\Themes\\Aero\\aero.msstyles";registry.clear();}
int main(){std::wstring detail;
 for(int count:{0,1,3,7}){reset();profileCount=count;Inventory i;assert(i.PreflightLoadedRemoval(detail));assert(writes==0);assert(i.RecheckLoadedRemoval(detail));
  assert(i.CommitLoadedRemovalStyle(detail));assert(writes==2*count);assert(i.RecheckLoadedRemoval(detail));assert(!i.CommitLoadedRemovalStyle(detail));
  for(int p=1;p<=count;++p){assert((registry[{p,L"CurrentTheme"}]==L"C:\\Windows\\Resources\\Themes\\aero.theme"));assert((registry[{p,L"DllName"}]==L"C:\\Windows\\Resources\\Themes\\Aero\\aero.msstyles"));}
  if(count){registry[{count,L"DllName"}]=L"custom.msstyles";assert(!i.RecheckLoadedRemoval(detail));}
 }
 {reset();Inventory i;missingKeys=true;assert(i.PreflightLoadedRemoval(detail));assert(writes==0);missingKeys=false;assert(i.CommitLoadedRemovalStyle(detail));assert(writes==6);}
 for(int failure:{0,2}){reset();Inventory i;if(failure==0)openAllOk=false;if(failure==1)profileCount=0;if(failure==2)denyProfile=2;assert(!i.PreflightLoadedRemoval(detail));assert(writes==0);}
 for(int at=1;at<=6;++at){reset();Inventory i;assert(i.PreflightLoadedRemoval(detail));failWriteAt=at;assert(!i.CommitLoadedRemovalStyle(detail));assert(writes==at);assert(detail.find(L"SID-")!=std::wstring::npos);assert(!i.CommitLoadedRemovalStyle(detail));}
 for(int failure=0;failure<7;++failure){reset();Inventory i;assert(i.PreflightLoadedRemoval(detail));
  if(failure==0)inventoryOk=false;if(failure==1)windowsOk=false;if(failure==2)pathSafe=false;if(failure==3)failCreateAt=3;
  if(failure==4)readOk=false;if(failure==5)actual=L"C:\\ThirdParty\\custom.msstyles";if(failure==6)failInventoryAt=inventoryCalls+2;
  assert(!i.CommitLoadedRemovalStyle(detail));assert(!i.RecheckLoadedRemoval(detail)||failure==0);
 }
 {reset();Inventory i;assert(i.PreflightLoadedRemoval(detail));actual=L"C:\\Windows\\Resources\\Themes\\Aero\\aerolite.msstyles";assert(i.CommitLoadedRemovalStyle(detail));}
 assert(openKeys==0);std::cout<<"PASS: actual loaded-user SUT selectors: zero/multiple loaded hives, no session/SID restriction, no preflight writes, two stock selectors per profile, partial failures and rechecks\n";
}
'''.replace('// METHODS','\n'.join(fn(t) for t in ['bool PreflightLoadedRemoval(','bool StockRemovalPaths(','bool VerifyLoadedRemovalStock(','bool RecheckLoadedRemoval(','bool CommitLoadedRemovalStyle(']))
with tempfile.TemporaryDirectory(prefix='GlassLoadedStyle-') as directory:
 p=Path(directory)/'test.cpp';exe=Path(directory)/'test';p.write_text(harness)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-pedantic','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
