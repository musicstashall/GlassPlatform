#!/usr/bin/env python3
"""Run production distribution-record functions with fake FS/Hash APIs, not Windows crypto."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassMaintenanceCache.h').read_text()
def function(token):
 a=s.index(token);b=s.index('{',a);depth=0
 for i in range(b,len(s)):
  if s[i]=='{':depth+=1
  if s[i]=='}':
   depth-=1
   if not depth:return s[a:i+1]
 raise AssertionError(token)
prefix=r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <map>
#include <vector>
#include <cwchar>
#include <iostream>
using BYTE=unsigned char;using DWORD=std::uint32_t;
constexpr DWORD GENERIC_READ=1,GENERIC_WRITE=2,FILE_SHARE_READ=1,OPEN_EXISTING=3,CREATE_NEW=1,FILE_FLAG_OPEN_REPARSE_POINT=1,FILE_ATTRIBUTE_NORMAL=0,FILE_ATTRIBUTE_DIRECTORY=16,FILE_ATTRIBUTE_REPARSE_POINT=1024,INVALID_FILE_ATTRIBUTES=~0u,ERROR_FILE_NOT_FOUND=2,ERROR_PATH_NOT_FOUND=3,ERROR_INVALID_DATA=13,ERROR_SUCCESS=0,ERROR_WRITE_FAULT=29;
struct Handle{std::wstring path;};using HANDLE=Handle*;const HANDLE INVALID_HANDLE_VALUE=nullptr;
struct BY_HANDLE_FILE_INFORMATION{DWORD dwFileAttributes=0,nNumberOfLinks=1;};
struct LARGE_INTEGER{long long QuadPart;};
std::map<std::wstring,std::string> files;
bool trusted=true,readable=true,hashable=true,flushed=true;DWORD attrs=0,links=1;int registration=1;
DWORD probeError=ERROR_FILE_NOT_FOUND;
DWORD GetLastError(){return probeError;}
DWORD GetFileAttributesW(const wchar_t* path){return files.count(path)?attrs:INVALID_FILE_ATTRIBUTES;}
int trustChecks=0;
namespace Diag{bool Fail(const wchar_t*,const std::wstring&,DWORD){return false;}}
bool TrustedPath(const std::wstring& p,bool){++trustChecks;return trusted&&files.count(p);}
std::wstring CurrentFile(const wchar_t* name){auto p=L"Current\\"+std::wstring(name);return trusted&&files.count(p)?p:L"";}
HANDLE CreateFileW(const wchar_t* path,DWORD,DWORD,void*,DWORD mode,DWORD,void*){
 if(mode==CREATE_NEW){if(files.count(path))return nullptr;files[path]="";}
 else if(!files.count(path))return nullptr;
 return new Handle{path};
}
bool CloseHandle(HANDLE h){delete h;return true;}
bool GetFileInformationByHandle(HANDLE,BY_HANDLE_FILE_INFORMATION* i){i->dwFileAttributes=attrs;i->nNumberOfLinks=links;return true;}
bool GetFileSizeEx(HANDLE h,LARGE_INTEGER* size){size->QuadPart=files[h->path].size();return true;}
bool ReadFile(HANDLE h,void* data,DWORD n,DWORD* read,void*){if(!readable)return false;*read=n;files[h->path].copy(static_cast<char*>(data),n);return true;}
bool WriteFile(HANDLE h,const void* data,DWORD n,DWORD* written,void*){*written=n;files[h->path]=std::string(static_cast<const char*>(data),n);return true;}
bool FlushFileBuffers(HANDLE){return flushed;}
bool DeleteFileW(const wchar_t* p){return files.erase(p)>0;}
namespace GlassRegisteredMsi{bool Hash(const std::wstring& path,std::array<BYTE,32>& digest){if(!hashable||!files.count(path))return false;digest={};unsigned i=0;for(auto c:files[path])digest[i++%32]^=static_cast<BYTE>(c);return true;}}
constexpr int MSIINSTALLCONTEXT_MACHINE=4;
struct Registration{std::wstring sid,version,local;int context;};
bool unambiguous=true,cacheVersionMatch=true,cachePackageMatch=true,machineContext=true;
bool Identity(const std::wstring&,std::wstring& code,std::wstring& version){code=L"code";version=L"1";return true;}
std::wstring PackageCode(const std::wstring& path){return path.find(L"WindowsInstaller")!=std::wstring::npos&&!cachePackageMatch?L"other package":L"package";}
bool Registrations(const std::wstring&,std::vector<Registration>& out){out={{L"",cacheVersionMatch?L"1":L"2",L"WindowsInstaller\\cached.msi",machineContext?MSIINSTALLCONTEXT_MACHINE:1}};if(!unambiguous)out.push_back(out[0]);return true;}
#define _wcsicmp wcscasecmp
int RegistrationState(const std::wstring&,const std::wstring&,const std::wstring&){return registration;}
'''
main=r'''
int main(){
 const std::wstring msi=L"Current\\product.msi",record=msi+L".sha256",copy=L"Work\\product.msi";
 files[L"WindowsInstaller\\cached.msi"]="stripped MSI cache";files[msi]="original distribution with cabinet";files[copy]=files[msi];
 assert(!MatchesInstalled(msi,L"code",L"1")&&trustChecks==0); // Legacy ACL+identity is insufficient.
 probeError=ERROR_PATH_NOT_FOUND;assert(!MatchesInstalled(msi,L"code",L"1")&&trustChecks==0);
 probeError=5;assert(!MatchesInstalled(msi,L"code",L"1")&&trustChecks==1); // Non-absence errors still reach diagnostic trust validation.
 probeError=ERROR_FILE_NOT_FOUND;
 assert(WritePublishedDigest(msi));assert(files[record].size()==128);
 assert(MatchesInstalled(msi,L"code",L"1")&&!MatchesInstalled(copy,L"code",L"1"));
 files[copy+L".sha256"]=files[record];assert(MatchesInstalled(copy,L"code",L"1"));
 const auto cached=files[L"WindowsInstaller\\cached.msi"];
 files[L"WindowsInstaller\\cached.msi"]="cache changed with identical package identity";assert(!MatchesInstalled(copy,L"code",L"1"));
 files[L"WindowsInstaller\\cached.msi"]=cached;unambiguous=false;assert(!MatchesInstalled(copy,L"code",L"1"));unambiguous=true;
 cacheVersionMatch=false;assert(!MatchesInstalled(copy,L"code",L"1"));cacheVersionMatch=true;
 cachePackageMatch=false;assert(!MatchesInstalled(copy,L"code",L"1"));cachePackageMatch=true;
 machineContext=false;assert(!MatchesInstalled(copy,L"code",L"1"));machineContext=true;
 files[copy]="changed distribution";assert(!MatchesInstalled(copy,L"code",L"1"));
 files[copy]=files[msi];
 registration=0;assert(!MatchesInstalled(copy,L"code",L"1"));registration=-1;assert(!MatchesInstalled(copy,L"code",L"1"));registration=1;
 const auto valid=files[record];
 for(unsigned scenario=0;scenario<9;++scenario){
  trusted=readable=hashable=true;attrs=0;links=1;files[record]=valid;
  if(scenario==0)trusted=false;if(scenario==1)readable=false;if(scenario==2)hashable=false;
  if(scenario==3)attrs=FILE_ATTRIBUTE_REPARSE_POINT;if(scenario==4)attrs=FILE_ATTRIBUTE_DIRECTORY;
  if(scenario==5)links=2;if(scenario==6)files[record]+='\n';
  if(scenario==7)files[record][0]='z';if(scenario==8)files.erase(record);
  assert(!MatchesInstalled(msi,L"code",L"1"));
 }
 trusted=readable=hashable=true;attrs=0;links=1;
 flushed=false;assert(!WritePublishedDigest(msi));assert(!files.count(record));
 flushed=true;assert(WritePublishedDigest(msi));assert(!WritePublishedDigest(msi)); // Never replace a concurrent record.
 std::cout<<"PASS: production distribution hash records: legacy refusal, distribution/cached-MSI digests kept separate, copied MSI binding, identity/context mismatch, changed distribution or cached bytes, bad/truncated/linked/reparse/untrusted records, I/O errors and record cleanup (fake FS/Hash)\n";
}
'''
with tempfile.TemporaryDirectory(prefix='glass-cache-digest-') as d:
 cpp=Path(d)/'test.cpp';exe=Path(d)/'test';cpp.write_text(prefix+'\n'.join(function(x) for x in ['inline bool InstalledCacheDigest(', 'inline bool PublishedDigestMatches(', 'inline bool WritePublishedDigest(', 'inline bool MatchesInstalled('])+main)
 subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',str(cpp),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
assert 'blobs[i].verifiedSource || PublishedDigestMatches(source)' in s
assert 'PublishedDigestMatches(staged' not in s
acq=(root/'GlassSetup/src/InstallSupport/GlassPackageAcquisition.cpp').read_text()
assert 'MatchesInstalled(sources[index]' in acq and 'ControllerDigest(staged)==GlassMaintenanceCache::ControllerDigest(sources[index])' in acq and 'SameMsiPublisher(staged)' in acq

repair=function('inline bool RegisterRepairSource(')
assert repair.index('PublishedDigestMatches(file)') < repair.index('MsiSourceListAddSourceExW(')
