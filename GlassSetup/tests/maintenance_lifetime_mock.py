"""Actual retirement/lease/volatile reboot-pin decisions with fake Windows APIs."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
life=(root/'GlassSetup/src/InstallSupport/GlassMaintenanceLifetime.h').read_text()
reboot=(root/'GlassSetup/src/InstallSupport/GlassMaintenanceReboot.h').read_text()
h=r'''
#include <cassert>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <cwchar>
#include <iostream>
using HANDLE=int;using DWORD=unsigned;using UINT=unsigned;using HKEY=void*;using PSECURITY_DESCRIPTOR=void*;
constexpr int INVALID_HANDLE_VALUE=-1,GENERIC_READ=1,GENERIC_WRITE=2,FILE_SHARE_READ=1,OPEN_ALWAYS=3,FILE_FLAG_OPEN_REPARSE_POINT=4;
constexpr int ERROR_ALREADY_EXISTS=183,ERROR_SUCCESS=0,ERROR_NO_MORE_ITEMS=259,ERROR_UNKNOWN_PRODUCT=1605,ERROR_FILE_NOT_FOUND=2,ERROR_PATH_NOT_FOUND=3,ERROR_NO_MORE_FILES=18;
constexpr int HKEY_LOCAL_MACHINE=1,REG_OPTION_VOLATILE=1,KEY_READ=1,KEY_WOW64_64KEY=256,SDDL_REVISION_1=1,FALSE=0,MSIINSTALLCONTEXT_ALL=7;
constexpr unsigned INVALID_FILE_ATTRIBUTES=~0u,FILE_ATTRIBUTE_DIRECTORY=16,FILE_ATTRIBUTE_REPARSE_POINT=1024;
struct SECURITY_ATTRIBUTES {size_t length;void* sd;int inherit;};
struct WIN32_FIND_DATAW {wchar_t cFileName[256];DWORD dwFileAttributes=16;};
bool trusted=true,busy=false,registryPresent=false,registryReadable=true,registryWritable=true,locked=false;int live=0,error_=2,registration=259;unsigned access_=0,share_=0;
std::set<std::wstring> dirs;std::map<std::wstring,std::string> files;std::vector<std::wstring> names;size_t index_=0;
HANDLE CreateFileW(const wchar_t*,unsigned access,unsigned share,void*,int,int,void*){access_=access;share_=share;if(busy)return -1;++live;return 42;}
bool CloseHandle(HANDLE){--live;return true;}int GetLastError(){return error_;}
DWORD GetFileAttributesW(const wchar_t* path){return dirs.count(path)?16:files.count(path)?0:INVALID_FILE_ATTRIBUTES;}
bool DeleteFileW(const wchar_t* path){return files.erase(path)>0;}
bool ConvertStringSecurityDescriptorToSecurityDescriptorW(const wchar_t*,int,void** out,void*){*out=reinterpret_cast<void*>(1);return true;}
void LocalFree(void*){}
int RegCreateKeyExW(int,const wchar_t*,int,void*,int options,int access,SECURITY_ATTRIBUTES* sa,HKEY* key,void*){
 assert(options==REG_OPTION_VOLATILE&&(access&KEY_WOW64_64KEY)&&sa->sd);*key=reinterpret_cast<void*>(2);if(!registryWritable)return 5;registryPresent=true;return 0;}
int RegOpenKeyExW(int,const wchar_t*,int,int access,HKEY* key){assert(access&KEY_WOW64_64KEY);*key=reinterpret_cast<void*>(2);return !registryReadable?5:registryPresent?0:2;}
void RegCloseKey(HKEY){}
namespace GlassInstallMsi {
constexpr auto kProductPlatformX64=L"a",kProductPlatformX86=L"b",kProductSettingsX64=L"c",kProductSettingsX86=L"d",kProductOpenGlass=L"e",kProductSecureUxThemeX64=L"f",kProductSecureUxThemeX86=L"g",kLegacyPlatformX64=L"h",kLegacyPlatformX86=L"i",kLegacyOpenGlass=L"j";
}
UINT MsiEnumProductsExW(const wchar_t*,const wchar_t*,int,int,wchar_t*,void*,void*,void*){return registration;}
namespace GlassInstallLog {void Line(const wchar_t*){}}
namespace GlassMaintenanceCache {
struct CacheLock {bool acquired=true;};
std::wstring Root(){return L"root";}bool EnsureRoot(){return trusted;}
bool HardenFile(const std::wstring&){return trusted;}bool TrustedPath(const std::wstring&,bool){return trusted;}bool TrustedDirectory(const std::wstring&){return trusted;}
bool ControllerRecordValid(const std::string& s){return s.size()==128&&s.find_first_not_of("0123456789abcdef")==std::string::npos;}
bool RemoveFlat(const std::wstring& path){if(locked)return false;dirs.erase(path);return true;}
}
void fill(WIN32_FIND_DATAW* data){std::wcscpy(data->cFileName,names[index_].c_str());}
HANDLE FindFirstFileW(const wchar_t*,WIN32_FIND_DATAW* data){names.clear();index_=0;
 for(const auto& dir:dirs)if(dir.rfind(L"root\\Installer\\",0)==0)names.push_back(dir.substr(15));
 if(names.empty()){error_=2;return -1;}fill(data);return 43;}
bool FindNextFileW(HANDLE,WIN32_FIND_DATAW* data){if(++index_>=names.size()){error_=18;return false;}fill(data);return true;}
void FindClose(HANDLE){}
bool fileProduct=false,rootsKnown=true,officialProduct=false;
namespace GlassInstallUtil {struct InstallRoots {std::wstring openGlass;}; bool ResolveInstallRoots(InstallRoots&){return rootsKnown;}}
namespace GlassOpenGlassInstall {bool HasProduct(){return officialProduct;}}
namespace GlassOpenGlassFiles {bool HasReceipt(const std::wstring&){return fileProduct;}}
// REBOOT
// LIFE
void reset(){assert(live==0);GlassMaintenanceReboot::pinRequested=false;trusted=registryReadable=registryWritable=true;busy=registryPresent=locked=false;registration=259;files.clear();dirs={L"root\\Installer",L"root\\Installer\\"+std::wstring(64,L'a'),L"root\\Current"};files[L"root\\Installer\\Active"]="record";}
int main(){using namespace GlassMaintenanceLifetime;
 reset();{Lease shared;assert(shared.Acquire()&&share_==FILE_SHARE_READ&&access_==GENERIC_READ);}assert(live==0);
 reset();fileProduct=true;assert(!RetireUnused());fileProduct=false;rootsKnown=false;assert(!RetireUnused());rootsKnown=true;
 reset();officialProduct=true;assert(!RetireUnused());officialProduct=false;
 reset();registration=0;assert(!RetireUnused()&&files.count(L"root\\Installer\\Active"));
 registration=5;assert(!RetireUnused()); // Incomplete registration discovery is not absence.
 reset();busy=true;assert(!RetireUnused()&&files.count(L"root\\Installer\\Active"));
 reset();assert(GlassMaintenanceReboot::Pinned(true)&&registryPresent);assert(!RetireUnused());
 registryPresent=false;GlassMaintenanceReboot::pinRequested=false; // Simulated reboot discards volatile HKLM key.
 assert(RetireUnused()&&!files.count(L"root\\Installer\\Active")&&share_==0);
 reset();registryReadable=false;assert(!RetireUnused());
 reset();locked=true;assert(!RetireUnused()&&!files.count(L"root\\Installer\\Active")); // No dangling authoritative pointer into partly deleted code.
 locked=false;assert(RetireUnused());
 reset();trusted=false;assert(!RetireUnused()&&files.count(L"root\\Installer\\Active"));
 std::cout<<"PASS: actual lifetime policy: shared/exclusive cross-process lease, registration/unknown refusal, volatile reboot pin, safe pointer revocation, locked-code retry and cleanup independence\n";
}
'''.replace('// REBOOT',reboot[reboot.index('namespace GlassMaintenanceReboot'):]).replace('// LIFE',life[life.index('namespace GlassMaintenanceLifetime'):])
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
