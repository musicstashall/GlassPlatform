"""Actual installed-Native path/ACL and publisher routing with fake Windows APIs.
Models Windows servicing + inherit-only ACEs; not a real Windows ACL/SIP test.
"""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'GlassSetup/src/InstallSupport/GlassInstallUtil.cpp').read_text()
start=source.index('bool NativeAncestorsProtected(')
end=source.index('\n} // namespace',start)
code=source[start:end]
helper=source.index('std::wstring InstalledSecureUxThemeNativeDll(')
helper_end=source.index('\n}\n}',helper)+2
route=source.index('std::wstring NativeDllForArch(',helper_end)
route_end=source.index('\n}\n',route)+2
code+='\n'+source[helper:helper_end]+'\n'+source[route:route_end]
prefix=r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <map>
#include <string>
#include <vector>
using DWORD=std::uint32_t;using BYTE=unsigned char;using PSID=void*;using WORD=unsigned short;
namespace GlassShared::GlassPlatformTask{enum class PlatformArch{X64,X86};}
namespace Shared::InjectModuleScan{constexpr char kNativeInstallerProtocolExport[]="protocol";}
constexpr WORD IMAGE_FILE_MACHINE_AMD64=0x8664,IMAGE_FILE_MACHINE_I386=0x14c;
using LPWSTR=wchar_t*;using PSECURITY_DESCRIPTOR=void*;using HANDLE=void*;
#define _countof(x) (sizeof(x)/sizeof((x)[0]))
#define INVALID_HANDLE_VALUE reinterpret_cast<HANDLE>(-1)
constexpr DWORD SECURITY_MAX_SID_SIZE=68,WinBuiltinAdministratorsSid=1,WinLocalSystemSid=2,
 FILE_ATTRIBUTE_DIRECTORY=16,FILE_ATTRIBUTE_REPARSE_POINT=1024,INVALID_FILE_ATTRIBUTES=0xffffffffu,
 SE_FILE_OBJECT=1,OWNER_SECURITY_INFORMATION=1,DACL_SECURITY_INFORMATION=4,ERROR_SUCCESS=0,
 GENERIC_ALL=1<<28,GENERIC_WRITE=1<<30,GENERIC_READ=1u<<31,DELETE=1<<16,WRITE_DAC=1<<18,
 WRITE_OWNER=1<<19,FILE_DELETE_CHILD=64,FILE_WRITE_DATA=2,FILE_APPEND_DATA=4,FILE_WRITE_EA=16,
 FILE_WRITE_ATTRIBUTES=256,FILE_SHARE_READ=1,OPEN_EXISTING=3,FILE_ATTRIBUTE_NORMAL=128;
constexpr BYTE INHERIT_ONLY_ACE=8,ACCESS_DENIED_ACE_TYPE=1,ACCESS_ALLOWED_ACE_TYPE=0;
struct ACE_HEADER{BYTE AceType=0,AceFlags=0;};
struct ACCESS_ALLOWED_ACE{ACE_HEADER Header;DWORD Mask=0,SidStart=0;};
struct ACL{DWORD AceCount=0;std::vector<ACCESS_ALLOWED_ACE> entries;};using PACL=ACL*;
struct Node{DWORD attr=FILE_ATTRIBUTE_DIRECTORY,owner=1;ACL acl;bool nullAcl=false,nullOwner=false;};
struct State{std::map<std::wstring,Node> nodes;int allocations=0,handles=0,signatures=0;
 bool signedDll=true,signedSetup=true,samePublisher=true,open=true,pe=true,protocol=true;} s;
