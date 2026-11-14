"""Exercise the production publisher gate; WinTrust/SIP cryptography is a Windows seam.
Only an exact Setup signer and success/untrusted-root status are accepted. The
mock cannot establish which status Windows returns for an actual signed MSI.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'GlassSetup/src/InstallSupport/GlassPackageAcquisition.cpp').read_text()
function = source[source.index('bool SameMsiPublisher('):source.index('bool CopyLocked(')]
prefix = r'''
#include <cassert>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <string>
#include <iostream>
using LONG=std::int32_t;using DWORD=std::uint32_t;using BYTE=unsigned char;
using PCCERT_CONTEXT=void*;
#define _wcsicmp wcscasecmp
constexpr int FALSE=0,ERROR_SUCCESS=0,WTD_REVOKE_NONE=0,WTD_UI_NONE=0,WTD_CHOICE_FILE=1,
 WTD_STATEACTION_VERIFY=1,WTD_STATEACTION_CLOSE=2,WTD_CACHE_ONLY_URL_RETRIEVAL=4,
 WTD_REVOCATION_CHECK_NONE=8,CERT_SHA1_HASH_PROP_ID=3;
constexpr LONG CERT_E_UNTRUSTEDROOT=static_cast<LONG>(0x800B0109u);
struct GUID{};GUID WINTRUST_ACTION_GENERIC_VERIFY_V2;
struct WINTRUST_FILE_INFO{DWORD cbStruct=0;const wchar_t* pcwszFilePath=nullptr;};
struct WINTRUST_DATA{DWORD cbStruct=0;int dwUIChoice=0,fdwRevocationChecks=0,dwUnionChoice=0;
 WINTRUST_FILE_INFO* pFile=nullptr;int dwStateAction=0,dwProvFlags=0;void* hWVTStateData=nullptr;};
struct Cert{PCCERT_CONTEXT pCert;};
struct Signer{unsigned csCertChain;Cert* pasCertChain;};
struct Provider{};
struct State{LONG status=0;int opens=0,closes=0,reads=0;bool provider=true,signer=true,cert=true,hash=true;
 DWORD bytes=20;std::wstring expected=std::wstring(40,L'A');} s;
Cert cert{reinterpret_cast<void*>(1)};Signer signer{1,&cert};Provider provider;
LONG WinVerifyTrust(void*,GUID*,WINTRUST_DATA* t){
 if(t->dwStateAction==WTD_STATEACTION_CLOSE){++s.closes;t->hWVTStateData=nullptr;return 0;}
 ++s.opens;t->hWVTStateData=&provider;assert(t->dwUIChoice==WTD_UI_NONE);
 assert(t->dwProvFlags==(WTD_CACHE_ONLY_URL_RETRIEVAL|WTD_REVOCATION_CHECK_NONE));
 return s.status;
}
Provider* WTHelperProvDataFromStateData(void* state){assert(state);++s.reads;return s.provider?&provider:nullptr;}
Signer* WTHelperGetProvSignerFromChain(Provider*,int,int,int){
 signer.csCertChain=s.cert?1:0;return s.signer?&signer:nullptr;
}
bool CertGetCertificateContextProperty(PCCERT_CONTEXT,int,BYTE* out,DWORD* bytes){
 std::memset(out,0xAA,20);*bytes=s.bytes;return s.hash;
}
namespace GlassProductInspect{std::wstring QuerySelfSignerSha1(){return s.expected;}}
namespace InstallLog{template<class... T>void LineFormat(const wchar_t*,T...){} }
'''
main = r'''
void check(bool expected){assert(SameMsiPublisher(L"C:\\kit\\Glass-x64.msi")==expected);assert(s.opens==s.closes);}
int main(){
 s={};check(true);
 s={};s.status=CERT_E_UNTRUSTEDROOT;check(true); // Fresh PC, exact pinned publisher.
 s={};s.expected=std::wstring(40,L'B');check(false);
 s={};s.status=CERT_E_UNTRUSTEDROOT;s.expected=std::wstring(40,L'B');check(false);
 s={};s.expected.clear();check(false);assert(s.opens==0);
 // Digest, no signature, expired, wrong usage, missing chain and generic failure
 // remain fatal, even if the provider exposes the expected leaf certificate.
 for(std::uint32_t status:{0x80096010u,0x800B0100u,0x800B0101u,0x800B0110u,0x800B010Au,0x800B0004u}){
  s={};s.status=static_cast<LONG>(status);check(false);assert(s.reads==0);
 }
 s={};s.provider=false;check(false);
 s={};s.signer=false;check(false);
 s={};s.cert=false;check(false);
 s={};s.hash=false;check(false);
 s={};s.bytes=19;check(false);
 std::cout<<"PASS: actual MSI publisher gate: exact signer, fresh-PC root policy, fail-closed statuses, state cleanup\n";
}
'''
with tempfile.TemporaryDirectory(prefix='GlassPublisher-') as directory:
    path = Path(directory) / 'test.cpp'
    exe = Path(directory) / 'test'
    path.write_text(prefix + function + main)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-no-pie', str(path), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
