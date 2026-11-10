"""Actual source retain/GC functions: fake FS and registration/boot pin seams."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassMaintenanceCache.h').read_text()
a=s.index('inline bool RetainReferencedSources(');b=s.index('struct Blob {',a)
h=r'''
#include <cassert>
#include <string>
#include <map>
#include <vector>
#include <cwchar>
#include <iostream>
using DWORD=unsigned;using HANDLE=int;
constexpr int INVALID_HANDLE_VALUE=-1,TRUE=1,FALSE=0,ERROR_FILE_NOT_FOUND=2,ERROR_FILE_EXISTS=80,ERROR_NO_MORE_FILES=18;
constexpr unsigned FILE_ATTRIBUTE_DIRECTORY=16,FILE_ATTRIBUTE_REPARSE_POINT=1024,INVALID_FILE_ATTRIBUTES=~0u;
struct WIN32_FIND_DATAW {wchar_t cFileName[256];DWORD dwFileAttributes=0;};
std::map<std::wstring,std::string> files;std::map<std::wstring,int> refs;std::map<std::wstring,bool> bound;
std::vector<std::wstring> names;size_t index_;int error_=0;bool trust=true,pinned=false,copyOk=true,enumerationOk=true;
namespace GlassMaintenanceReboot {bool Pinned(){return pinned;}}
std::wstring Root(){return L"root";}bool TrustedDirectory(const std::wstring&){return trust;}
bool TrustedPath(const std::wstring&,bool){return trust;}bool HardenFile(const std::wstring&){return trust;}
int GetLastError(){return error_;}
void fill(WIN32_FIND_DATAW* data){std::wcscpy(data->cFileName,names[index_].c_str());}
HANDLE FindFirstFileW(const wchar_t* mask,WIN32_FIND_DATAW* data){names.clear();index_=0;if(!enumerationOk){error_=5;return -1;}
 std::wstring path=mask;auto folder=path.substr(0,path.find_last_of(L'\\')+1);
 for(const auto& f:files)if(f.first.rfind(folder,0)==0&&f.first.size()>4&&f.first.substr(f.first.size()-4)==L".msi")names.push_back(f.first.substr(folder.size()));
 if(names.empty()){error_=2;return -1;}fill(data);return 1;}
bool FindNextFileW(HANDLE,WIN32_FIND_DATAW* data){if(++index_>=names.size()){error_=18;return false;}fill(data);return true;}void FindClose(HANDLE){}
bool Identity(const std::wstring& file,std::wstring& code,std::wstring& version){code=file;version=L"1";return true;}
int RegistrationState(const std::wstring& file,const std::wstring&,const std::wstring&){return refs[file];}
std::string ControllerDigest(const std::wstring& file){return files.count(file)?std::string(64,files[file][0]):std::string{};}
bool PublishedDigestMatches(const std::wstring& file){return bound[file]&&files[file+L".sha256"]==files[file];}
bool CopyFileW(const wchar_t* from,const wchar_t* to,int){if(!copyOk){error_=5;return false;}if(files.count(to)){error_=80;return false;}
 files[to]=files[from];refs[to]=refs[from];bound[to]=bound[from];return true;}
bool DeleteFileW(const wchar_t* file){return files.erase(file)>0;}
DWORD GetFileAttributesW(const wchar_t* file){return files.count(file)?0:INVALID_FILE_ATTRIBUTES;}
struct CacheLock {bool acquired=true;};
// SOURCE
void reset(){files.clear();refs.clear();bound.clear();trust=copyOk=enumerationOk=true;pinned=false;}
int main(){
 const std::wstring current=L"root\\Current",retained=L"root\\Retained",file=current+L"\\omitted-product.msi",dest=retained+L"\\"+std::wstring(64,'a')+L".msi";
 reset();files[file]="a";files[file+L".sha256"]="a";refs[file]=2;bound[file]=true;
 assert(RetainReferencedSources(current,retained)&&files.count(dest)&&PublishedDigestMatches(dest));
 assert(RetainReferencedSources(current,retained)); // Idempotent exact-byte collision.
 files[dest]="b";assert(!RetainReferencedSources(current,retained));
 reset();files[file]="a";refs[file]=1;assert(RetainReferencedSources(current,retained)&&files.count(dest)&&!files.count(dest+L".sha256"));
 refs[dest]=-1;assert(!GarbageCollect()&&files.count(dest)); // Unknown SID/context pins.
 refs[dest]=1;assert(GarbageCollect()&&files.count(dest));
 refs[dest]=0;pinned=true;assert(GarbageCollect()&&files.count(dest));
 pinned=false;assert(GarbageCollect()&&!files.count(dest));
 reset();files[file]="a";refs[file]=0;pinned=true;assert(RetainReferencedSources(current,retained)&&files.count(dest));
 pinned=false;files[dest+L".sha256"]="a";assert(GarbageCollect()&&!files.count(dest+L".sha256"));
 reset();files[file]="a";refs[file]=-1;assert(!RetainReferencedSources(current,retained)&&files.count(file));
 refs[file]=1;copyOk=false;assert(!RetainReferencedSources(current,retained));
 copyOk=true;enumerationOk=false;assert(!RetainReferencedSources(current,retained)&&!GarbageCollect());
 std::cout<<"PASS: actual source reference GC: all Current MSIs including omitted products, content addressing, exact collision check, bound/unbound receipts, unknown registrations and reboot pins, safe release and I/O refusal\n";
}
'''.replace('// SOURCE',s[a:b])
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
