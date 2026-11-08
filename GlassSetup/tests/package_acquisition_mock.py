"""Production Prepare/CopyLocked against fake files, MSI inventory and release IO.
Windows metadata/signature APIs remain separate seams; full sources also cross-compile.
"""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'GlassSetup/src/InstallSupport/GlassPackageAcquisition.cpp').read_text()
prepare=source[source.index('bool Prepare('):source.rfind('\n}')]
start=source.index('bool CopyLocked(')
copy=source[start:source.index('\n}\n}',start)+2]
prefix=r'''
#include "GlassSetup/src/InstallSupport/GlassPackageCatalog.h"
#include <cassert>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <array>
using DWORD=unsigned long;using BYTE=unsigned char;using BOOL=int;
using HANDLE=void*; constexpr BOOL FALSE=0;constexpr HANDLE invalid(){return reinterpret_cast<HANDLE>(-1);}
#define INVALID_HANDLE_VALUE invalid()
#define _wcsicmp wcscasecmp
constexpr DWORD GENERIC_READ=1,GENERIC_WRITE=2,FILE_SHARE_READ=4,OPEN_EXISTING=1,CREATE_NEW=2,
 FILE_FLAG_OPEN_REPARSE_POINT=8,FILE_ATTRIBUTE_NORMAL=0,FILE_ATTRIBUTE_DIRECTORY=16,FILE_ATTRIBUTE_REPARSE_POINT=32,FILE_BEGIN=0;
struct LARGE_INTEGER{long long QuadPart=0;};
struct BY_HANDLE_FILE_INFORMATION{DWORD dwFileAttributes=0,nNumberOfLinks=1;};
struct File{std::string bytes;DWORD attr=0,links=1;};
struct Open{std::wstring path;std::size_t pos=0;};
struct State{std::map<std::wstring,File> files;int handles=0,downloads=0;bool installed=false,workspaceTrusted=true,downloadFails=false,cancelDownload=false,writeFails=false;std::wstring installedVersion=L"1.0.0";std::string download; } s;
std::atomic<bool> cancelled{false};
File& F(HANDLE h){return s.files.at(static_cast<Open*>(h)->path);}
HANDLE CreateFileW(const wchar_t* path,DWORD,DWORD,void*,DWORD mode,DWORD,void*){
 auto found=s.files.find(path);if((mode==OPEN_EXISTING && found==s.files.end()) || (mode==CREATE_NEW && found!=s.files.end()))return INVALID_HANDLE_VALUE;
 if(mode==CREATE_NEW)s.files[path]={};++s.handles;return new Open{path,0};
}
DWORD GetLastError(){return 2;}
BOOL CloseHandle(HANDLE h){delete static_cast<Open*>(h);--s.handles;return 1;}
BOOL GetFileInformationByHandle(HANDLE h,BY_HANDLE_FILE_INFORMATION* out){out->dwFileAttributes=F(h).attr;out->nNumberOfLinks=F(h).links;return 1;}
BOOL GetFileSizeEx(HANDLE h,LARGE_INTEGER* out){out->QuadPart=F(h).bytes.size();return 1;}
BOOL SetFilePointerEx(HANDLE h,LARGE_INTEGER,void*,DWORD){static_cast<Open*>(h)->pos=0;return 1;}
BOOL ReadFile(HANDLE h,void* buffer,DWORD size,DWORD* read,void*){
 auto& open=*static_cast<Open*>(h);const auto& bytes=F(h).bytes;*read=std::min<std::size_t>(size,bytes.size()-open.pos);std::memcpy(buffer,bytes.data()+open.pos,*read);open.pos+=*read;return 1;
}
BOOL WriteFile(HANDLE h,const void* buffer,DWORD size,DWORD* written,void*){
 if(s.writeFails)return 0;auto& open=*static_cast<Open*>(h);auto& bytes=F(h).bytes;bytes.resize(std::max(bytes.size(),open.pos+size));std::memcpy(bytes.data()+open.pos,buffer,size);open.pos+=size;*written=size;return 1;
}
BOOL FlushFileBuffers(HANDLE){return !s.writeFails;}
BOOL DeleteFileW(const wchar_t* path){return s.files.erase(path)!=0;}
DWORD GetModuleFileNameW(void*,wchar_t* out,DWORD){std::wcscpy(out,L"C:\\kit\\GlassSetup.exe");return std::wcslen(out);}
namespace InstallLog{template<class... T>void LineFormat(const wchar_t*,T...){} }
namespace GlassInstallMsi{
 bool IsProductInstalled(const wchar_t*){return s.installed;}
 std::wstring QueryDisplayVersion(const wchar_t*){return s.installedVersion;}
}
namespace GlassMaintenanceCache{
 bool TrustedDirectory(const std::wstring&){return s.workspaceTrusted;}
 std::wstring CurrentFile(const wchar_t* file){return L"C:\\cache\\Current\\"+std::wstring(file);}
 bool MatchesInstalled(const std::wstring& path,const std::wstring&,const std::wstring&){return s.installed&&s.files.at(path).bytes.find("|INSTALLED|")!=std::string::npos;}
}
namespace GlassReleaseClient{
 HANDLE CreatePrivateFile(const std::wstring& path){return CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);}
 std::wstring Wide(const std::string& text){return {text.begin(),text.end()};}
 bool VerifyHash(HANDLE h,std::uint64_t size,const std::string& hash,const std::atomic<bool>& cancel,std::wstring&){
  return !cancel.load()&&F(h).bytes.size()==size&&hash==std::string(64,'a')&&F(h).bytes.find("|HASH|")!=std::string::npos;
 }
 bool Get(const std::wstring&,HANDLE out,std::string*,std::uint64_t,const std::atomic<bool>&,std::wstring&){
  ++s.downloads;if(s.cancelDownload)cancelled=true;if(s.downloadFails)return false;F(out).bytes=s.download;return true;
 }
}
namespace GlassPackageAcquisition{
using namespace GlassPackageRules;using GlassReleaseClient::Wide;
struct Handle{HANDLE h=INVALID_HANDLE_VALUE;~Handle(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}};
struct Spec{const wchar_t* code;};const Spec specs[]={{L"code"},{L"code"},{L"code"},{L"code"},{L"code"}};
bool Fail(std::wstring& error,const std::wstring& message){error=message;return false;}
bool Metadata(const std::wstring& path,const Spec&,Version& v,std::wstring& code,std::wstring& display){
 const auto& bytes=s.files.at(path).bytes;if(bytes.find("|GOOD|")==std::string::npos)return false;
 const auto text=bytes.substr(0,bytes.find('|'));code=L"code";display={text.begin(),text.end()};return ParseVersion(text,v);
}
std::string Narrow(const std::wstring& text){return {text.begin(),text.end()};}
bool SameMsiPublisher(const std::wstring& path){return s.files.at(path).bytes.find("|SIGNED|")!=std::string::npos;}
'''
main=r'''
}
using GlassPackageCatalog::Product;
std::string Package(const char* version,const char* markers="|SIGNED|"){std::string text=std::string(version)+"|GOOD|"+markers;text.resize(1024,' ');return text;}
void reset(){assert(s.handles==0);s=State{};cancelled=false;}
int main(){
 GlassPackageCatalog::Catalog catalog;std::wstring error;
 auto run=[&]{return GlassPackageAcquisition::Prepare(Product::Platform64,catalog,L"C:\\cache\\Work",cancelled,error);};
 const auto dest=L"C:\\cache\\Work\\Glass-x64.msi",part=L"C:\\cache\\Work\\Glass-x64.msi.part";
 reset();assert(!run());assert(error.find(L"cannot open file (Win32 2)")!=std::wstring::npos);assert(error.find(L"no downloadable asset")!=std::wstring::npos);
 reset();s.files[L"C:\\kit\\packages\\Glass-x64.msi"].bytes=Package("2.0.0");assert(run());assert(s.files.at(dest).bytes==Package("2.0.0")&&s.downloads==0);
 reset();s.files[L"C:\\kit\\Glass-x64.msi"].bytes=Package("2.0.0");assert(run());
 reset();s.files[L"C:\\kit\\packages\\Glass-x64.msi"].bytes=Package("2.0.0");s.files[L"C:\\kit\\Glass-x64.msi"].bytes=Package("3.0.0");assert(run());assert(s.files.at(dest).bytes==Package("3.0.0"));
 reset();s.installed=true;s.files[L"C:\\cache\\Current\\Glass-x64.msi"].bytes=Package("1.0.0","|INSTALLED|");assert(run());
 reset();s.files[L"C:\\cache\\Current\\Glass-x64.msi"].bytes=Package("1.0.0","|INSTALLED|");assert(!run());assert(!s.files.count(dest));
 reset();s.installed=true;s.installedVersion=L"5.0.0";s.files[L"C:\\kit\\Glass-x64.msi"].bytes=Package("1.0.0");assert(!run());
 reset();s.files[L"C:\\kit\\Glass-x64.msi"].bytes=Package("2.0.0","");assert(!run());assert(error.find(L"signature/publisher")!=std::wstring::npos); // Unsigned/unhashed local is not implicitly trusted.
 reset();s.files[L"C:\\kit\\Glass-x64.msi"]={Package("2.0.0"),FILE_ATTRIBUTE_REPARSE_POINT};assert(!run());
 reset();s.files[L"C:\\kit\\Glass-x64.msi"]={Package("2.0.0"),0,2};assert(!run());
 auto& remote=catalog.products[0];remote=GlassPackageCatalog::Asset{};remote->version.parts={3,0,0,0};remote->downloadable=true;remote->size=1024;remote->sha256=std::string(64,'a');remote->url="https://github.com/release";
 reset();s.download=Package("3.0.0","|HASH|");assert(run());assert(s.downloads==1&&s.files.at(dest).bytes==s.download&&!s.files.count(part));
 reset();s.download=Package("3.0.0","");assert(!run());assert(!s.files.count(dest)&&!s.files.count(part));
 reset();s.download=Package("2.0.0","|HASH|");assert(!run());assert(!s.files.count(dest)&&!s.files.count(part)); // digest isn't enough without target metadata.
 reset();s.downloadFails=true;assert(!run());assert(!s.files.count(dest)&&!s.files.count(part));
 reset();s.cancelDownload=true;s.download=Package("3.0.0","|HASH|");assert(!run());assert(!s.files.count(dest)&&!s.files.count(part));
 reset();s.writeFails=true;s.download=Package("3.0.0","|HASH|");assert(!run());assert(!s.files.count(dest)&&!s.files.count(part));
 reset();s.workspaceTrusted=false;assert(!run());assert(s.downloads==0);
 assert(s.handles==0);std::cout<<"PASS: actual package acquisition/snapshot: local/cache/release, trust, downgrade, cancellation and cleanup\n";
}
'''
# Production constants/factories differ from the mock environment, not algorithm.
prefix=prefix.replace('constexpr HANDLE invalid()', 'inline HANDLE invalid()')
with tempfile.TemporaryDirectory(prefix='GlassAcquisition-') as directory:
    path=Path(directory)/'test.cpp';exe=Path(directory)/'test'
    path.write_text(prefix+copy+prepare+main)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-no-pie','-I',str(root),str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
