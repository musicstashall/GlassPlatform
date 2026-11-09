"""Real profile inventory/preflight/commit methods with fake registry/filesystem APIs."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassProfileMaintenance.h').read_text()
def function(token):
    start=s.index(token);brace=s.index('{',start);depth=0
    for i in range(brace,len(s)):
        if s[i]=='{':depth+=1
        elif s[i]=='}':
            depth-=1
            if not depth:return s[start:i+1]
    raise ValueError(token)
prefix=r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <map>
#include <string>
#include <vector>
#include <utility>
using DWORD=std::uint32_t;using BYTE=unsigned char;using LONG=int;using HKEY=void*;using PSID=void*;
HKEY HKEY_LOCAL_MACHINE=reinterpret_cast<HKEY>(1),HKEY_USERS=reinterpret_cast<HKEY>(2);
constexpr LONG ERROR_SUCCESS=0,ERROR_BADDB=1009,ERROR_FILE_NOT_FOUND=2,ERROR_NO_MORE_ITEMS=259;
constexpr DWORD KEY_READ=1,KEY_WRITE=2,KEY_WOW64_64KEY=4,KEY_ALL_ACCESS=7,REG_PROCESS_APPKEY=1,REG_SZ=1,REG_EXPAND_SZ=2,
 FILE_ATTRIBUTE_DIRECTORY=16,FILE_ATTRIBUTE_REPARSE_POINT=1024,INVALID_FILE_ATTRIBUTES=0xffffffff;
struct Entry{std::wstring sid,path;bool loaded=false;};
std::vector<Entry> entries;
struct Handle{int kind,index;};std::map<HKEY,Handle> handles;std::uintptr_t serial=10;
std::map<std::wstring,DWORD> attributes;
bool loadBadDb=false,fallbackOk=false;int fallbackCalls=0,ownedUnloads=0;
bool enumFail=false,openListFail=false,openEntryFail=false,openHiveFail=false,loadFail=false,invalidSid=false,badPathValue=false,otherSession=false;
int malformed=0,changeOnCheck=0;
int offlineLoads=0,cleanChecks=0,cleanWrites=0;int failCheckAt=0,failWriteAt=0;
HKEY create(int kind,int index){HKEY h=reinterpret_cast<HKEY>(++serial);handles[h]={kind,index};return h;}
LONG RegCloseKey(HKEY h){assert(handles.erase(h)==1);return 0;}
LONG RegOpenKeyExW(HKEY root,const wchar_t* name,DWORD,DWORD,HKEY* out){
 if(root==HKEY_LOCAL_MACHINE){if(openListFail)return 5;*out=create(0,0);return 0;}
 for(std::size_t i=0;i<entries.size();++i)if(entries[i].sid==name){
  if(root==HKEY_USERS){if(openHiveFail)return 5;if(!entries[i].loaded)return 2;*out=create(2,i);return 0;}
  assert(handles.at(root).kind==0);if(openEntryFail)return 5;*out=create(1,i);return 0;
 }return 2;
}
LONG RegEnumKeyExW(HKEY key,DWORD index,wchar_t* name,DWORD* size,void*,void*,void*,void*){
 assert(handles.at(key).kind==0);if(enumFail)return 5;if(index>=entries.size())return ERROR_NO_MORE_ITEMS;
 assert(entries[index].sid.size()<*size);std::wcscpy(name,entries[index].sid.c_str());*size=entries[index].sid.size();return 0;
}
LONG RegQueryValueExW(HKEY key,const wchar_t*,void*,DWORD* type,BYTE* data,DWORD* size){
 if(badPathValue)return 5;const auto& text=entries[handles.at(key).index].path;*type=REG_SZ;
 const DWORD needed=(text.size()+1)*sizeof(wchar_t);if(data){assert(*size>=needed);std::memcpy(data,text.c_str(),needed);
 if(malformed==1)*type=3;
 if(malformed==2)reinterpret_cast<wchar_t*>(data)[text.size()]=L'x';
 if(malformed==3)reinterpret_cast<wchar_t*>(data)[1]=0;
 }*size=needed;if(malformed==4)--*size;return 0;
}
DWORD ExpandEnvironmentStringsW(const wchar_t* text,wchar_t* out,DWORD size){DWORD need=std::wcslen(text)+1;if(need<=size)std::wcscpy(out,text);return need;}
int ConvertStringSidToSidW(const wchar_t*,PSID* sid){if(invalidSid)return 0;*sid=std::malloc(1);return 1;}
void LocalFree(void* p){std::free(p);}
DWORD GetFileAttributesW(const wchar_t* raw){std::wstring path=raw;auto it=attributes.find(path);if(it!=attributes.end())return it->second;return path.find(L"NTUSER.DAT")!=std::wstring::npos?128:FILE_ATTRIBUTE_DIRECTORY;}
LONG RegLoadAppKeyW(const wchar_t* path,HKEY* out,DWORD,DWORD,DWORD){++offlineLoads;if(loadBadDb)return ERROR_BADDB;if(loadFail)return 5;for(std::size_t i=0;i<entries.size();++i)if(entries[i].path+L"\\NTUSER.DAT"==path){*out=create(2,i);return 0;}return 2;}
bool OtherUserSessions(std::wstring& detail){if(otherSession)detail=L"Other session";return otherSession;}
namespace Shared::RegistrySanity {
bool CleanOwnedSettings(HKEY,bool,bool commit,std::wstring& detail){
 int n=commit?++cleanWrites:++cleanChecks;
 if(!commit&&changeOnCheck==n)entries.push_back({L"S-1-5-21-333",L"C:\\Users\\C"});
 if(n==(commit?failWriteAt:failCheckAt)){detail=L"failure";return false;}return true;
}}
'''
prefix+=r'''
int themeChecks=0,themeCommits=0,failThemeCheck=0,failThemeCommit=0;
namespace GlassSavedThemeCleanup {struct Batch {bool Prepare(const std::wstring&,HKEY,bool (*)(const std::wstring&),std::wstring&){return ++themeChecks!=failThemeCheck;}bool Cleanup(HKEY,bool,bool&,std::wstring&){return ++themeCommits!=failThemeCommit;}};}
bool IsDesktopProfile(const std::wstring&){return false;}
'''
prefix+='\nbool deletedLocal=false;bool DeletedLocalProfile(const std::wstring& sid){return deletedLocal&&sid==L"S-1-5-21-222";}\n'
prefix+=r'''
namespace GlassInstallLog {template<class... T>void LineFormat(const wchar_t*,T...) {}}
namespace GlassOfflineUserHive {
 bool Unmount(const std::wstring& name){assert(name==L"owned-test");++ownedUnloads;return true;}
 bool LoadMounted(const std::wstring& path,HKEY& hive,std::wstring& mount,std::wstring&){++fallbackCalls;if(!fallbackOk)return false;
  for(std::size_t n=0;n<entries.size();++n)if(entries[n].path+L"\\NTUSER.DAT"==path){hive=create(2,n);mount=L"owned-test";return true;}return false;}
}
'''
profile=s[s.index('struct Profile {'):s.index('inline bool ReadString(')]
methods='\n'.join(function(token) for token in ('bool OpenAll(', 'bool VerifyInventory(', 'bool PreflightCleanup(', 'bool Cleanup('))
body=prefix+profile+function('inline bool ReadString(')+function('inline bool SafePath(')+'''\nclass Inventory {
 std::vector<Profile> profiles_;bool skipDeletedLocal_=false;bool removalStylePrepared_=false,removalStyleCommitted_=false;bool cleanupPrepared_=false;bool savedThemeRegistryFallback_=false;
 public:
 const std::vector<Profile>& Profiles() const {return profiles_;}
'''+methods+'\n};\n'
main=r'''
void reset(){loadBadDb=fallbackOk=false;fallbackCalls=ownedUnloads=0;deletedLocal=false;assert(handles.empty());entries={{L"S-1-5-21-111",L"C:\\Users\\A"},{L"S-1-12-1-222",L"C:\\Users\\B"}};attributes.clear();enumFail=openListFail=openEntryFail=openHiveFail=loadFail=invalidSid=badPathValue=otherSession=false;offlineLoads=cleanChecks=cleanWrites=failCheckAt=failWriteAt=malformed=changeOnCheck=0;themeChecks=themeCommits=failThemeCheck=failThemeCommit=0;}
int main(){
 std::wstring orphanDetail;
 reset();{loadBadDb=fallbackOk=true;entries[0].loaded=true;Inventory i;assert(i.OpenAll(orphanDetail,true));assert(i.Profiles().size()==2);assert(fallbackCalls==1);assert(i.VerifyInventory(orphanDetail));}assert(ownedUnloads==1&&handles.empty());
 reset();{loadBadDb=fallbackOk=true;Inventory i;assert(!i.OpenAll(orphanDetail));assert(fallbackCalls==0);} // Never broaden general cleanup loader.
 reset();{loadBadDb=true;fallbackOk=false;Inventory i;assert(!i.OpenAll(orphanDetail,true));assert(fallbackCalls==1&&handles.empty());}

 reset();{entries[1].sid=L"S-1-5-21-222";deletedLocal=true;loadFail=true;entries[0].loaded=true;
  Inventory i;assert(i.OpenAll(orphanDetail,true));assert(i.Profiles().size()==1);assert(offlineLoads==0);assert(i.VerifyInventory(orphanDetail));
  deletedLocal=false;assert(!i.VerifyInventory(orphanDetail));}
 reset();{entries[1].sid=L"S-1-5-21-222";deletedLocal=true;loadFail=true;entries[0].loaded=true;
  Inventory i;assert(!i.OpenAll(orphanDetail));} // Cleanup remains strict.

 std::wstring detail;
 reset();{Inventory i;assert(!i.Cleanup(detail));assert(cleanWrites==0);assert(i.PreflightCleanup(detail));assert(i.Profiles().size()==2&&offlineLoads==2);assert(cleanChecks==3);assert(i.Cleanup(detail));assert(cleanWrites==3);assert(!i.Cleanup(detail));assert(cleanWrites==3);}
 reset();{entries[0].loaded=true;Inventory i;assert(i.PreflightCleanup(detail));assert(offlineLoads==1);}
 for(int bad=1;bad<=4;++bad){reset();malformed=bad;Inventory i;assert(!i.PreflightCleanup(detail));assert(i.Profiles().empty());assert(handles.empty());assert(cleanWrites==0);}
 // A second profile error discards the first already-opened profile too.
 for(auto attr:{INVALID_FILE_ATTRIBUTES,FILE_ATTRIBUTE_REPARSE_POINT,128u}){
  reset();attributes[L"C:\\Users\\B"]=attr;Inventory i;assert(!i.PreflightCleanup(detail));assert(i.Profiles().empty());assert(handles.empty());assert(!i.Cleanup(detail));assert(cleanWrites==0);
 }
 for(auto attr:{INVALID_FILE_ATTRIBUTES,FILE_ATTRIBUTE_REPARSE_POINT,FILE_ATTRIBUTE_DIRECTORY}){
  reset();attributes[L"C:\\Users\\B\\NTUSER.DAT"]=attr;entries[1].loaded=true;Inventory i;assert(!i.PreflightCleanup(detail));assert(i.Profiles().empty());assert(handles.empty());
 }
 reset();attributes[L"C:\\Users"]=FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT;{Inventory i;assert(!i.PreflightCleanup(detail));assert(cleanWrites==0);}
 for(int failure=0;failure<7;++failure){
  reset();enumFail=failure==0;openListFail=failure==1;openEntryFail=failure==2;openHiveFail=failure==3;loadFail=failure==4;invalidSid=failure==5;badPathValue=failure==6;
  Inventory i;assert(!i.PreflightCleanup(detail));assert(i.Profiles().empty());assert(!i.Cleanup(detail));assert(cleanWrites==0&&handles.empty());
 }
 reset();{Inventory i;otherSession=true;assert(!i.PreflightCleanup(detail));assert(handles.empty()&&cleanChecks==0);}
 for(int failure=1;failure<=3;++failure){reset();failCheckAt=failure;Inventory i;assert(!i.PreflightCleanup(detail));assert(!i.Cleanup(detail));assert(cleanWrites==0);}
 reset();{Inventory i;assert(i.PreflightCleanup(detail));otherSession=true;assert(!i.Cleanup(detail));assert(cleanWrites==0);otherSession=false;assert(!i.Cleanup(detail));}
 reset();{Inventory i;assert(i.PreflightCleanup(detail));failWriteAt=2;assert(!i.Cleanup(detail));assert(cleanWrites==2);assert(!i.Cleanup(detail));assert(cleanWrites==2);assert(i.PreflightCleanup(detail));failWriteAt=0;assert(i.Cleanup(detail));}
 reset();{Inventory i;assert(i.PreflightCleanup(detail));assert(i.OpenAll(detail));assert(!i.Cleanup(detail));assert(cleanWrites==0);}
 reset();{Inventory i;changeOnCheck=3;assert(!i.PreflightCleanup(detail));assert(!i.Cleanup(detail));assert(cleanWrites==0);}
 reset();{Inventory i;failThemeCheck=2;assert(!i.PreflightCleanup(detail));assert(!i.Cleanup(detail));assert(cleanWrites==0&&themeCommits==0);}
 reset();{Inventory i;assert(i.PreflightCleanup(detail));failThemeCommit=2;assert(!i.Cleanup(detail));assert(cleanWrites==1);assert(!i.Cleanup(detail));assert(cleanWrites==1);}
 // Reordering is benign; metadata changes require a new preflight and no writes.
 reset();{Inventory i;assert(i.PreflightCleanup(detail));std::swap(entries[0],entries[1]);assert(i.Cleanup(detail));assert(cleanWrites==3);}
 for(int mutation=0;mutation<10;++mutation){
  reset();Inventory i;assert(i.PreflightCleanup(detail));const auto pinned=handles.size();
  if(mutation==0)entries.push_back({L"S-1-5-21-333",L"C:\\Users\\C"});
  if(mutation==1)entries.pop_back();
  if(mutation==2)entries[1].path=L"C:\\Users\\Moved";
  if(mutation==3)enumFail=true;
  if(mutation==4)attributes[L"C:\\Users\\B"]=FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT;
  if(mutation==5)attributes[L"C:\\Users\\B\\NTUSER.DAT"]=INVALID_FILE_ATTRIBUTES;
  if(mutation==6)entries[1].sid=entries[0].sid;
  if(mutation==7)badPathValue=true;
  if(mutation==8)openListFail=true;
  if(mutation==9)openEntryFail=true;
  assert(!i.Cleanup(detail));assert(cleanWrites==0);assert(handles.size()==pinned);
  assert(!i.Cleanup(detail));assert(cleanWrites==0); // Consent is not reusable after failed revalidation.
 }
 assert(handles.empty());
}
'''
assert 'LookupAccountSidW' not in function('bool OpenAll(')
assert 'ProfileSidHasLiveAccount' not in s
with tempfile.TemporaryDirectory() as d:
    source=Path(d)/'profiles.cpp';source.write_text(body+main)
    binary=Path(d)/'profiles'
    subprocess.run(['g++','-std=c++17','-fsanitize=address,undefined','-fno-pie','-no-pie',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: actual all-profile inventory/preflight/one-shot cleanup; inaccessible hives, unsafe paths, partial inventories, inventory changes before commit, sessions, errors and handle release (fake APIs)')
