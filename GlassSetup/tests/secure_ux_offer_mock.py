"""Actual vendor Describe/FindLocal/Prepare with fake OS, hashing and MSI APIs.
No filesystem, network, Windows Installer, real hashing or identity API is executed.
Release JSON parsing is real; low-level hash/inventory have separate tests.
"""
from pathlib import Path
import shutil, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassSecureUxPackage.cpp').read_text()
def fn(token):
 a=s.index(token);b=s.index('{',a);depth=1;c=b+1
 while depth:
  depth+=(s[c]=='{')-(s[c]=='}');c+=1
 return s[a:c]
code='\n'.join(fn(t) for t in ['std::vector<std::wstring> CandidatePaths(', 'bool FetchAsset(', 'bool FindLocal(', 'bool Describe(', 'bool Prepare(const std::wstring& privateDirectory'])
assert 'SecureUxTheme.msi' not in fn('std::vector<std::wstring> CandidatePaths(')
assert 'CreatePrivateFile' not in fn('bool Describe(') and 'CopySnapshot' not in fn('bool Describe(')
main=(root/'GlassSetup/src/main.cpp').read_text()
assert 'const auto& sutTarget=sutLocal?sutOffer.localVersion:sutOffer.releaseVersion' in main
assert 'JsStringLiteral(sutOffer.releaseTag)' in main
assert 'GlassSecureUxPackage::Describe(cancel,report.secureUxOffer' in (root/'GlassSetup/src/InstallSupport/GlassProductInspect.cpp').read_text()
h=r'''
#include "GlassSetup/src/InstallSupport/GlassSecureUxRelease.h"
#include <atomic>
#include <cassert>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
using namespace GlassSecureUxRelease;
using HANDLE=int;using DWORD=unsigned;
constexpr HANDLE INVALID_HANDLE_VALUE=-1;
constexpr unsigned GENERIC_READ=1,FILE_SHARE_READ=2,OPEN_EXISTING=3,FILE_FLAG_OPEN_REPARSE_POINT=4,
 ERROR_ACCESS_DENIED=5,ERROR_INVALID_DATA=13,ERROR_NOT_ENOUGH_MEMORY=8;
struct LARGE_INTEGER {long long QuadPart=0;};
struct Package {std::wstring path,productCode,version;bool alreadyInstalled=false;};
struct Offer {std::wstring localVersion,localPath,releaseVersion,releaseTag;std::uint64_t localSize=0,releaseSize=0;bool localVerified=false;};
struct Entry {Asset asset;std::wstring version=L"4.0.0";bool safe=true,identity=true;};
std::map<std::wstring,Entry> files;std::map<int,std::wstring> handles;
int nextHandle=1,fetches=0,downloads=0,copies=0,creates=0;bool online=true,moduleOk=true,existing=false,trusted=true;
std::string json;Architecture machine=Architecture::X64;
std::wstring Wide(const std::string& s){return {s.begin(),s.end()};}
void CloseHandle(HANDLE h){assert(handles.erase(h)==1);}
struct File {HANDLE h=-1;~File(){if(h!=-1)CloseHandle(h);}};
unsigned GetModuleFileNameW(void*,wchar_t* out,unsigned){if(!moduleOk)return 0;std::wcscpy(out,L"C:\\kit\\GlassSetup.exe");return std::wcslen(out);}
HANDLE CreateFileW(const wchar_t* path,unsigned access,unsigned share,void*,unsigned mode,unsigned flags,void*){
 assert(access==GENERIC_READ&&share==FILE_SHARE_READ&&mode==OPEN_EXISTING&&flags==FILE_FLAG_OPEN_REPARSE_POINT);
 if(!files.count(path))return -1;int h=nextHandle++;handles[h]=path;return h;
}
bool GetFileSizeEx(HANDLE h,LARGE_INTEGER* out){out->QuadPart=files.at(handles.at(h)).asset.size;return true;}
bool FlushFileBuffers(HANDLE){return true;}
void DeleteFileW(const wchar_t* p){files.erase(p);}
namespace InstallLog {template<class... A>void LineFormat(const wchar_t*,A...){}void Line(const wchar_t*){}}
namespace GlassMaintenanceCache {
std::wstring CurrentFile(const wchar_t* name){return std::wstring(L"cache\\")+name;}
bool TrustedDirectory(const std::wstring&){return trusted;}
}
namespace GlassReleaseClient {
HANDLE CreatePrivateFile(const std::wstring& path){++creates;int h=nextHandle++;handles[h]=path;return h;}
}
bool Fail(std::wstring& error,const wchar_t* text,unsigned=0){error=text;return false;}
bool Cancelled(const std::atomic_bool& cancel,std::wstring& error){if(!cancel.load())return false;error=L"cancelled";return true;}
Architecture NativeArchitecture(){return machine;}
bool Hash(File& f,const Asset& asset,const std::atomic_bool&,std::wstring& error){const auto& e=files.at(handles.at(f.h));
 if(e.safe&&e.asset.sha256==asset.sha256&&e.asset.size==asset.size)return true;error=L"hash/type/links";return false;}
bool Metadata(const std::wstring& path,Architecture arch,Package& p,std::wstring& error){const auto& e=files.at(path);
 if(!e.identity||e.asset.name!=FileName(arch)){error=L"MSI identity/architecture";return false;}p.version=e.version;p.productCode=L"official";return true;}
bool Get(const std::wstring&,HANDLE h,std::string* text,std::uint64_t,const std::atomic_bool&,std::wstring& error){
 if(!online){error=L"offline";return false;}if(h==-1){++fetches;*text=json;return true;}
 ++downloads;Asset remote;std::string why;assert(Select(json,machine,remote,why));files[handles.at(h)]={remote,L"4.1.0"};return true;
}
bool CopySnapshot(File& file,const std::wstring& path,const std::atomic_bool&,std::wstring&){++copies;files[path]=files.at(handles.at(file.h));return true;}
bool CheckLegacyRepackAbsent(std::wstring&){return true;}
bool QueryInstallPresence(std::vector<std::wstring>& out,const std::atomic_bool&,std::wstring&){if(existing)out.push_back(L"installed");return true;}
// ACTUAL
void reset(const std::string& data){assert(handles.empty());files.clear();fetches=downloads=copies=creates=0;online=moduleOk=trusted=true;existing=false;json=data;machine=Architecture::X64;}
int main(int argc,char** argv){assert(argc==2);std::ifstream input(argv[1]);const std::string fixture((std::istreambuf_iterator<char>(input)),{});
 const auto known=OfflineV4(Architecture::X64);const std::wstring local=L"C:\\kit\\SecureUxTheme_x64.msi";
 std::atomic_bool cancel{false};Offer offer;std::wstring error;Package package;
 {reset(fixture);files[local]={known};assert(Describe(cancel,offer,error));assert(offer.localVerified&&offer.localVersion==L"4.0.0"&&offer.localPath==local);
  assert(offer.releaseVersion==L"4.0.0"&&offer.releaseTag==L"v4.0.0");assert(copies==0&&creates==0&&downloads==0&&handles.empty());
  assert(Prepare(L"private",cancel,package,error));assert(package.version==offer.localVersion&&package.path==L"private\\SecureUxTheme_x64.msi");assert(copies==1&&downloads==0);}
 {reset(fixture);online=false;files[local]={known};assert(Describe(cancel,offer,error));assert(offer.localVersion==L"4.0.0"&&offer.releaseVersion.empty());}
 {reset(fixture);assert(Describe(cancel,offer,error));assert(!offer.localVerified&&offer.localVersion.empty()&&offer.releaseVersion==L"4.0.0");assert(copies==0&&creates==0&&downloads==0);}
 {reset(fixture);online=false;assert(!Describe(cancel,offer,error));assert(offer.localVersion.empty()&&offer.releaseVersion.empty());}
 for(int n=0;n<3;++n){reset(fixture);files[local]={known};
  if(n==0)files[local].safe=false;if(n==1)files[local].identity=false;if(n==2)files[local].asset.sha256=std::string(64,'0');
  assert(Describe(cancel,offer,error));assert(!offer.localVerified&&offer.localVersion.empty());}
 {reset(fixture);files[L"C:\\kit\\SecureUxTheme.msi"]={known};assert(Describe(cancel,offer,error));assert(!offer.localVerified);}
 {reset(fixture);files[local]={known};files[L"C:\\kit\\packages\\SecureUxTheme_x64.msi"]={known,L"4.0.1"};
  assert(Describe(cancel,offer,error));assert(offer.localVersion==L"4.0.1"&&offer.localPath.find(L"packages")!=std::wstring::npos);}
 auto newer=nlohmann::json::parse(fixture);newer["tag_name"]="v4.1.0";
 for(auto& a:newer["assets"]){a["browser_download_url"]="https://github.com/namazso/SecureUxTheme/releases/download/v4.1.0/"+a["name"].get<std::string>();a["digest"]="sha256:"+std::string(64,'a');}
 Asset remote;std::string why;assert(Select(newer.dump(),Architecture::X64,remote,why));
 {reset(newer.dump());files[local]={remote,L"4.0.9"};assert(Describe(cancel,offer,error));
  assert(offer.localVersion==L"4.0.9"&&offer.releaseVersion==L"4.1.0");assert(Prepare(L"private",cancel,package,error));assert(package.version==L"4.0.9"&&downloads==0);}
 {reset(newer.dump());files[local]={known};assert(Describe(cancel,offer,error));assert(offer.localVersion==L"4.0.0"&&offer.releaseVersion==L"4.1.0");
  assert(Prepare(L"private",cancel,package,error)&&package.version==L"4.0.0");}
 {reset(newer.dump());assert(Prepare(L"private",cancel,package,error));assert(downloads==1&&copies==1&&package.version==L"4.1.0");}
 {reset(fixture);cancel=true;assert(!Describe(cancel,offer,error)&&offer.localVersion.empty()&&fetches==0);cancel=false;}
 {reset(fixture);moduleOk=false;assert(!Describe(cancel,offer,error)&&offer.localVersion.empty());}
 {reset(fixture);existing=true;assert(Prepare(L"private",cancel,package,error)&&package.alreadyInstalled&&copies==0&&fetches==0);}
 assert(handles.empty());
}
'''.replace('// ACTUAL',code)
with tempfile.TemporaryDirectory(prefix='GlassSutOffer-') as directory:
 p=Path(directory)/'test.cpp';exe=Path(directory)/'test';p.write_text(h)
 subprocess.run([shutil.which('clang++') or shutil.which('g++'),'-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-pedantic','-I'+str(root),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe),str(root/'GlassSetup/tests/fixtures/secure_ux_release_v4.json')],check=True)
print('PASS: actual SUT discovery/acquisition: canonical local-first MSI, actual ProductVersion vs release tag, offline/online, rejected files, cancellation, no discovery writes/downloads, immutable prepare snapshot')
