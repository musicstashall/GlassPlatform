"""Actual file receipt/transaction with fake Win32 filesystem and SHA API.
Not Windows ACL/sharing/runtime proof. No files are installed on the host.
"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassOpenGlassFiles.h').read_text()
s='\n'.join(line for line in s.splitlines() if not line.startswith('#include "') and line!='#include <wincrypt.h>' and line!='#pragma once')
h=r'''
#include <string>
#include <vector>
#include <map>
#include <cstring>
#include <algorithm>
#include <cassert>
#include "GlassSetup/third_party/nlohmann/json.hpp"
using BYTE=unsigned char;using DWORD=unsigned;using HANDLE=int;using HCRYPTPROV=int;using HCRYPTHASH=int;
constexpr int INVALID_HANDLE_VALUE=-1,TRUE=1,FALSE=0;
constexpr DWORD FILE_ATTRIBUTE_DIRECTORY=16,FILE_ATTRIBUTE_REPARSE_POINT=1024,INVALID_FILE_ATTRIBUTES=~0u,
 ERROR_SUCCESS=0,ERROR_FILE_EXISTS=80,ERROR_FILE_NOT_FOUND=2,ERROR_PATH_NOT_FOUND=3,ERROR_ALREADY_EXISTS=183,GENERIC_READ=1,GENERIC_WRITE=2,DELETE=4,
 FILE_READ_ATTRIBUTES=8,FILE_SHARE_READ=1,FILE_SHARE_WRITE=2,OPEN_EXISTING=3,CREATE_NEW=1,
 FILE_FLAG_BACKUP_SEMANTICS=0x2000000,FILE_FLAG_OPEN_REPARSE_POINT=0x200000,FILE_BEGIN=0,
 PROV_RSA_AES=1,CRYPT_VERIFYCONTEXT=1,CALG_SHA_256=1,HP_HASHVAL=1;
struct LARGE_INTEGER{long long QuadPart=0;};struct BY_HANDLE_FILE_INFORMATION{DWORD dwFileAttributes=0,nNumberOfLinks=1;};
struct FILE_DISPOSITION_INFO{bool DeleteFile=false;};constexpr int FileDispositionInfo=4;
struct Node{std::vector<BYTE> data;DWORD attr=0,links=1;bool trusted=true;};
struct Open{std::wstring path;std::size_t at=0;bool erase=false;};
std::map<std::wstring,Node> nodes;std::map<int,Open> handles;int serial=0;DWORD error=0;bool denyWrite=false;
DWORD GetLastError(){return error;}
DWORD GetFileAttributesW(const wchar_t* path){auto it=nodes.find(path);if(it==nodes.end()){error=2;return INVALID_FILE_ATTRIBUTES;}return it->second.attr;}
int CreateFileW(const wchar_t* path,DWORD,DWORD,void*,DWORD creation,DWORD,void*){
 if(creation==CREATE_NEW){if(nodes.count(path)){error=183;return -1;}nodes[path]={};}
 else if(!nodes.count(path)){error=2;return -1;}
 int h=++serial;handles[h]={path,0,false};return h;
}
bool CloseHandle(int h){auto it=handles.find(h);assert(it!=handles.end());if(it->second.erase)nodes.erase(it->second.path);handles.erase(it);return true;}
bool GetFileInformationByHandle(int h,BY_HANDLE_FILE_INFORMATION* info){if(!handles.count(h))return false;auto& n=nodes.at(handles.at(h).path);info->dwFileAttributes=n.attr;info->nNumberOfLinks=n.links;return true;}
bool GetFileSizeEx(int h,LARGE_INTEGER* size){size->QuadPart=nodes.at(handles.at(h).path).data.size();return true;}
bool SetFilePointerEx(int h,LARGE_INTEGER pos,void*,DWORD){handles.at(h).at=static_cast<std::size_t>(pos.QuadPart);return true;}
bool ReadFile(int h,void* data,DWORD size,DWORD* read,void*){auto& f=handles.at(h);auto& b=nodes.at(f.path).data;*read=static_cast<DWORD>(std::min<std::size_t>(size,b.size()-f.at));std::memcpy(data,b.data()+f.at,*read);f.at+=*read;return true;}
bool WriteFile(int h,const void* data,DWORD size,DWORD* written,void*){
 if(denyWrite){denyWrite=false;return false;}
 auto& f=handles.at(h);auto& b=nodes.at(f.path).data;b.resize(std::max(b.size(),f.at+size));std::memcpy(b.data()+f.at,data,size);f.at+=size;*written=size;return true;
}
bool SetEndOfFile(int h){auto& f=handles.at(h);nodes.at(f.path).data.resize(f.at);return true;}
bool FlushFileBuffers(int){return true;}
bool SetFileInformationByHandle(int h,int,FILE_DISPOSITION_INFO* info,unsigned){handles.at(h).erase=info->DeleteFile;return true;}
bool CreateDirectoryW(const wchar_t* path,void*){if(nodes.count(path)){error=183;return false;}nodes[path].attr=16;return true;}
int SHCreateDirectoryExW(void*,const wchar_t* path,void*){nodes[path].attr=16;return 0;}
bool CryptAcquireContextW(int* out,void*,void*,unsigned,unsigned){*out=1;return true;}
bool CryptCreateHash(int,unsigned,int,unsigned,int* out){*out=1;return true;}
unsigned hashValue=0;
bool CryptHashData(int,const BYTE* bytes,DWORD n,unsigned){hashValue=2166136261u;for(unsigned i=0;i<n;++i)hashValue=(hashValue^bytes[i])*16777619u;return true;}
bool CryptGetHashParam(int,unsigned,BYTE* out,DWORD* n,unsigned){for(unsigned i=0;i<32;++i)out[i]=static_cast<BYTE>(hashValue>>((i%4)*8));*n=32;return true;}
void CryptDestroyHash(int){}void CryptReleaseContext(int,unsigned){}
namespace GlassInstallUtil {bool IsSafeInstallPath(const std::wstring& path){return path.rfind(L"C:\\",0)==0&&path.find(L"..") == std::wstring::npos;}}
namespace GlassMaintenanceCache {
 bool TrustedPath(const std::wstring& path,bool directory){auto it=nodes.find(path);return it!=nodes.end()&&it->second.trusted&&bool(it->second.attr&16)==directory&&!(it->second.attr&1024);}
 bool TrustedDirectory(const std::wstring& path){return TrustedPath(path,true);}
}
namespace GlassPackageRules {struct Version{};bool ParseVersion(const std::string& s,Version&){return s=="3.0.2.3749";}}
// PRODUCTION
const std::wstring root=L"C:\\Program Files\\Glass\\OpenGlass";
void reset(){assert(handles.empty());nodes.clear();for(auto p:{L"C:\\",L"C:\\Program Files",L"C:\\Program Files\\Glass",L"C:\\Stage"})nodes[p].attr=16;
 for(auto p:{L"C:\\Stage\\dll",L"C:\\Stage\\host",L"C:\\Stage\\gui"})nodes[p].data={1,2,3};}
const std::array<std::wstring,3> sources={L"C:\\Stage\\dll",L"C:\\Stage\\host",L"C:\\Stage\\gui"};
int main(){using namespace GlassOpenGlassFiles;std::wstring error;
 reset();{Transaction t;assert(t.Begin(root,error));assert(t.Apply(L"3.0.2.3749",false,sources,error));auto r=Inspect(root);assert(r.present&&r.integrity&&!r.milComp);assert(t.Rollback());}assert(!HasReceipt(root));
 reset();{Transaction t;assert(t.Begin(root,error)&&t.Apply(L"3.0.2.3749",true,sources,error));t.Commit();}auto r=Inspect(root);assert(r.present&&r.integrity&&r.milComp);
 auto baseline=nodes[root+L"\\"+Names[0]].data;{Transaction t;assert(t.Begin(root,error));denyWrite=true;assert(!t.Apply(L"3.0.2.3749",false,sources,error));assert(t.Rollback());}assert(nodes[root+L"\\"+Names[0]].data==baseline);
 {Transaction t;assert(t.Begin(root,error)&&t.Remove(error));assert(!HasReceipt(root));assert(t.Rollback());}assert(Inspect(root).integrity);
 {Transaction t;assert(t.Begin(root,error)&&t.Apply(L"3.0.2.3749",false,sources,error));nodes[root+L"\\"+Names[3]].data={99};assert(!t.Rollback());t.Commit();}
 reset();nodes[root].attr=16;nodes[root+L"\\OpenGlass.dll"].data={99};{Transaction t;assert(!t.Begin(root,error));assert(t.Rollback());}assert(nodes[root+L"\\OpenGlass.dll"].data[0]==99);
 reset();nodes[L"C:\\Program Files\\Glass"].attr|=1024;{Transaction t;assert(!t.Begin(root,error));assert(t.Rollback());}
 assert(handles.empty());
}
'''.replace('// PRODUCTION',s)
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'test.cpp';p.write_text(h);exe=Path(tmp)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(root),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: actual OpenGlass file receipt/transaction: install, inspect, update failure rollback, remove rollback, concurrent postimage refusal, foreign files and reparse refusal; fake Win32 APIs')
