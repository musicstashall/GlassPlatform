"""Compile real cache diagnostics/directory preflight with fake Win32 APIs.
No Windows files, ACLs, products or processes are changed. This is not a Windows
integration test: it verifies refusal attribution and preservation of first error.
"""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
cache = (repo / "GlassSetup/src/InstallSupport/GlassMaintenanceCache.h").read_text(encoding="utf-8")
diag = (repo / "GlassSetup/src/InstallSupport/GlassCacheDiagnostics.h").read_text(encoding="utf-8")
diag = diag[diag.index("namespace GlassMaintenanceCache::Diagnostics {"):]


def function(token, text=cache):
    start = text.index(token)
    brace = text.index("{", start)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    raise AssertionError(token)


stubs = r'''
#include <string>
#include <vector>
#include <map>
#include <cassert>
#include <cstdint>
#include <cwchar>
#include <cstdarg>
#include <iostream>
#include <atomic>
using DWORD=unsigned long;using WORD=unsigned short;using BYTE=unsigned char;
using BOOL=int;using HRESULT=long;using LPWSTR=wchar_t*;using PWSTR=wchar_t*;using LPVOID=void*;
using PSID=void*;using PSECURITY_DESCRIPTOR=void*;
constexpr DWORD ERROR_SUCCESS=0,ERROR_ACCESS_DENIED=5,ERROR_INVALID_DATA=13,ERROR_GEN_FAILURE=31,ERROR_ALREADY_EXISTS=183;
constexpr DWORD FILE_ATTRIBUTE_DIRECTORY=16,FILE_ATTRIBUTE_REPARSE_POINT=1024,INVALID_FILE_ATTRIBUTES=0xffffffffUL;
constexpr DWORD GENERIC_ALL=0x10000000,GENERIC_WRITE=0x40000000,DELETE=0x10000,WRITE_DAC=0x40000,WRITE_OWNER=0x80000;
constexpr DWORD FILE_WRITE_DATA=2,FILE_APPEND_DATA=4,FILE_DELETE_CHILD=64,FILE_WRITE_EA=16,FILE_WRITE_ATTRIBUTES=256;
constexpr DWORD OWNER_SECURITY_INFORMATION=1,DACL_SECURITY_INFORMATION=4,SE_FILE_OBJECT=1,SDDL_REVISION_1=1;
constexpr DWORD FORMAT_MESSAGE_ALLOCATE_BUFFER=1,FORMAT_MESSAGE_FROM_SYSTEM=2,FORMAT_MESSAGE_IGNORE_INSERTS=4;
constexpr DWORD WinBuiltinAdministratorsSid=1,WinLocalSystemSid=2,SECURITY_MAX_SID_SIZE=68;
constexpr BYTE ACCESS_ALLOWED_ACE_TYPE=0,ACCESS_DENIED_ACE_TYPE=1;
constexpr BOOL FALSE=0;constexpr int FOLDERID_ProgramData=1;
struct ACE_HEADER {BYTE AceType=0,AceFlags=0;WORD AceSize=0;};
struct ACCESS_ALLOWED_ACE {ACE_HEADER Header;DWORD Mask=0,SidStart=3;};
struct ACL {DWORD AceCount=0;};using PACL=ACL*;
struct SECURITY_ATTRIBUTES {DWORD length;PSECURITY_DESCRIPTOR descriptor;BOOL inherit;};
DWORD lastError=0;DWORD ownerSid=2;ACL acl;ACCESS_ALLOWED_ACE ace;
std::map<std::wstring,DWORD> files;
std::wstring denied;int ownedCreateCalls=0;
DWORD GetLastError(){return lastError;}void SetLastError(DWORD e){lastError=e;}
int swprintf_s(wchar_t* p,std::size_t n,const wchar_t* f,...){va_list a;va_start(a,f);int r=vswprintf(p,n,f,a);va_end(a);return r;}
DWORD FormatMessageW(DWORD,void*,DWORD,DWORD,LPWSTR,DWORD,void*){return 0;}
void LocalFree(void*){}void CoTaskMemFree(void*){}
bool IsValidSid(PSID p){return p!=nullptr;}
bool ConvertSidToStringSidW(PSID,LPWSTR* out){static wchar_t sid[]=L"S-1-test-user";*out=sid;return true;}
HRESULT SHGetKnownFolderPath(int,DWORD,void*,LPWSTR* out){static wchar_t p[]=L"C:\\ProgramData";*out=p;return 0;}
bool FAILED(HRESULT h){return h<0;}
DWORD GetFileAttributesW(const wchar_t* p){auto i=files.find(p);if(i==files.end()){lastError=2;return INVALID_FILE_ATTRIBUTES;}return i->second;}
DWORD GetNamedSecurityInfoW(LPWSTR,DWORD,DWORD,PSID* owner,void*,PACL* out,void*,PSECURITY_DESCRIPTOR* sd){*owner=&ownerSid;*out=&acl;*sd=nullptr;return 0;}
bool CreateWellKnownSid(DWORD kind,void*,void* p,DWORD*){*static_cast<DWORD*>(p)=kind;return true;}
bool EqualSid(PSID a,PSID b){return *static_cast<DWORD*>(a)==*static_cast<DWORD*>(b);}
bool GetAce(PACL,DWORD,void** p){*p=&ace;return true;}
bool ConvertStringSecurityDescriptorToSecurityDescriptorW(const wchar_t*,DWORD,PSECURITY_DESCRIPTOR* p,void*){*p=nullptr;return true;}
bool CreateDirectoryW(const wchar_t* p,SECURITY_ATTRIBUTES*) {
 if(std::wstring(p)==L"C:\\ProgramData\\Packages\\GlassPlatform")++ownedCreateCalls;
 if(denied==p){lastError=ERROR_ACCESS_DENIED;return false;}
 if(files.count(p)){lastError=ERROR_ALREADY_EXISTS;return false;}
 files[p]=FILE_ATTRIBUTE_DIRECTORY;return true;
}
namespace GlassInstallLog {void Line(const wchar_t*){}void LineFormat(const wchar_t*,...) {}}
'''

