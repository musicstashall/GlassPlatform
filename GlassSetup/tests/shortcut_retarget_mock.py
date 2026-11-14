"""Actual locked-link stream/commit/restore logic against file API seams.
No real Windows locking, COM serialization, ACLs or MSI integration is tested.
"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassInstallRemoval.cpp').read_text()
start=s.index('bool RewriteShortcutStream(')
code=s[start:s.index('\n} // namespace',start)]
same=s[s.index('bool SameOpenFiles('):s.index('bool RemoveShortcut(')]
start=s.index('    if(session.shortcutRetargetAttempted)',s.index('bool Restore(Session&'))
restore=s[start:s.index('    if (session.shortcutRemoved)',start)]
h=r'''
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <iostream>
using HANDLE=int;using BYTE=unsigned char;using DWORD=unsigned;
constexpr HANDLE INVALID_HANDLE_VALUE=-1;
constexpr DWORD GENERIC_READ=1,FILE_SHARE_READ=1,OPEN_EXISTING=1,FILE_FLAG_OPEN_REPARSE_POINT=1,
 FILE_ATTRIBUTE_DIRECTORY=16,FILE_ATTRIBUTE_REPARSE_POINT=32,FILE_BEGIN=0,INVALID_FILE_ATTRIBUTES=~0u;
constexpr int SE_SECURITY_NAME=1,FileBasicInfo=0;
struct LARGE_INTEGER{long long QuadPart=0;};struct FILE_BASIC_INFO{int stamp=37;};
struct BY_HANDLE_FILE_INFORMATION{unsigned nNumberOfLinks=1,dwFileAttributes=0;};
struct ShortcutLock{HANDLE file=1;};
struct Session{ShortcutLock shortcutLock;std::wstring shortcutBackup=L"old",shortcutRetargetBackup=L"new",shortcutRetargetExe=L"exe";
 std::vector<BYTE> shortcutSecurity{1,2};FILE_BASIC_INFO shortcutBasic;
 bool shortcutPreserved=true,shortcutRetargetAttempted=false;};
struct State{std::map<std::wstring,std::string> files;std::map<int,std::pair<std::wstring,unsigned>> handles;
 unsigned writes=0,closes=0;int next=10;bool writeFails=false,shortWrite=false,readFails=false,flushFails=false,truncateFails=false,
 changedSecurity=false,missingTarget=false,safe=true,privilege=true,metadataFails=false;unsigned attrs=0,links=1;} f;
struct ScopedPrivilege{bool enabled;explicit ScopedPrivilege(int):enabled(f.privilege){}};
HANDLE CreateFileW(const wchar_t* path,unsigned,unsigned,void*,unsigned,unsigned,void*){if(!f.files.count(path))return -1;
 int id=f.next++;f.handles[id]={path,0};return id;}
void CloseHandle(HANDLE h){assert(h>=10);f.handles.erase(h);++f.closes;}
bool GetFileInformationByHandle(HANDLE,BY_HANDLE_FILE_INFORMATION* info){info->nNumberOfLinks=f.links;info->dwFileAttributes=f.attrs;return true;}
bool GetFileSizeEx(HANDLE h,LARGE_INTEGER* size){size->QuadPart=f.files.at(f.handles.at(h).first).size();return true;}
bool SetFilePointerEx(HANDLE h,LARGE_INTEGER,void*,unsigned){f.handles.at(h).second=0;return true;}
bool ReadFile(HANDLE h,void* out,unsigned count,unsigned* read,void*){
 if(f.readFails)return false;auto& open=f.handles.at(h);auto& bytes=f.files.at(open.first);
 *read=std::min<unsigned>(count,bytes.size()-open.second);std::memcpy(out,bytes.data()+open.second,*read);open.second+=*read;return true;}
bool WriteFile(HANDLE h,const void* in,unsigned count,unsigned* written,void*){
 ++f.writes;if(f.writeFails)return false;auto& open=f.handles.at(h);auto& bytes=f.files.at(open.first);
 *written=f.shortWrite?count/2:count;bytes.resize(std::max<unsigned>(bytes.size(),open.second+*written));
 std::memcpy(bytes.data()+open.second,in,*written);open.second+=*written;return true;}
bool SetEndOfFile(HANDLE h){if(f.truncateFails)return false;auto& open=f.handles.at(h);f.files.at(open.first).resize(open.second);return true;}
bool FlushFileBuffers(HANDLE){return !f.flushFails;}
bool SetFileInformationByHandle(HANDLE,int,void*,unsigned){return !f.metadataFails;}
unsigned GetFileAttributesW(const wchar_t*){return f.missingTarget?INVALID_FILE_ATTRIBUTES:0;}
bool ReadFileSecurity(HANDLE,std::vector<BYTE>& out){out=f.changedSecurity?std::vector<BYTE>{9}:std::vector<BYTE>{1,2};return true;}
namespace GlassInstallUtil{bool IsSafeInstallPath(const std::wstring&){return f.safe;}}
std::wstring Problem(bool,const wchar_t*,const wchar_t* e){return e;}
bool RemoveShortcut(Session&,bool,std::wstring&){return true;}
// CODE
bool RestoreRetarget(Session& session){bool ok=true;bool russian=false;std::wstring error;
// RESTORE
return ok;}
void reset(){f={};f.files={{L"old","custom original link data with arguments"},{L"new","new link"},{L"shared","custom original link data with arguments"}};
 f.files[L"shared"]=f.files[L"old"];f.handles[1]={L"shared",0};}
int main(){
 {reset();Session s;std::wstring error;assert(CommitShortcut(s,false,error));assert(f.files[L"shared"]==f.files[L"new"]);
  assert(s.shortcutRetargetAttempted&&s.shortcutSecurity==std::vector<BYTE>({1,2}));
  assert(RestoreRetarget(s)&&!s.shortcutRetargetAttempted&&f.files[L"shared"]==f.files[L"old"]);assert(f.handles.size()==1);}
 for(int n=0;n<6;++n){reset();Session s;std::wstring error;
  if(n==0)f.changedSecurity=true;if(n==1)f.missingTarget=true;if(n==2)f.safe=false;
  if(n==3)f.privilege=false;if(n==4)f.files[L"shared"]="concurrently edited";if(n==5)s.shortcutLock.file=-1;
  assert(!CommitShortcut(s,false,error)&&f.writes==0&&!s.shortcutRetargetAttempted);}
 for(int n=0;n<5;++n){reset();Session s;std::wstring error;
  if(n==0)f.writeFails=true;if(n==1)f.shortWrite=true;if(n==2)f.readFails=true;if(n==3)f.flushFails=true;if(n==4)f.truncateFails=true;
  assert(!CommitShortcut(s,false,error));
  f.writeFails=f.shortWrite=f.readFails=f.flushFails=f.truncateFails=false;
  assert(RestoreRetarget(s)&&f.files[L"shared"]==f.files[L"old"]&&f.handles.size()==1);}
 {reset();Session s;std::wstring error;assert(CommitShortcut(s,false,error));f.changedSecurity=true;
  assert(!RestoreRetarget(s)&&s.shortcutRetargetAttempted);}
 {reset();Session s;s.shortcutRetargetBackup.clear();std::wstring error;assert(CommitShortcut(s,false,error)&&f.writes==0);}
 std::cout<<"PASS: actual shortcut commit/restore streams: original bytes/security guard, retained target, short/failed writes, truncation/flush errors and rollback refusal\n";
}
'''.replace('// CODE',same+code).replace('// RESTORE',restore)
with tempfile.TemporaryDirectory(prefix='ShortcutRetarget-') as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(h)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-no-pie',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
 # Compile the real Session/RAII declaration with type-only Windows stubs.
 # This catches accidental copies/double-close/reset leaks, not OS locking.
 (Path(d)/'windows.h').write_text('''#pragma once
using HANDLE=int; using BYTE=unsigned char;
constexpr int INVALID_HANDLE_VALUE=-1;
struct FILE_BASIC_INFO { int stamp=0; };
inline int closeCount=0;
inline bool CloseHandle(HANDLE){++closeCount;return true;}
''')
 (Path(d)/'GlassPlatformTask.h').write_text('''#pragma once
namespace GlassShared::GlassPlatformTask {enum class PlatformArch {X64,X86};}
''')
 (Path(d)/'session.cpp').write_text('''#include "GlassSetup/src/InstallSupport/GlassInstallRemoval.h"
#include <cassert>
int main(){using GlassInstallRemoval::Session;
 {Session a;a.shortcutLock.file=7;Session b=std::move(a);assert(a.shortcutLock.file==-1);b={};assert(closeCount==1);
  a.shortcutLock.file=8;Session c;c=std::move(a);assert(a.shortcutLock.file==-1);}
 assert(closeCount==2);
}
''')
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',d,'-I',str(root),str(Path(d)/'session.cpp'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
