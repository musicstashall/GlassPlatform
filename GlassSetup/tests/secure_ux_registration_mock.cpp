// Portable fake registry for the ACTUAL Native registration implementation.
// The runner inserts its source at the marker. No real Windows operations.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>
using BYTE=unsigned char; using DWORD=std::uint32_t; using LONG=std::int32_t;
using HRESULT=std::int32_t; using HANDLE=void*;
struct Key {std::wstring path;}; using HKEY=Key*;
constexpr HKEY HKEY_LOCAL_MACHINE=nullptr;
constexpr LONG ERROR_SUCCESS=0,ERROR_FILE_NOT_FOUND=2,ERROR_ACCESS_DENIED=5,ERROR_INVALID_DATA=13,
    ERROR_BUSY=170,ERROR_RETRY=1237,ERROR_NOT_ENOUGH_MEMORY=8,ERROR_PRIVILEGE_NOT_HELD=1314,
    ERROR_TRANSACTIONAL_CONFLICT=6800;
constexpr DWORD KEY_QUERY_VALUE=1,KEY_WOW64_64KEY=0x100,REG_OPTION_BACKUP_RESTORE=4,
    REG_NONE=0,REG_SZ=1,REG_EXPAND_SZ=2,REG_DWORD=4,REG_BINARY=3,WAIT_OBJECT_0=0,WAIT_ABANDONED=128;
constexpr HRESULT S_OK=0;
constexpr bool FALSE=false;
#ifndef _countof
#define _countof(a) (sizeof(a)/sizeof((a)[0]))
#endif
#ifndef _WIN32
#define _wcsicmp wcscasecmp
#define wcstok_s wcstok
#endif
#define DBG_ERROR 3
HRESULT HRESULT_FROM_WIN32(LONG code){return code?static_cast<HRESULT>(0x80070000u|(code&0xffffu)):0;}
void DebugLog(int,const wchar_t*){}
struct Stored {DWORD type;std::vector<BYTE> data;bool operator==(const Stored& b)const{return type==b.type&&data==b.data;}};
using Values=std::map<std::wstring,Stored>;
std::map<std::wstring,Values> registry;
int writes=0,failWrite=0;bool injectConcurrent=false,native64=true;
LONG RegOpenKeyExW(HKEY,const wchar_t* path,DWORD,DWORD,HKEY* key){
    if(!registry.count(path))return ERROR_FILE_NOT_FOUND;
    *key=new Key{path};return 0;
}
LONG RegCreateKeyExW(HKEY,const wchar_t* path,DWORD,void*,DWORD options,DWORD,void*,HKEY* key,DWORD* disposition){
    assert(options==REG_OPTION_BACKUP_RESTORE);*disposition=registry.count(path)?2:1;
    registry[path];*key=new Key{path};return 0;
}
LONG RegQueryValueExW(HKEY key,const wchar_t* name,void*,DWORD* type,BYTE* data,DWORD* bytes){
    const auto& values=registry.at(key->path);auto it=values.find(name);
    if(it==values.end())return ERROR_FILE_NOT_FOUND;
    *type=it->second.type;const auto n=static_cast<DWORD>(it->second.data.size());
    if(data){if(*bytes<n)return 234;std::memcpy(data,it->second.data.data(),n);}*bytes=n;return 0;
}
void ConcurrentEdit();
bool FailWrite(){if(++writes==failWrite){if(injectConcurrent)ConcurrentEdit();return true;}return false;}
LONG RegSetValueExW(HKEY key,const wchar_t* name,DWORD,DWORD type,const BYTE* data,DWORD bytes){
    if(FailWrite())return ERROR_ACCESS_DENIED;
    registry[key->path][name]={type,{data,data+bytes}};return 0;
}
LONG RegDeleteValueW(HKEY key,const wchar_t* name){
    if(FailWrite())return ERROR_ACCESS_DENIED;
    return registry[key->path].erase(name)?0:ERROR_FILE_NOT_FOUND;
}
HANDLE CreateMutexW(void*,bool,const wchar_t*){return reinterpret_cast<HANDLE>(1);}
DWORD WaitForSingleObject(HANDLE,DWORD){return 0;}
void ReleaseMutex(HANDLE){} DWORD GetLastError(){return 0;}
DWORD ExpandEnvironmentStringsW(const wchar_t* input,wchar_t* out,DWORD size){
    std::wstring text=input;const std::wstring token=L"%SystemRoot%";
    const auto at=text.find(token);if(at!=std::wstring::npos)text.replace(at,token.size(),L"C:\\Windows");
    const auto n=static_cast<DWORD>(text.size()+1);if(n<=size)std::copy(text.c_str(),text.c_str()+n,out);return n;
}
namespace wil {
struct unique_hkey {HKEY h=nullptr;~unique_hkey(){delete h;}HKEY* operator&(){return &h;}HKEY get()const{return h;}};
struct unique_handle {HANDLE h;explicit unique_handle(HANDLE value):h(value){}HANDLE get()const{return h;}explicit operator bool()const{return h!=nullptr;}};
template<class F>struct Exit {F f;~Exit(){f();}};
template<class F>Exit<F> scope_exit(F f){return {f};}
}
struct RegistrationPrivileges {bool ready=true;}; // Privilege semantics require Windows VM testing.
std::wstring NativeSystem32(){return L"C:\\Windows\\System32";}
std::wstring JoinPath(const std::wstring& folder,const wchar_t* name){return folder+L"\\"+name;}
bool IsNative64(){return native64;}