resource_stubs = r'''
using UINT=unsigned int;using HINSTANCE=void*;using HRSRC=void*;using HGLOBAL=void*;using HANDLE=void*;
const HANDLE INVALID_HANDLE_VALUE=reinterpret_cast<HANDLE>(~std::uintptr_t(0));
constexpr DWORD ERROR_INVALID_PARAMETER=87,ERROR_RESOURCE_DATA_NOT_FOUND=1812,ERROR_CANCELLED=1223,ERROR_WRITE_FAULT=29;
constexpr DWORD CREATE_ALWAYS=2,FILE_ATTRIBUTE_NORMAL=128;
enum ResourceCase{Good,WriteFail,Cancelled,ZeroWrite,CreateFail,EmptyResource,LockFail,CloseFail};
ResourceCase resourceCase=Good;
const wchar_t* MAKEINTRESOURCEW(UINT id){return reinterpret_cast<const wchar_t*>(std::uintptr_t(id));}
HRSRC FindResourceW(HINSTANCE instance,const wchar_t*,const wchar_t*){return instance;}
DWORD SizeofResource(HINSTANCE,HRSRC){return resourceCase==EmptyResource?0:3;}
HGLOBAL LoadResource(HINSTANCE instance,HRSRC){return instance;}
void* LockResource(HGLOBAL p){return resourceCase==LockFail?nullptr:p;}
HANDLE CreateFileW(const wchar_t*,DWORD,DWORD,void*,DWORD,DWORD,void*){if(resourceCase==CreateFail){SetLastError(5);return INVALID_HANDLE_VALUE;}return reinterpret_cast<HANDLE>(1);}
bool WriteFile(HANDLE,const void*,DWORD n,DWORD* written,void*){if(resourceCase==WriteFail){SetLastError(112);return false;}*written=resourceCase==ZeroWrite?0:n;return true;}
bool CloseHandle(HANDLE){SetLastError(6);return resourceCase!=CloseFail;}
bool DeleteFileW(const wchar_t*){SetLastError(5);return false;}
'''

