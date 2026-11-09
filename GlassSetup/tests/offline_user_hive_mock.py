"""Actual fallback loader/privilege RAII with fake registry/token APIs."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassOfflineUserHive.h').read_text();s=s[s.index('namespace GlassOfflineUserHive {'):]
h=r'''
#include <string>
#include <cassert>
#include <cwchar>
#include <map>
#include <iostream>
using DWORD=unsigned;using LONG=long;using HANDLE=int*;using HKEY=void*;
constexpr int FALSE=0,ERROR_SUCCESS=0,ERROR_PRIVILEGE_NOT_HELD=1314,TOKEN_QUERY=1,TOKEN_ADJUST_PRIVILEGES=2,SE_PRIVILEGE_ENABLED=2,KEY_READ=1,KEY_WRITE=2;
constexpr const wchar_t* SE_BACKUP_NAME=L"backup",*SE_RESTORE_NAME=L"restore";
HKEY HKEY_USERS=reinterpret_cast<HKEY>(1);
struct LUID {int id;};struct TOKEN_PRIVILEGES {DWORD PrivilegeCount=0;struct {LUID Luid{};DWORD Attributes=0;}Privileges[1];};
struct GUID {int id=1;};
#define FAILED(hr) ((hr)<0)
int error=0,tokens=0,loads=0,unloads=0,opens=0,restores=0;bool tokenOk=true,privilegeOk=true,guidOk=true,stringOk=true;
LONG loadResult=0,openResult=0,unloadResult=0;bool mounted=false;
void SetLastError(int e){error=e;}int GetLastError(){return error;}
HANDLE GetCurrentProcess(){return nullptr;}
bool OpenProcessToken(HANDLE,int,HANDLE* out){if(!tokenOk)return false;*out=new int(0);++tokens;return true;}
bool LookupPrivilegeValueW(void*,const wchar_t* name,LUID* out){out->id=std::wstring(name)==L"backup"?1:2;return true;}
bool AdjustTokenPrivileges(HANDLE,int,TOKEN_PRIVILEGES*,DWORD bytes,TOKEN_PRIVILEGES* previous,DWORD*){
 if(bytes){previous->PrivilegeCount=1;error=privilegeOk?0:1300;}else ++restores;
 return true;
}
void CloseHandle(HANDLE h){delete h;--tokens;}
LONG CoCreateGuid(GUID*){return guidOk?0:-1;}
int StringFromGUID2(GUID,wchar_t* text,int){std::wcscpy(text,L"{test-guid}");return stringOk?12:0;}
LONG RegLoadKeyW(HKEY root,const wchar_t* name,const wchar_t* path){assert(root==HKEY_USERS&&std::wstring(name).find(L"GlassSetup-Offline-")==0&&std::wstring(path)==L"C:\\Users\\A\\NTUSER.DAT");++loads;if(!loadResult)mounted=true;return loadResult;}
LONG RegOpenKeyExW(HKEY,const wchar_t*,int,int,HKEY* out){++opens;if(!openResult)*out=reinterpret_cast<HKEY>(2);return openResult;}
LONG RegUnLoadKeyW(HKEY root,const wchar_t* name){assert(root==HKEY_USERS&&std::wstring(name).find(L"GlassSetup-Offline-")==0);++unloads;if(!unloadResult)mounted=false;return unloadResult;}
namespace GlassInstallLog {template<class... T>void LineFormat(const wchar_t*,T...) {}}
// SOURCE
void reset(){assert(tokens==0&&!mounted);tokenOk=privilegeOk=guidOk=stringOk=true;loadResult=openResult=unloadResult=0;loads=unloads=opens=restores=0;}
int main(){using namespace GlassOfflineUserHive;
 std::wstring name,detail;HKEY hive=nullptr;
 reset();assert(LoadMounted(L"C:\\Users\\A\\NTUSER.DAT",hive,name,detail));assert(mounted&&hive&&tokens==0&&restores==2);assert(Unmount(name));assert(!mounted&&unloads==1&&tokens==0);
 for(int failure=0;failure<6;++failure){reset();hive=nullptr;name.clear();
  if(failure==0)tokenOk=false;if(failure==1)privilegeOk=false;if(failure==2)guidOk=false;if(failure==3)stringOk=false;if(failure==4)loadResult=1009;if(failure==5)openResult=5;
  assert(!LoadMounted(L"C:\\Users\\A\\NTUSER.DAT",hive,name,detail));assert(!mounted&&hive==nullptr&&tokens==0&&name.empty());
  assert(unloads==(failure==5?1:0)); // Never unload a name we failed to load.
 }
 reset();openResult=5;unloadResult=5;assert(!LoadMounted(L"C:\\Users\\A\\NTUSER.DAT",hive,name,detail));assert(mounted&&!name.empty()&&tokens==0);unloadResult=0;assert(Unmount(name));assert(!mounted);
 reset();assert(Unmount(L""));assert(unloads==0);
 std::cout<<"PASS: actual offline hive fallback, privilege restoration, unique ownership, load/open failures and cleanup retry; no existing SID mount unload\n";
}
'''.replace('// SOURCE',s)
with tempfile.TemporaryDirectory(prefix='OfflineHive-') as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(h)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
