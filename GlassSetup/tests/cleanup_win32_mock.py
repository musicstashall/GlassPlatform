#!/usr/bin/env python3
"""Exercise the real Win32 directory-cleanup adapter with a fake filesystem.

The fake models sharing, reparse points, directory enumeration and 8.3 aliases;
actual Windows filesystem behavior still requires a VM test.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "GlassSetup/src/InstallSupport/GlassInstallUtil.cpp").read_text(encoding="utf-8")
start = source.index("namespace {\n\n// Normalise before checking roots")
end = source.index("bool RunHiddenCommand(", start)
# The system-root resolver has its own Win32/registry mock; keep this fake
# focused on the directory-cleanup adapter and its filesystem API surface.
resolver_start = source.index("namespace {\n\n// FOLDERID_ProgramFilesX64", start)
resolver_end = source.index("void PruneEmptyDirectories(", resolver_start)
cleanup = source[start:resolver_start] + source[resolver_end:end]
header = (root / "GlassSetup/src/InstallSupport/GlassEmptyDirectories.h").as_posix()
prefix = r'''
#include "HEADER_PATH"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cwchar>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>
using DWORD=std::uint32_t;using BOOL=int;constexpr BOOL TRUE=1,FALSE=0;
#define _wcsicmp wcscasecmp
#define _wcsnicmp wcsncasecmp
constexpr DWORD FILE_SHARE_READ=1,FILE_SHARE_WRITE=2,FILE_SHARE_DELETE=4,
 FILE_LIST_DIRECTORY=8,FILE_READ_ATTRIBUTES=16,DELETE=32,OPEN_EXISTING=3,
 FILE_FLAG_BACKUP_SEMANTICS=64,FILE_FLAG_OPEN_REPARSE_POINT=128,
 FILE_ATTRIBUTE_DIRECTORY=0x10,FILE_ATTRIBUTE_REPARSE_POINT=0x400,INVALID_FILE_ATTRIBUTES=0xffffffff,
 ERROR_FILE_NOT_FOUND=2,ERROR_PATH_NOT_FOUND=3,ERROR_ACCESS_DENIED=5,ERROR_NO_MORE_FILES=18,
 ERROR_SHARING_VIOLATION=32,ERROR_DIR_NOT_EMPTY=145,ERROR_DIRECTORY=267;
constexpr int FileDispositionInfo=1;
struct Node{bool directory=true,reparse=false;DWORD links=1;};
struct Handle{int kind=0;std::wstring path;DWORD access=0,share=0;std::vector<std::wstring> entries;std::size_t index=0;};
using HANDLE=Handle*;
#define INVALID_HANDLE_VALUE reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-1))
struct BY_HANDLE_FILE_INFORMATION{DWORD dwFileAttributes=0,nNumberOfLinks=1;};
struct FILE_DISPOSITION_INFO{BOOL DeleteFile=FALSE;};
struct WIN32_FIND_DATAW{DWORD dwFileAttributes=0;wchar_t cFileName[260]{};};
static DWORD errorCode=0;static std::map<std::wstring,Node> nodes;static std::vector<HANDLE> openHandles;
static std::set<std::wstring> removed,opened;static std::wstring protectedError,raceLink;
DWORD GetLastError(){return errorCode;}void SetLastError(DWORD e){errorCode=e;}
std::wstring normalized(const std::wstring& input){
 std::wstring path=input;std::replace(path.begin(),path.end(),L'/',L'\\');
 if(path.size()>=3 && path[1]==L':'){
  std::wstring root=path.substr(0,3);std::vector<std::wstring> parts;std::size_t pos=3;
  while(pos<path.size()){auto end=path.find(L'\\',pos);auto part=path.substr(pos,end==std::wstring::npos?end:end-pos);
   if(part==L".." && !parts.empty())parts.pop_back();else if(!part.empty() && part!=L"." && part!=L"..")parts.push_back(part);
   if(end==std::wstring::npos)break;pos=end+1;}
  for(auto& p:parts){if(root.back()!=L'\\')root+=L'\\';root+=p;}
  return root;
 }
 return path;
}
std::wstring physical(std::wstring path){
 path=normalized(path);const std::wstring alias=L"C:\\products\\KEE~1",actual=L"C:\\products\\kept";
 if(path.rfind(alias,0)==0 && (path.size()==alias.size() || path[alias.size()]==L'\\'))path=actual+path.substr(alias.size());
 return path;
}
BOOL PathIsRelativeW(const wchar_t* p){const std::wstring s=p;return !(s.size()>=3 && s[1]==L':' && (s[2]==L'\\'||s[2]==L'/')) && s.rfind(L"\\\\",0)!=0;}
BOOL PathIsRootW(const wchar_t* p){const std::wstring s=p;return s.size()==3 && s[1]==L':' && s[2]==L'\\';}
DWORD GetFullPathNameW(const wchar_t* input,DWORD capacity,wchar_t* output,wchar_t**){
 const auto value=normalized(input);if(!output)return static_cast<DWORD>(value.size()+1);
 if(capacity<=value.size())return static_cast<DWORD>(value.size()+1);
 std::wmemcpy(output,value.c_str(),value.size()+1);return static_cast<DWORD>(value.size());
}
DWORD attributes(const std::wstring& path){
 auto it=nodes.find(physical(path));if(it==nodes.end()){errorCode=ERROR_FILE_NOT_FOUND;return INVALID_FILE_ATTRIBUTES;}
 return (it->second.directory?FILE_ATTRIBUTE_DIRECTORY:0)|(it->second.reparse?FILE_ATTRIBUTE_REPARSE_POINT:0);
}
DWORD GetFileAttributesW(const wchar_t* path){if(protectedError==path){errorCode=ERROR_ACCESS_DENIED;return INVALID_FILE_ATTRIBUTES;}return attributes(path);}
HANDLE CreateFileW(const wchar_t* p,DWORD access,DWORD share,void*,DWORD,DWORD,void*){
 const auto path=physical(p);opened.insert(path);auto it=nodes.find(path);
 if(it==nodes.end()){errorCode=ERROR_FILE_NOT_FOUND;return INVALID_HANDLE_VALUE;}
 if(path==raceLink)it->second.reparse=true;
 for(auto* existing:openHandles)if(existing->kind==1 && existing->path==path){
  if(((access&DELETE) && !(existing->share&FILE_SHARE_DELETE)) || ((existing->access&DELETE) && !(share&FILE_SHARE_DELETE))){
   errorCode=ERROR_SHARING_VIOLATION;return INVALID_HANDLE_VALUE;
  }
 }
 auto* h=new Handle{1,path,access,share};openHandles.push_back(h);return h;
}
BOOL GetFileInformationByHandle(HANDLE h,BY_HANDLE_FILE_INFORMATION* info){info->dwFileAttributes=attributes(h->path);
 if(info->dwFileAttributes==INVALID_FILE_ATTRIBUTES)return FALSE;info->nNumberOfLinks=nodes.at(h->path).links;return TRUE;}
BOOL CloseHandle(HANDLE h){auto it=std::find(openHandles.begin(),openHandles.end(),h);if(it!=openHandles.end())openHandles.erase(it);delete h;return TRUE;}
void copyEntry(WIN32_FIND_DATAW* out,const std::wstring& name,const Node& n){
 std::wcsncpy(out->cFileName,name.c_str(),259);out->cFileName[259]=0;
 out->dwFileAttributes=(n.directory?FILE_ATTRIBUTE_DIRECTORY:0)|(n.reparse?FILE_ATTRIBUTE_REPARSE_POINT:0);
}
HANDLE FindFirstFileW(const wchar_t* pattern,WIN32_FIND_DATAW* out){
 std::wstring path=pattern;if(path.size()<2 || path.substr(path.size()-2)!=L"\\*"){errorCode=ERROR_PATH_NOT_FOUND;return INVALID_HANDLE_VALUE;}
 path=physical(path.substr(0,path.size()-2));std::wstring prefix=path+L"\\";std::vector<std::wstring> entries;
 for(const auto& [child,node]:nodes)if(child.rfind(prefix,0)==0){auto name=child.substr(prefix.size());if(name.find(L'\\')==std::wstring::npos)entries.push_back(name);}
 if(entries.empty()){errorCode=ERROR_FILE_NOT_FOUND;return INVALID_HANDLE_VALUE;}
 auto* h=new Handle{2,path,0,0,entries,0};copyEntry(out,entries[0],nodes.at(path+L"\\"+entries[0]));return h;
}
BOOL FindNextFileW(HANDLE h,WIN32_FIND_DATAW* out){
 if(++h->index>=h->entries.size()){errorCode=ERROR_NO_MORE_FILES;return FALSE;}
 const auto& name=h->entries[h->index];copyEntry(out,name,nodes.at(h->path+L"\\"+name));return TRUE;
}
BOOL FindClose(HANDLE h){delete h;return TRUE;}
BOOL SetFileInformationByHandle(HANDLE h,int,void*,DWORD){
 const std::wstring prefix=h->path+L"\\";for(const auto& [path,node]:nodes)if(path.rfind(prefix,0)==0){errorCode=ERROR_DIR_NOT_EMPTY;return FALSE;}
 removed.insert(h->path);nodes.erase(h->path);return TRUE;
}
namespace GlassInstallLog {
 std::vector<std::wstring> messages;
 template<class...A>void LineFormat(const wchar_t* f,A...){messages.emplace_back(f);}
 void Line(const wchar_t* f){messages.emplace_back(f);}
 bool Has(const wchar_t* token){for(const auto& m:messages)if(m.find(token)!=std::wstring::npos)return true;return false;}
}
namespace GlassInstallUtil {
std::wstring TrimTrailingSlashes(std::wstring path){while(!path.empty() && (path.back()==L'\\'||path.back()==L'/'))path.pop_back();return path;}
'''.replace("HEADER_PATH", header)
main = r'''
}
using namespace GlassInstallUtil;
void reset(){assert(openHandles.empty());nodes.clear();removed.clear();opened.clear();protectedError.clear();raceLink.clear();GlassInstallLog::messages.clear();errorCode=0;}
int main(){
 assert(IsWithinInstallRoot(L"C:\\Program Files (x86)\\GlassPlatform",L"c:\\PROGRAM FILES (X86)\\GlassPlatform\\GlassCompat.dll"));
 assert(IsWithinInstallRoot(L"C:\\products",L"C:\\products\\sub\\..\\file.dll"));
 assert(IsWithinInstallRoot(L"C:\\products",L"C:\\products"));
 assert(!IsWithinInstallRoot(L"C:\\products",L"C:\\products-old\\file.dll"));
 assert(!IsWithinInstallRoot(L"C:\\products",L"C:\\products\\..\\developer\\Debug\\GlassCompat.dll"));
 assert(!IsWithinInstallRoot(L"C:\\products",L"D:\\VS_PROJECT\\Build\\x86\\Debug\\GlassCompat.dll"));
 assert(!IsWithinInstallRoot(L"C:\\products",L"01:\\Software\\GlassPlatform"));
 assert(!IsWithinInstallRoot(L"C:\\products",L"relative\\GlassCompat.dll"));
 reset();nodes={{L"C:\\products",{}},{L"C:\\products\\ordinary",{}},{L"C:\\products\\ordinary\\file.dll",{false,false}}};
 assert(IsSafeInstallPath(L"C:\\products\\ordinary\\file.dll"));
 nodes[L"C:\\products\\ordinary\\file.dll"].links=2;
 assert(!IsSafeInstallPath(L"C:\\products\\ordinary\\file.dll"));
 nodes[L"C:\\products\\ordinary\\file.dll"].links=1;
 assert(IsSafeInstallPath(L"C:\\products\\new\\file.dll"));
 assert(!IsSafeInstallPath(L"relative\\file.dll"));
 nodes[L"C:\\products"].reparse=true;
 assert(!IsSafeInstallPath(L"C:\\products\\ordinary\\file.dll"));
 nodes[L"C:\\products"].reparse=false;nodes[L"C:\\products\\ordinary"].reparse=true;
 assert(!IsSafeInstallPath(L"C:\\products\\ordinary\\file.dll"));
 nodes[L"C:\\products\\ordinary"].reparse=false;protectedError=L"C:\\products\\ordinary";
 assert(!IsSafeInstallPath(L"C:\\products\\ordinary\\file.dll"));
 reset();nodes={{L"\\\\server\\share\\folder",{}},{L"\\\\server\\share\\folder\\file.dll",{false,false}}};
 assert(IsSafeInstallPath(L"\\\\server\\share\\folder\\file.dll"));
 nodes[L"\\\\server\\share\\folder"].reparse=true;
 assert(!IsSafeInstallPath(L"\\\\server\\share\\folder\\file.dll"));
 reset();nodes={{L"C:\\products",{}},{L"C:\\products\\OpenGlass",{}},{L"C:\\products\\OpenGlass\\empty",{}},
  {L"C:\\products\\OpenGlass\\file.txt",{false,false}},{L"C:\\products\\OpenGlass\\link",{true,true}}};
 PruneEmptyDirectories(L"C:\\products\\OpenGlass",{});
 assert(removed.count(L"C:\\products\\OpenGlass\\empty"));assert(nodes.count(L"C:\\products\\OpenGlass\\file.txt"));
 assert(nodes.count(L"C:\\products\\OpenGlass\\link") && nodes.count(L"C:\\products\\OpenGlass"));
 assert(GlassInstallLog::Has(L"cleanup starting") && GlassInstallLog::Has(L"Marked empty directory") && GlassInstallLog::Has(L"cleanup retained"));
 reset();nodes={{L"C:\\products",{}},{L"C:\\products\\kept",{}},{L"C:\\products\\kept\\empty",{}},
  {L"C:\\products\\removed",{}},{L"C:\\products\\removed\\empty",{}}};
 PruneEmptyDirectories(L"C:\\products",{L"C:\\products\\KEE~1"});
 assert(nodes.count(L"C:\\products\\kept\\empty") && nodes.count(L"C:\\products\\kept"));
 assert(!nodes.count(L"C:\\products\\removed") && nodes.count(L"C:\\products"));
 reset();nodes={{L"C:\\products",{}},{L"C:\\products\\unsafe",{}},{L"C:\\products\\unsafe\\child",{}}};
 raceLink=L"C:\\products\\unsafe";PruneEmptyDirectories(L"C:\\products",{});
 assert(nodes.count(L"C:\\products\\unsafe\\child") && !opened.count(L"C:\\products\\unsafe\\child"));
 assert(GlassInstallLog::Has(L"skipped non-directory or reparse point"));
 reset();nodes={{L"C:\\products",{}},{L"C:\\products\\link",{true,true}},{L"C:\\products\\link\\child",{}}};
 PruneEmptyDirectories(L"C:\\products\\link\\child",{});assert(nodes.count(L"C:\\products\\link\\child"));
 reset();nodes={{L"C:\\products",{}},{L"C:\\products\\empty",{}}};protectedError=L"C:\\products\\kept";
 PruneEmptyDirectories(L"C:\\products",{protectedError});assert(nodes.count(L"C:\\products\\empty"));
 reset();nodes={{L"C:\\",{}},{L"C:\\foo",{}},{L"\\\\server\\share",{}}};
 for(const auto* bad:{L"C:\\",L"C:\\foo\\..",L"C:relative",L"relative",L"\\\\server\\share",L"\\\\?\\C:\\foo"})
  PruneEmptyDirectories(bad,{});
 assert(removed.empty() && nodes.size()==3);assert(openHandles.empty());
 std::cout<<"PASS: real cleanup adapter: roots, links, race, user files, shared/nested/alias protection\n";
}
'''
with tempfile.TemporaryDirectory(prefix="glass-cleanup-test-") as directory:
    cpp = Path(directory) / "cleanup.cpp"
    exe = Path(directory) / "cleanup-test"
    cpp.write_text(prefix + cleanup + main, encoding="utf-8")
    compiler = os.environ.get("CXX", "g++")
    subprocess.run([compiler, "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                    str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
