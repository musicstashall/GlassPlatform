"""Actual controller store/parser/publication against a fake protected filesystem.
No real filesystem ACL, Authenticode, Windows resource loader or Windows I/O.
"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
cache=(root/'GlassSetup/src/InstallSupport/GlassMaintenanceCache.h').read_text()
launch=(root/'GlassSetup/src/InstallSupport/GlassMaintenanceLaunch.cpp').read_text()
def fn(s,token):
 a=s.index(token);b=s.index('{',a);depth=0
 for i in range(b,len(s)):
  depth+=(s[i]=='{')-(s[i]=='}')
  if depth==0:return s[a:i+1]
 raise ValueError(token)
h=r'''
#include <cassert>
#include <array>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstring>
#include <cwchar>
#include <iostream>
using BYTE=unsigned char;using DWORD=unsigned;using BOOL=int;
constexpr unsigned FILE_ATTRIBUTE_DIRECTORY=16,FILE_ATTRIBUTE_REPARSE_POINT=1024,INVALID_FILE_ATTRIBUTES=~0u;
constexpr int GENERIC_READ=1,GENERIC_WRITE=2,FILE_SHARE_READ=1,OPEN_EXISTING=3,CREATE_NEW=4,FILE_ATTRIBUTE_NORMAL=0,FILE_FLAG_OPEN_REPARSE_POINT=32,TRUE=1,FALSE=0,MOVEFILE_REPLACE_EXISTING=1,MOVEFILE_WRITE_THROUGH=2,LOAD_LIBRARY_AS_DATAFILE=4,LOAD_LIBRARY_AS_IMAGE_RESOURCE=8,IDR_MAINTENANCE_UNINS=310,RT_RCDATA=10;
struct Open {std::wstring path;};using HANDLE=Open*;using HMODULE=Open*;
HANDLE INVALID_HANDLE_VALUE=nullptr;
std::set<std::wstring> dirs;
std::map<std::wstring,std::string> files;
std::map<std::wstring,BYTE> digests;
bool trusted=true,signedOk=true,copyOk=true,extractOk=true,hardenOk=true,flushOk=true,moveOk=true;int handles=0,loads=0;
std::wstring lockedFolder,modulePath;bool lockAvailable=true;int arpRefreshes=0;
HANDLE CreateFileW(const wchar_t* path,int,int,void*,int mode,int,void*){
 if(mode==CREATE_NEW){if(files.count(path))return nullptr;files[path]="";}else if(!files.count(path))return nullptr;
 ++handles;return new Open{path};
}
bool CloseHandle(HANDLE h){--handles;delete h;return true;}
bool ReadFile(HANDLE h,void* out,DWORD size,DWORD* count,void*){auto& f=files[h->path];*count=static_cast<DWORD>(std::min<size_t>(f.size(),size));memcpy(out,f.data(),*count);return true;}
bool WriteFile(HANDLE h,const void* data,DWORD size,DWORD* count,void*){files[h->path].assign(static_cast<const char*>(data),size);*count=size;return true;}
bool FlushFileBuffers(HANDLE){return flushOk;}
DWORD GetFileAttributesW(const wchar_t* path){return dirs.count(path)?FILE_ATTRIBUTE_DIRECTORY:files.count(path)?0:INVALID_FILE_ATTRIBUTES;}
bool DeleteFileW(const wchar_t* p){return files.erase(p)>0;}
bool CopyFileW(const wchar_t* from,const wchar_t* to,int){if(!copyOk||files.count(to))return false;files[to]=files[from];digests[to]=digests[from];return true;}
bool MoveFileExW(const wchar_t* a,const wchar_t* b,int){if(!moveOk)return false;std::wstring from=a,to=b;
 if(dirs.erase(from)){dirs.insert(to);std::vector<std::wstring> names;for(const auto& f:files)if(f.first.rfind(from+L"\\",0)==0)names.push_back(f.first);
 for(const auto& name:names){auto dest=to+name.substr(from.size());files[dest]=files[name];digests[dest]=digests[name];files.erase(name);digests.erase(name);}return true;}
 if(!files.count(from))return false;files[to]=files[from];files.erase(from);return true;
}
namespace GlassInstallUtil {bool IsSafeInstallPath(const std::wstring&){return trusted;}}
namespace GlassRegisteredMsi {bool Hash(const std::wstring& path,std::array<BYTE,32>& out){if(!files.count(path))return false;out.fill(digests[path]);return true;}}
namespace GlassMaintenanceCache {
std::wstring Root(){return L"root";}
bool TrustedDirectory(const std::wstring& p){return trusted&&dirs.count(p);}
bool TrustedPath(const std::wstring& p,bool directory){return trusted&&(directory?dirs.count(p):files.count(p));}
bool EnsureRoot(){return trusted;}
bool SecureDirectory(const std::wstring& p){if(!trusted)return false;dirs.insert(p);return true;}
bool HardenFile(const std::wstring&){return hardenOk;}
bool RemoveFlat(const std::wstring& folder){if(folder==lockedFolder)return false;std::vector<std::wstring> names;for(const auto& f:files)if(f.first.rfind(folder+L"\\",0)==0)names.push_back(f.first);
 for(const auto& n:names)files.erase(n);dirs.erase(folder);return true;}
// CACHE
}
bool ControllerTrusted(const std::wstring& p){return signedOk&&files.count(p);}
std::wstring ControllerToken(){return L"unique";}
HMODULE LoadLibraryExW(const wchar_t* p,void*,int flags){assert(flags==(LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE));++loads;return new Open{p};}
void FreeLibrary(HMODULE h){delete h;--loads;}
namespace GlassInstallUtil {bool ExtractResourceToFile(HMODULE,int,int,const std::wstring& path){if(!extractOk)return false;files[path]="unins";digests[path]=0xbb;return true;}}

struct ControllerLock {struct {HANDLE value=nullptr;} handle;bool acquired=lockAvailable;};
constexpr int WAIT_OBJECT_0=0,WAIT_ABANDONED=128;
int WaitForSingleObject(HANDLE,int){return WAIT_OBJECT_0;}
bool ConfirmControllerStartup(bool ready){return ready;}
std::wstring ModulePath(){return modulePath;}
bool PathsEqualCanonical(const std::wstring& a,const std::wstring& b){return a==b;}
namespace GlassInstallLog {void Line(const wchar_t*){}template<class... T>void LineFormat(const wchar_t*,T...) {}}
namespace GlassInstallUtil {struct InstallRoots {std::wstring x64,x86,openGlass;};bool ResolveInstallRoots(InstallRoots&){return true;}}
namespace GlassInstallMsi {bool RefreshManagedArp(const std::wstring&,const std::wstring&,const std::wstring&){++arpRefreshes;return true;}}
struct WIN32_FIND_DATAW {wchar_t cFileName[256];DWORD dwFileAttributes=0;};
std::vector<std::wstring> enumerated;size_t enumIndex=0;
void fill(WIN32_FIND_DATAW* data){std::wcscpy(data->cFileName,enumerated[enumIndex].c_str());data->dwFileAttributes=FILE_ATTRIBUTE_DIRECTORY;}
HANDLE FindFirstFileW(const wchar_t*,WIN32_FIND_DATAW* data){enumerated.clear();enumIndex=0;const std::wstring prefix=L"root\\Installer\\";
 for(const auto& dir:dirs)if(dir.rfind(prefix,0)==0&&dir.substr(prefix.size()).find(L'\\')==std::wstring::npos)enumerated.push_back(dir.substr(prefix.size()));
 if(enumerated.empty())return nullptr;fill(data);++handles;return new Open{L"find"};}
bool FindNextFileW(HANDLE,WIN32_FIND_DATAW* data){if(++enumIndex>=enumerated.size())return false;fill(data);return true;}
void FindClose(HANDLE h){CloseHandle(h);}
// HANDLE
// WRITE
// STAGE
namespace GlassSetupSourceLayout {std::string catalogText;bool catalogInherited=false;}
// SNAPSHOT
// READCATALOG
// FINALIZE
// RECOVER
void reset(){assert(handles==0&&loads==0);lockedFolder.clear();modulePath.clear();lockAvailable=true;arpRefreshes=0;dirs={L"root"};files.clear();digests.clear();trusted=signedOk=copyOk=extractOk=hardenOk=flushOk=moveOk=true;files[L"download.exe"]="setup";digests[L"download.exe"]=0xaa;}
int main(){using namespace GlassMaintenanceCache;std::string record;
 reset();assert(StageController(L"download.exe",record));assert(record==std::string(64,'a')+std::string(64,'b'));
 assert(WriteControllerRecord(L"Active",record));assert(ReadControllerRecord()==record);
 assert(!ControllerFile(L"GlassSetup.exe").empty()&&!ControllerFile(L"GlassUnins.exe").empty());
 assert(ControllerFileForRecord(record,L"evil.exe").empty());
 assert(!ControllerRecordValid(record+"a")&&!ControllerRecordValid(std::string(128,'/')));
 files[L"root\\Installer\\Active"]+="x";assert(ReadControllerRecord().empty()&&ControllerFile(L"GlassSetup.exe").empty());
 files[L"root\\Installer\\Active"]=record;auto setup=ControllerFile(L"GlassSetup.exe");digests[setup]=1;assert(ControllerFile(L"GlassSetup.exe").empty());
 reset();assert(StageController(L"download.exe",record));assert(WriteControllerRecord(L"Active",record));trusted=false;assert(ControllerFile(L"GlassSetup.exe").empty());
 for(int failure=0;failure<4;++failure){reset();if(failure==0)signedOk=false;if(failure==1)copyOk=false;if(failure==2)extractOk=false;if(failure==3)hardenOk=false;
  assert(!StageController(L"download.exe",record));assert(!files.count(L"root\\Installer\\Active")&&handles==0&&loads==0);}
 reset();assert(StageController(L"download.exe",record));flushOk=false;assert(!WriteControllerRecord(L"Active",record));assert(ReadControllerRecord().empty());
 flushOk=true;assert(WriteControllerRecord(L"Active",record));moveOk=false;assert(!WriteControllerRecord(L"Active",std::string(128,'c')));assert(ReadControllerRecord()==record);

 reset();std::string oldRecord;
 assert(StageController(L"download.exe",record));assert(WriteControllerRecord(L"Active",record));
 files[L"old.exe"]="old";digests[L"old.exe"]=0xcc;assert(StageController(L"old.exe",oldRecord));assert(WriteControllerRecord(L"Previous",oldRecord));
 modulePath=ControllerFile(L"GlassSetup.exe");
 FinalizeControllerStartup(false);assert(ReadControllerRecord()==oldRecord); // Restore last known-good pointer on startup failure.
 assert(WriteControllerRecord(L"Active",record));
 const auto oldFolder=L"root\\Installer\\"+std::wstring(64,L'c');lockedFolder=oldFolder;
 dirs.insert(L"root\\Current");files[L"root\\Current\\old.msi"]="source";
 FinalizeControllerStartup(true);assert(ReadControllerRecord()==record&&dirs.count(oldFolder)&&arpRefreshes==1);
 lockedFolder.clear();FinalizeControllerStartup(true);assert(dirs.count(oldFolder)&&files.count(L"root\\Current\\old.msi"));
 assert(!ControllerFileForRecord(oldRecord,L"GlassSetup.exe").empty()); // Cached ModifyPath still launches after old process exits.
 assert(!ControllerFile(L"GlassSetup.exe").empty());

 // Replay crashes on each side of the durable confirmation boundary.
 assert(WriteControllerRecord(L"Confirmed",oldRecord));assert(WriteControllerRecord(L"Pending",record));
 // Simulate external loss/corruption: an unavailable rollback target fails closed.
 assert(RemoveFlat(oldFolder));
 assert(!RecoverControllerTransition());
 files[L"old.exe"]="old";digests[L"old.exe"]=0xcc;assert(StageController(L"old.exe",oldRecord));
 assert(RecoverControllerTransition()&&ReadControllerRecord()==oldRecord&&ReadControllerRecord(L"Rejected")==record);
 assert(WriteControllerRecord(L"Active",record)&&WriteControllerRecord(L"Confirmed",record)&&WriteControllerRecord(L"Pending",record));
 assert(RecoverControllerTransition()&&ReadControllerRecord()==record); // Crash after Confirmed, before journal deletion.
 assert(WriteControllerRecord(L"Pending",oldRecord));assert(RecoverControllerTransition()&&ReadControllerRecord()==record); // Staged, not committed.
 files.erase(L"root\\Installer\\Confirmed");assert(WriteControllerRecord(L"Pending",record));
 assert(RecoverControllerTransition()&&ReadControllerRecord()==record); // First install: forward recovery without a fake rollback target.

 GlassSetupSourceLayout::catalogText="catalog";
 {ControllerCatalogSnapshot snapshot;assert(snapshot.Write());auto path=snapshot.path;
  GlassSetupSourceLayout::catalogText.clear();assert(ReadControllerCatalog(path)&&GlassSetupSourceLayout::catalogInherited&&GlassSetupSourceLayout::catalogText=="catalog");
  assert(!ReadControllerCatalog(L"untrusted\\Catalog-test")&&!ReadControllerCatalog(L"root\\Installer\\Catalog-..\\Active"));
  trusted=false;assert(!ReadControllerCatalog(path));trusted=true;
  files[path]=std::string(1024*1024+1,'x');assert(!ReadControllerCatalog(path));
 }
 assert(!files.count(L"root\\Installer\\Catalog-unique"));
 GlassSetupSourceLayout::catalogText.clear();
 {ControllerCatalogSnapshot snapshot;assert(snapshot.Write()&&ReadControllerCatalog(snapshot.path)&&GlassSetupSourceLayout::catalogText.empty());}
 assert(handles==0&&loads==0);
 std::cout<<"PASS: actual controller store: immutable Setup/Unins digests, resource-data-only staging, strict pointer parsing, ACL/hash refusal, atomic pointer writes, copy/extract/flush/move failure preservation and cleanup\n";
}
'''
methods='\n'.join(fn(cache,t) for t in ['inline bool ControllerRecordValid(', 'inline std::string ReadControllerRecord(', 'inline std::string ControllerDigest(', 'inline std::wstring ControllerFileForRecord(', 'inline std::wstring ControllerFile('])
h=h.replace('// CACHE',methods).replace('// HANDLE',fn(launch,'struct ControllerHandle ')+ ';').replace('// WRITE',fn(launch,'bool WriteControllerRecord(')).replace('// STAGE',fn(launch,'bool StageController(')).replace('// SNAPSHOT',fn(launch,'struct ControllerCatalogSnapshot ')+ ';').replace('// READCATALOG',fn(launch,'bool ReadControllerCatalog(')).replace('// FINALIZE',fn(launch,'bool FinalizeControllerStartup(')).replace('// RECOVER',fn(launch,'bool RecoverControllerTransition('))
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
