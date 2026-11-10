"""Real bounded, pinned user-data walker with fake Win32 file APIs; no disk deletion."""
from pathlib import Path
import subprocess,tempfile,shutil
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassUserDataCleanup.h').read_text().replace('#pragma once','').replace('#include <windows.h>','')
h=r'''
#include <string>
#include <map>
#include <vector>
#include <cassert>
#include <cwchar>
using DWORD=unsigned;using HANDLE=int;
constexpr int INVALID_HANDLE_VALUE=-1,TRUE=1;
constexpr DWORD FILE_READ_ATTRIBUTES=1,DELETE=2,FILE_SHARE_READ=4,FILE_SHARE_WRITE=8,OPEN_EXISTING=1,
 FILE_FLAG_BACKUP_SEMANTICS=16,FILE_FLAG_OPEN_REPARSE_POINT=32,FILE_ATTRIBUTE_REPARSE_POINT=64,
 FILE_ATTRIBUTE_DIRECTORY=128,INVALID_FILE_ATTRIBUTES=~0u,ERROR_FILE_NOT_FOUND=2,ERROR_PATH_NOT_FOUND=3,ERROR_NO_MORE_FILES=18;
struct BY_HANDLE_FILE_INFORMATION {DWORD dwFileAttributes=0,nNumberOfLinks=1;};
struct WIN32_FIND_DATAW {wchar_t cFileName[256]{};};
struct FILE_DISPOSITION_INFO {bool DeleteFile=false;};constexpr int FileDispositionInfo=1;
struct Entry {DWORD attr=FILE_ATTRIBUTE_DIRECTORY,links=1;bool denied=false;};
std::map<std::wstring,Entry> files;std::map<int,std::wstring> handles;
std::map<int,std::pair<std::vector<std::wstring>,unsigned>> finds;
DWORD error=0;int next=1,deletes=0;
DWORD GetLastError(){return error;}void SetLastError(DWORD n){error=n;}
HANDLE CreateFileW(const wchar_t* p,DWORD,DWORD share,void*,DWORD,DWORD flags,void*){
 assert(share==(FILE_SHARE_READ|FILE_SHARE_WRITE));assert(flags==(FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT));
 auto it=files.find(p);if(it==files.end()){error=2;return -1;}if(it->second.denied){error=5;return -1;}
 int h=next++;handles[h]=p;return h;
}
bool CloseHandle(int h){handles.erase(h);return true;}
bool GetFileInformationByHandle(int h,BY_HANDLE_FILE_INFORMATION* out){auto& e=files.at(handles.at(h));out->dwFileAttributes=e.attr;out->nNumberOfLinks=e.links;return true;}
DWORD GetFileAttributesW(const wchar_t* p){auto i=files.find(p);if(i==files.end()){error=2;return INVALID_FILE_ATTRIBUTES;}return i->second.attr;}
int FindFirstFileW(const wchar_t* pattern,WIN32_FIND_DATAW* data){
 std::wstring parent=pattern;parent.resize(parent.size()-1);std::vector<std::wstring> names;
 for(auto& [p,e]:files){(void)e;if(p.rfind(parent,0)==0&&p.find(L'\\',parent.size())==std::wstring::npos)names.push_back(p.substr(parent.size()));}
 if(names.empty()){error=ERROR_FILE_NOT_FOUND;return -1;}
 int h=next++;finds[h]={names,0};std::wcscpy(data->cFileName,names[0].c_str());return h;
}
bool FindNextFileW(int h,WIN32_FIND_DATAW* data){auto& f=finds.at(h);if(++f.second>=f.first.size()){error=ERROR_NO_MORE_FILES;return false;}std::wcscpy(data->cFileName,f.first[f.second].c_str());return true;}
void FindClose(int h){finds.erase(h);}
bool SetFileInformationByHandle(int h,int,FILE_DISPOSITION_INFO* d,unsigned){assert(d->DeleteFile);++deletes;files.erase(handles.at(h));return true;}
// PRODUCTION
void reset(){assert(handles.empty()&&finds.empty());files.clear();deletes=0;
 for(const auto* p:{L"C:\\",L"C:\\Users",L"C:\\Users\\me",L"C:\\Users\\me\\AppData",L"C:\\Users\\me\\AppData\\Local",L"C:\\Users\\me\\AppData\\Local\\GlassPlatform"})files[p]={};
 files[L"C:\\Users\\me\\AppData\\Local\\GlassPlatform\\settings.json"]={0,1,false};
 files[L"C:\\Users\\me\\unrelated.txt"]={0,1,false};}
int main(){std::wstring detail;const std::wstring own=L"C:\\Users\\me\\AppData\\Local\\GlassPlatform";
 reset();assert(GlassUserDataCleanup::Run(L"C:\\Users\\me",false,detail)&&deletes==0);
 assert(GlassUserDataCleanup::Run(L"C:\\Users\\me",true,detail)&&deletes==2&&files.count(L"C:\\Users\\me\\unrelated.txt"));
 reset();files[own].attr|=FILE_ATTRIBUTE_REPARSE_POINT;assert(!GlassUserDataCleanup::Run(L"C:\\Users\\me",true,detail)&&deletes==0);
 reset();files[own+L"\\settings.json"].links=2;assert(!GlassUserDataCleanup::Run(L"C:\\Users\\me",true,detail)&&deletes==0);
 reset();files[L"C:\\Users\\me\\AppData"].attr|=FILE_ATTRIBUTE_REPARSE_POINT;assert(!GlassUserDataCleanup::Run(L"C:\\Users\\me",true,detail)&&deletes==0);
 reset();files[own+L"\\settings.json"].denied=true;assert(!GlassUserDataCleanup::Run(L"C:\\Users\\me",false,detail)&&deletes==0);
 reset();files.erase(own);assert(GlassUserDataCleanup::Run(L"C:\\Users\\me",false,detail));
 assert(!GlassUserDataCleanup::Run(L"C:\\Users\\..\\Windows",true,detail));assert(handles.empty()&&finds.empty());
}
'''.replace('// PRODUCTION',s)
with tempfile.TemporaryDirectory(prefix='GlassUserData-') as temp:
 p=Path(temp)/'test.cpp';p.write_text(h);exe=Path(temp)/'test'
 subprocess.run([shutil.which('g++'),'-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: production profile data walker: pinned ancestors, links/reparse/access refusal, read-only preflight, confined handle deletion and cleanup')