checks = r''' 
namespace D=GlassMaintenanceCache::Diagnostics;
void reset(){files={{L"C:\\ProgramData",FILE_ATTRIBUTE_DIRECTORY},{L"C:\\ProgramData\\Packages",FILE_ATTRIBUTE_DIRECTORY}};denied.clear();ownerSid=2;acl.AceCount=0;ace={};ownedCreateCalls=0;lastError=999;}
int main(){
 using namespace GlassMaintenanceCache;
 const std::wstring parent=L"C:\\ProgramData\\Packages",root=parent+L"\\GlassPlatform";
 std::wstring detail;
 // Existing shared parent with an ordinary user's write/create ACE. Reject
 // BEFORE CreateDirectory of our child and explain a policy refusal, not a
 // made-up CreateDirectory Win32 failure. Never rewrite the parent's ACL.
 reset();acl.AceCount=1;ace.Mask=FILE_APPEND_DATA;ace.Header.AceFlags=3;
 {D::Publication report(&detail);assert(!EnsureRoot());assert(report.context.first.domain==L"Policy");assert(report.context.first.path==parent);assert(report.context.first.operation==L"ValidateOwnerAndDacl");assert(report.context.first.reason.find(L"SID=")!=std::wstring::npos);assert(report.context.first.reason.find(L"mask=")!=std::wstring::npos);}
 assert(ownedCreateCalls==0&&detail.find(L"validate-packages-parent")!=std::wstring::npos);
 reset();ownerSid=3;
 {D::Publication report(&detail);assert(!EnsureRoot());assert(report.context.first.reason.find(L"owner=")!=std::wstring::npos);}
 assert(ownedCreateCalls==0);
 // Actual API access failure creating OUR child: distinguish it from policy.
 reset();denied=root;
 {D::Publication report(&detail);assert(!EnsureRoot());assert(report.context.first.domain==L"Win32");assert(report.context.first.operation==L"CreateDirectoryW");assert(report.context.first.path==root);assert(report.context.first.code==5);}
 assert(detail.find(L"create-owned-root")!=std::wstring::npos&&ownedCreateCalls==1);
 // Protected parent works and produces no false warning / stale last-error.
 reset();
 {D::Publication report(&detail);assert(EnsureRoot());assert(!report.context.failed);report.succeeded=true;}
 assert(detail.empty()&&files.count(root));
 // Keep original publication failure when subsequent recovery also fails.
 reset();
 {D::Publication report(&detail);D::Step(L"activate-current",root);D::Fail(L"MoveFileExW",root,32);D::Fail(L"Recovery",parent,5);assert(report.context.first.code==32);}
 assert(detail.find(L"MoveFileExW")!=std::wstring::npos&&detail.find(L"(32)")!=std::wstring::npos&&D::active==nullptr);
 // Resource I/O must preserve its real error across CloseHandle/DeleteFile.
 for(auto scenario:{WriteFail,Cancelled,ZeroWrite,CreateFail,EmptyResource,LockFail,CloseFail,Good}) {
  resourceCase=scenario;std::atomic_bool cancel{scenario==Cancelled};SetLastError(999);
  const bool ok=GlassInstallUtil::ExtractResourceToFile(reinterpret_cast<HINSTANCE>(1),1,L"RCDATA",L"mock-cache-file",&cancel);
  if(scenario==Good){assert(ok);continue;}
  assert(!ok);const DWORD expected=scenario==WriteFail?112:scenario==Cancelled?ERROR_CANCELLED:scenario==ZeroWrite?ERROR_WRITE_FAULT:scenario==CreateFail?5:scenario==CloseFail?6:ERROR_RESOURCE_DATA_NOT_FOUND;
  assert(GetLastError()==expected);
 }
 // Unknown internal failures are explicit, never reported as API success 0.
 {D::Publication report(&detail);D::Step(L"test-stage",root);}
 assert(detail.find(L"Policy=")!=std::wstring::npos&&detail.find(L"(31)")!=std::wstring::npos);
 std::cout<<"PASS: real cache preflight distinguishes parent ACL policy, owner and child API failures; first-error/stage/path/code diagnostics and resource I/O error preservation\n";
}
'''

code = stubs + diag + "\nnamespace GlassMaintenanceCache {namespace Diag=Diagnostics;\n"
for token in ("inline std::wstring Root()", "inline bool TrustedPath(",
              "inline bool TrustedDirectory(", "inline bool SecureDirectory(",
              "inline bool EnsureRoot()"):
    code += function(token) + "\n"
util = (repo / "GlassSetup/src/InstallSupport/GlassInstallUtil.cpp").read_text(encoding="utf-8")
code += "}\n" + resource_stubs + "\nnamespace GlassInstallUtil {\n"
code += function("bool ExtractResourceToFile(", util) + "\n}\n" + checks
with tempfile.TemporaryDirectory(prefix="GlassCacheDiagnostics-") as directory:
    source = Path(directory) / "test.cpp"
    exe = Path(directory) / "test"
    source.write_text(code, encoding="utf-8")
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-Wno-misleading-indentation", str(source), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
