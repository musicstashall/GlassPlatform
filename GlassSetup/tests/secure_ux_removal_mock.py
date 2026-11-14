"""Execute actual original-MSI removal snapshot/recheck with fake Windows APIs.
No real registry, MSI, signature/ACL enforcement or filesystem is exercised.
"""
from pathlib import Path
import subprocess, shutil, tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'GlassSetup/src/InstallSupport/GlassSecureUxPackage.cpp').read_text()
legacy=source[source.index('bool CheckLegacyRepackAbsent('):source.index('bool QueryInstallPresence(')]
presence=source[source.index('bool QueryInstallPresence('):source.index('bool Describe(')]
removal=source[source.index('namespace {\nbool RemovalInventory'):source.rfind('\n}')]
harness=r'''
#include <array>
#include <atomic>
#include <cassert>
#include <cwchar>
#include <iostream>
#include <map>
#include <string>
#include <vector>
using BYTE=unsigned char;using DWORD=unsigned;using UINT=unsigned;using HANDLE=int;using MSIINSTALLCONTEXT=int;
constexpr int INVALID_HANDLE_VALUE=-1,FALSE=0,INSTALLSTATE_DEFAULT=5,INSTALLSTATE_ABSENT=2,INSTALLSTATE_UNKNOWN=-1,ERROR_RETRY=1237,MSIINSTALLCONTEXT_NONE=0,
 MSIINSTALLCONTEXT_ALL=7,MSIINSTALLCONTEXT_MACHINE=4;
constexpr UINT ERROR_SUCCESS=0,ERROR_INVALID_DATA=13,ERROR_ACCESS_DENIED=5,ERROR_NOT_ENOUGH_MEMORY=8,
 GENERIC_READ=1,FILE_SHARE_READ=2,OPEN_EXISTING=3,FILE_FLAG_OPEN_REPARSE_POINT=4,
 FILE_ATTRIBUTE_REPARSE_POINT=1024,FILE_ATTRIBUTE_DIRECTORY=16;
constexpr const wchar_t* INSTALLPROPERTY_LOCALPACKAGE=L"LocalPackage",*INSTALLPROPERTY_PACKAGECODE=L"PackageCode";
#define _countof(a) (sizeof(a)/sizeof(a[0]))
#define _wcsicmp wcscasecmp
struct BY_HANDLE_FILE_INFORMATION {DWORD dwFileAttributes=0,nNumberOfLinks=1;};
struct FileData {std::string bytes="vendor";std::wstring code=L"official",version=L"4.0.0",package=L"package";};
struct State {
 std::vector<std::wstring> inventory{L"official"};std::map<std::wstring,FileData> files;
 std::wstring cache=L"C:\\Windows\\Installer\\vendor.msi",registeredPackage=L"package";
 bool query=true,legacy=false,contextSafe=true,legacyContextSafe=true,machine=true,trusted=true,copy=true,metadata=true,hash=true,clean=true;
 int installed=INSTALLSTATE_DEFAULT,handles=0,queries=0,copies=0;
 DWORD attributes=0,links=1;
} s;
std::atomic_bool cancel{false};
bool DeleteFileW(const wchar_t* path){assert(std::wstring(path).rfind(L"work\\",0)==0);if(!s.clean)return false;s.files.erase(path);return true;}
UINT GetWindowsDirectoryW(wchar_t* out,UINT){std::wcscpy(out,L"C:\\Windows");return 10;}
UINT MsiGetProductInfoW(const wchar_t*,const wchar_t* property,wchar_t* out,DWORD*){
 std::wcscpy(out,std::wstring(property)==INSTALLPROPERTY_LOCALPACKAGE?s.cache.c_str():s.registeredPackage.c_str());return 0;
}
int MsiQueryProductStateW(const wchar_t*){return s.installed;}
UINT MsiEnumProductsExW(const wchar_t*,const wchar_t*,int,DWORD,wchar_t*,int* context,void*,void*){*context=s.machine?4:1;return 0;}
HANDLE CreateFileW(const wchar_t* path,DWORD,DWORD,void*,DWORD,DWORD,void*){if(!s.files.count(path))return -1;++s.handles;return 1;}
bool GetFileInformationByHandle(HANDLE,BY_HANDLE_FILE_INFORMATION* info){info->dwFileAttributes=s.attributes;info->nNumberOfLinks=s.links;return true;}
namespace Shared::SecureUxThemeInventory {UINT Query(std::vector<std::wstring>& out){++s.queries;out=s.inventory;return s.query?0:5;}}
namespace GlassInstallMsi {
 enum class ProductPathCheck {Safe,Unsafe};
 constexpr const wchar_t* kProductSecureUxThemeX64=L"legacy64",*kProductSecureUxThemeX86=L"legacy32";
 bool IsProductInstalled(const wchar_t*){return s.legacy;}
 ProductPathCheck CheckProductContexts(const wchar_t* code,std::wstring&,const std::atomic_bool* =nullptr){return ((std::wstring(code)==L"legacy64"||std::wstring(code)==L"legacy32")?s.legacyContextSafe:s.contextSafe)?ProductPathCheck::Safe:ProductPathCheck::Unsafe;}
}
namespace GlassMaintenanceCache {
 bool TrustedDirectory(const std::wstring&){return s.trusted;}
 bool TrustedPath(const std::wstring&,bool){return s.trusted;}
 std::wstring PackageCode(const std::wstring& path){return s.files.at(path).package;}
}
namespace GlassRegisteredMsi {bool Hash(const std::wstring& path,std::array<BYTE,32>& out,const std::atomic_bool* cancellation){
 if(!s.hash||cancellation->load()||!s.files.count(path))return false;
 out={};for(unsigned i=0;i<s.files.at(path).bytes.size();++i)out[i%32]^=static_cast<BYTE>(s.files.at(path).bytes[i]);return true;
}}
namespace GlassSecureUxPackage {
struct Removal {std::wstring productCode,version,baseline;std::array<BYTE,32> digest{};bool ready=false;};
struct Package {std::wstring productCode,version;};
struct File {int h=-1;~File(){if(h!=-1)--s.handles;}};
bool Fail(std::wstring& error,const wchar_t* operation,DWORD=0){error=operation;return false;}
bool Cancelled(const std::atomic_bool& c,std::wstring& error){if(c.load()){error=L"cancel";return true;}return false;}

int NativeArchitecture(){return 0;}
bool Metadata(const std::wstring& path,int,Package& out,std::wstring& error){
 if(!s.metadata)return Fail(error,L"metadata");out.productCode=s.files.at(path).code;out.version=s.files.at(path).version;return true;
}
bool CopySnapshot(File&,const std::wstring& path,const std::atomic_bool& c,std::wstring&){
 ++s.copies;if(!s.copy||c.load()||s.files.count(path))return false;s.files[path]=s.files.at(s.cache);return true;
}
// SOURCE
}
void Reset(){s={};s.files[s.cache]={};cancel=false;}
int main(){
 using namespace GlassSecureUxPackage;std::wstring error;
 {Reset();s.legacyContextSafe=false;assert(!CheckLegacyRepackAbsent(error));
  Removal r;assert(!PrepareRemoval(L"work",L"official",cancel,r,error));assert(s.copies==0);
 }
 {std::vector<std::wstring> installed;
  Reset();assert(QueryInstallPresence(installed,cancel,error)&&installed==s.inventory);
  Reset();s.installed=INSTALLSTATE_ABSENT;assert(QueryInstallPresence(installed,cancel,error)&&installed.empty());assert(s.copies==0);
  Reset();s.installed=INSTALLSTATE_ABSENT;s.contextSafe=false;assert(!QueryInstallPresence(installed,cancel,error)&&installed.empty());assert(s.copies==0);
  Reset();s.query=false;assert(!QueryInstallPresence(installed,cancel,error)&&installed.empty());
  Reset();cancel=true;assert(!QueryInstallPresence(installed,cancel,error)&&installed.empty());
  Reset();s.installed=1;assert(!QueryInstallPresence(installed,cancel,error)&&installed.empty());
 }
 {Reset();Removal r;assert(PrepareRemoval(L"work",L"official",cancel,r,error));assert(r.ready&&s.handles==0);
  assert(RecheckRemoval(L"work",cancel,r,error));assert(s.files.count(r.baseline)&&s.files.size()==2&&s.handles==0);}
 for(unsigned test=0;test<13;++test){
  Reset();Removal r;
  if(test==0)s.inventory.clear();if(test==1)s.inventory.push_back(L"other");if(test==2)s.inventory={L"other"};
  if(test==3)s.legacy=true;if(test==4)s.contextSafe=false;if(test==5)s.machine=false;if(test==6)s.trusted=false;
  if(test==7)s.query=false;if(test==8)s.installed=1;if(test==9)s.cache=L"C:\\User\\vendor.msi";
  if(test==10)s.attributes=FILE_ATTRIBUTE_REPARSE_POINT;if(test==11)s.links=2;if(test==12)cancel=true;
  assert(!PrepareRemoval(L"work",L"official",cancel,r,error));assert(!r.ready&&s.handles==0);
 }
 for(unsigned test=0;test<5;++test){
  Reset();Removal r;if(test==0)s.copy=false;if(test==1)s.metadata=false;if(test==2)s.hash=false;
  if(test==3)s.files[s.cache].code=L"foreign";if(test==4)s.registeredPackage=L"different";
  assert(!PrepareRemoval(L"work",L"official",cancel,r,error));assert(!r.ready&&s.handles==0&&s.files.size()==1);
 }
 for(unsigned test=0;test<10;++test){
  Reset();Removal r;assert(PrepareRemoval(L"work",L"official",cancel,r,error));
  if(test==0)s.files[s.cache].bytes="changed";if(test==1)s.files[r.baseline].bytes="tampered";
  if(test==2)s.files[s.cache].version=L"5.0.0";if(test==3)s.inventory={L"other"};
  if(test==4)s.contextSafe=false;if(test==5)s.legacy=true;if(test==6)cancel=true;
  if(test==7)s.machine=false;if(test==8)s.registeredPackage=L"changed";if(test==9)s.hash=false;
  assert(!RecheckRemoval(L"work",cancel,r,error));assert(s.handles==0);
  assert(!s.files.count(L"work\\SecureUxTheme-removal-recheck.msi"));
 }
 {Reset();Removal r;assert(!RecheckRemoval(L"work",cancel,r,error));assert(s.copies==0);}
 {Reset();Removal r;assert(PrepareRemoval(L"work",L"official",cancel,r,error));s.clean=false;
  assert(!RecheckRemoval(L"work",cancel,r,error));assert(s.handles==0&&s.files.count(s.cache));}
 {Reset();Removal r;assert(PrepareRemoval(L"work",L"official",cancel,r,error));
  assert(!RecheckRemoval(L"other-work",cancel,r,error));}
 std::cout<<"PASS: actual vendor removal snapshot/recheck: exact single machine MSI, protected cache, context/metadata/PackageCode, links, pinned digest, inventory changes, cancellation and cleanup\n";
}
'''.replace('// SOURCE',legacy+presence+removal)
compiler=shutil.which('clang++') or shutil.which('g++')
if not compiler:raise SystemExit('clang++ or g++ required')
with tempfile.TemporaryDirectory(prefix='GlassSutRemoval-') as directory:
    path=Path(directory)/'removal.cpp';exe=Path(directory)/'removal'
    path.write_text(harness)
    subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-pedantic',str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