constexpr DWORD servicing=3,users=4,creator=5;
DWORD Sid(PSID sid){DWORD n=0;std::memcpy(&n,sid,sizeof(n));return n;}
bool EqualSid(PSID a,PSID b){return a&&b&&Sid(a)==Sid(b);}
bool CreateWellKnownSid(DWORD type,void*,BYTE* data,DWORD*){std::memcpy(data,&type,4);return true;}
bool ConvertStringSidToSidW(const wchar_t*,PSID* out){*out=new DWORD(servicing);++s.allocations;return true;}
void LocalFree(void* p){delete static_cast<DWORD*>(p);--s.allocations;}
DWORD GetFileAttributesW(const wchar_t* p){auto it=s.nodes.find(p);return it==s.nodes.end()?INVALID_FILE_ATTRIBUTES:it->second.attr;}
DWORD GetNamedSecurityInfoW(LPWSTR path,DWORD,DWORD,PSID* owner,void*,PACL* acl,void*,PSECURITY_DESCRIPTOR* sd){
 auto& n=s.nodes.at(path);*owner=n.nullOwner?nullptr:&n.owner;*acl=n.nullAcl?nullptr:&n.acl;
 *sd=new DWORD(0);++s.allocations;return 0;
}
bool GetAce(PACL acl,DWORD i,void** raw){*raw=&acl->entries.at(i);return true;}
namespace GlassInstallLog{template<class... T>void LineFormat(const wchar_t*,T...){} }
std::wstring TrimTrailingSlashes(std::wstring p){while(!p.empty()&&p.back()==L'\\')p.pop_back();return p;}
HANDLE CreateFileW(const wchar_t*,DWORD access,DWORD share,void*,DWORD,DWORD,void*){
 assert(access==GENERIC_READ&&share==FILE_SHARE_READ);if(!s.open)return INVALID_HANDLE_VALUE;
 ++s.handles;return reinterpret_cast<HANDLE>(1);
}
DWORD GetLastError(){return 5;}
void CloseHandle(HANDLE){--s.handles;}
DWORD GetModuleFileNameW(void*,wchar_t* p,DWORD){std::wcscpy(p,L"C:\\kit\\GlassSetup.exe");return std::wcslen(p);}
bool PeMatches(const std::wstring&,WORD,const char* name){
 return s.pe&&(std::string(name)!="protocol"||s.protocol);
}
bool SignedPublisher(const std::wstring& p,std::array<BYTE,32>& hash){
 ++s.signatures;bool setup=p==L"C:\\kit\\GlassSetup.exe";hash.fill(setup||s.samePublisher?1:2);
 return setup?s.signedSetup:s.signedDll;
}
'''
main=r'''
const std::wstring root=L"C:\\Program Files\\GlassPlatform",parent=root+L"\\GlassSettings",dll=parent+L"\\GlassSettings.Native.dll";
void ace(const std::wstring& p,DWORD sid,DWORD mask,BYTE flags=0){auto& a=s.nodes.at(p).acl;
 a.entries.push_back({{ACCESS_ALLOWED_ACE_TYPE,flags},mask,sid});a.AceCount=a.entries.size();}
void reset(){assert(s.allocations==0&&s.handles==0);s={};
 for(auto p:{L"C:\\",L"C:\\Program Files",root.c_str(),parent.c_str(),dll.c_str()}){
  s.nodes[p]={};ace(p,1,GENERIC_ALL);ace(p,2,GENERIC_ALL);ace(p,servicing,GENERIC_ALL);
  ace(p,creator,GENERIC_ALL,INHERIT_ONLY_ACE);ace(p,users,0);
 }
 s.nodes[L"C:\\Program Files"].owner=servicing;s.nodes[dll].attr=FILE_ATTRIBUTE_NORMAL;
 ace(L"C:\\",users,FILE_APPEND_DATA); // Default root: may create folders, not replace existing children.
}
void check(bool wanted){assert(TrustedInstalledNative(dll,root)==wanted);assert(s.allocations==0&&s.handles==0);}
int main(){
 reset();check(true);assert(s.signatures==2);
 reset();assert(NativeDllForArch(GlassShared::GlassPlatformTask::PlatformArch::X64,root,L"")==dll);
 reset();s.pe=false;assert(NativeDllForArch(GlassShared::GlassPlatformTask::PlatformArch::X64,root,L"").empty());assert(s.signatures==0);
 reset();s.protocol=false;assert(NativeDllForArch(GlassShared::GlassPlatformTask::PlatformArch::X64,root,L"").empty());
 reset();s.nodes[dll].owner=servicing;check(true);
 reset();s.nodes[parent].owner=users;check(false);assert(s.signatures==0);
 for(auto p:{L"C:\\",L"C:\\Program Files",root.c_str(),parent.c_str(),dll.c_str()}){
  for(DWORD mask:{GENERIC_WRITE,DELETE,WRITE_DAC,WRITE_OWNER,FILE_DELETE_CHILD,FILE_WRITE_DATA}){
   reset();ace(p,users,mask);check(false);assert(s.signatures==0);
  }
  reset();s.nodes[p].attr|=FILE_ATTRIBUTE_REPARSE_POINT;check(false);
  reset();s.nodes[p].nullAcl=true;check(false);
  reset();s.nodes[p].nullOwner=true;check(false);
 }
 for(DWORD mask:{FILE_APPEND_DATA,FILE_WRITE_EA,FILE_WRITE_ATTRIBUTES}){reset();ace(dll,users,mask);check(false);}
 reset();s.nodes.erase(dll);check(false);
 reset();s.nodes[dll].attr=FILE_ATTRIBUTE_DIRECTORY;check(false);
 reset();s.signedDll=false;check(false);
 reset();s.signedSetup=false;check(false);
 reset();s.samePublisher=false;check(false);
 reset();s.open=false;check(false);
 reset();assert(!TrustedInstalledNative(root+L"\\other.dll",root));assert(s.signatures==0);
 std::cout<<"PASS: actual installed Native trust: Windows servicing/inherit-only ACLs, protected ancestors/file, publisher pin, cleanup\n";
}
'''
with tempfile.TemporaryDirectory(prefix='NativeAcl-') as d:
    path=Path(d)/'test.cpp';exe=Path(d)/'test'
    path.write_text(prefix+code+main)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-no-pie',str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