// INSERT_NATIVE_REGISTRATION_HERE

void PutString(const wchar_t* key,const wchar_t* value,const std::wstring& text,DWORD type=REG_SZ){
    const auto* bytes=reinterpret_cast<const BYTE*>(text.c_str());
    registry[key][value]={type,{bytes,bytes+(text.size()+1)*sizeof(wchar_t)}};
}
void PutFlags(const std::wstring& key,DWORD bits){
    const auto* bytes=reinterpret_cast<const BYTE*>(&bits);registry[key][L"GlobalFlag"]={REG_DWORD,{bytes,bytes+sizeof(bits)}};
}
std::wstring String(const std::wstring& key,const wchar_t* value){
    const auto& data=registry.at(key).at(value).data;
    return reinterpret_cast<const wchar_t*>(data.data());
}
DWORD Flags(){DWORD bits=0;auto& data=registry.at(IfeoSubkey(L"winlogon.exe")).at(L"GlobalFlag").data;
    std::memcpy(&bits,data.data(),sizeof(bits));return bits;}
auto Nonempty(){auto copy=registry;for(auto it=copy.begin();it!=copy.end();)if(it->second.empty())it=copy.erase(it);else ++it;return copy;}
void Reset(){
    registry.clear();writes=0;failWrite=0;injectConcurrent=false;native64=true;
    for(const auto* keys:{&kInProcKeysNative,&kInProcKeysWow})for(const auto* key:*keys)
        PutString(key,L"",kInProcThemeUi,REG_EXPAND_SZ);
}
void ConcurrentEdit(){PutString(kInProcKeysNative[0],L"",L"C:\\Foreign\\server.dll");}
int main(){
    Reset();assert(ToggleRegistration(true)==0);assert(Flags()==0x100);
    for(const auto* keys:{&kInProcKeysNative,&kInProcKeysWow})for(const auto* key:*keys)assert(String(key,L"")==kInProcProxy);
    assert(ToggleRegistration(false)==0);assert(!registry.at(IfeoSubkey(L"winlogon.exe")).count(L"VerifierDlls"));
    assert(!registry.at(IfeoSubkey(L"winlogon.exe")).count(L"GlobalFlag"));
    for(const auto* keys:{&kInProcKeysNative,&kInProcKeysWow})for(const auto* key:*keys)assert(String(key,L"")==kInProcThemeUi);
    Reset();PutFlags(IfeoSubkey(L"winlogon.exe"),0x142);
    PutString(IfeoSubkey(L"winlogon.exe").c_str(),L"VerifierDlls",L"other.dll");
    assert(ToggleRegistration(true)==0&&Flags()==0x142);
    assert(ToggleRegistration(false)==0&&Flags()==0x142);
    assert(String(IfeoSubkey(L"winlogon.exe"),L"VerifierDlls")==L"other.dll");
    Reset();PutFlags(IfeoSubkey(L"winlogon.exe"),0x40);
    assert(ToggleRegistration(true)==0&&Flags()==0x140);assert(ToggleRegistration(false)==0&&Flags()==0x40);
    Reset();for(auto* key:kInProcKeysNative)PutString(key,L"",kInProcProxy);
    assert(ToggleRegistration(false)==0); // Permanent proxy/COM residue, no patcher/MSI needed for off.
    Reset();PutString(kInProcKeysNative[1],L"",L"C:\\Foreign\\server.dll");auto before=Nonempty();
    assert(ToggleRegistration(true)<0&&writes==0&&Nonempty()==before);
    Reset();registry[IfeoSubkey(L"winlogon.exe")][L"VerifierDlls"]={REG_BINARY,{1,2}};before=Nonempty();
    assert(ToggleRegistration(true)<0&&writes==0&&Nonempty()==before);
    for(int failure=1;failure<=6;++failure){Reset();before=Nonempty();failWrite=failure;
        assert(ToggleRegistration(true)<0&&Nonempty()==before);}
    Reset();failWrite=5;injectConcurrent=true;
    assert(ToggleRegistration(true)==HRESULT_FROM_WIN32(ERROR_TRANSACTIONAL_CONFLICT));
    assert(String(kInProcKeysNative[0],L"")==L"C:\\Foreign\\server.dll");
    Reset();native64=false;PutString(kInProcKeysWow[0],L"",L"C:\\Foreign\\server.dll");
    assert(ToggleRegistration(true)==0);assert(String(kInProcKeysWow[0],L"")==L"C:\\Foreign\\server.dll");
    Reset();for(const auto* exe:{L"winlogon.exe",L"explorer.exe",L"SystemSettings.exe",L"LogonUI.exe",L"dwm.exe"}){
        PutString(IfeoSubkey(exe).c_str(),L"VerifierDlls",L"other.dll SecureUxTheme4.dll");PutFlags(IfeoSubkey(exe),0x140);}
    assert(ToggleRegistration(false)==0);
    for(const auto* exe:{L"winlogon.exe",L"explorer.exe",L"SystemSettings.exe",L"LogonUI.exe",L"dwm.exe"})
        assert(String(IfeoSubkey(exe),L"VerifierDlls")==L"other.dll");
    std::cout<<"PASS: actual Native registration logic with fake registry: on/off, residues, x86/x64 views, foreign values, 6 rollback failure points and concurrent edit preservation\n";
}
