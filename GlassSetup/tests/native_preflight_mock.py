#!/usr/bin/env python3
"""Actual installed-helper publisher verifier with fake WinTrust (not Windows trust validation)."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassInstallUtil.cpp').read_text()
a=s.index('bool SignedPublisher('); b=s.index('// Protect against renaming',a)
method=s[a:b]
prefix=r'''
#include <array>
#include <string>
#include <cassert>
#include <cstdint>
#include <iostream>
using BYTE=unsigned char;using DWORD=unsigned;using UINT=unsigned;using LONG=std::int32_t;
constexpr LONG ERROR_SUCCESS=0,CERT_E_UNTRUSTEDROOT=-1,CERT_E_CHAINING=-2;
constexpr int WTD_UI_NONE=0,WTD_REVOKE_NONE=0,WTD_CHOICE_FILE=1,WTD_STATEACTION_VERIFY=1,WTD_STATEACTION_CLOSE=2,WTD_CACHE_ONLY_URL_RETRIEVAL=4;
constexpr bool FALSE=false;
constexpr auto BCRYPT_SHA256_ALGORITHM=L"SHA256";
struct GUID{};const GUID WINTRUST_ACTION_GENERIC_VERIFY_V2{};
struct WINTRUST_FILE_INFO {unsigned cbStruct;const wchar_t* pcwszFilePath=nullptr;};
struct WINTRUST_DATA {unsigned cbStruct;unsigned dwUIChoice=0,fdwRevocationChecks=0,dwUnionChoice=0;WINTRUST_FILE_INFO* pFile=nullptr;unsigned dwStateAction=0,dwProvFlags=0;void* hWVTStateData=nullptr;};
struct Cert {BYTE* pbCertEncoded=nullptr;unsigned cbCertEncoded=0;};
struct Chain {Cert* pCert=nullptr;};struct Signer {unsigned csCertChain=1;Chain* pasCertChain=nullptr;};
Cert cert;Chain chain{&cert};Signer signer{1,&chain};
struct Fake {LONG status=0;bool provider=true,signer=true,hash=true;unsigned bytes=32;int closes=0;};Fake fake;
LONG WinVerifyTrust(void*,GUID*,WINTRUST_DATA* d){
 if(d->dwStateAction==WTD_STATEACTION_CLOSE){++fake.closes;return 0;}
 assert(d->dwProvFlags==WTD_CACHE_ONLY_URL_RETRIEVAL && d->dwUIChoice==WTD_UI_NONE);
 d->hWVTStateData=&fake;return fake.status;
}
void* WTHelperProvDataFromStateData(void* p){return fake.provider?p:nullptr;}
Signer* WTHelperGetProvSignerFromChain(void*,int,bool,int){return fake.signer?&signer:nullptr;}
bool CryptHashCertificate2(const wchar_t*,int,void*,BYTE*,unsigned,BYTE* out,DWORD* size){
 *size=fake.bytes;for(unsigned i=0;i<32;++i)out[i]=static_cast<BYTE>(i);return fake.hash;
}
'''
main=r'''
int main(){
 for(LONG status:{0,-1,-2,-3,-4,-5,-6,-7,-8}) {
  fake={};fake.status=status;std::array<BYTE,32> hash{};
  assert(SignedPublisher(L"installed.dll",hash)==(status>=-2));assert(fake.closes==1);
 }
 for(int missing=0;missing<6;++missing){
  fake={};chain.pCert=&cert;signer.csCertChain=1;signer.pasCertChain=&chain;
  if(missing==0)fake.provider=false;if(missing==1)fake.signer=false;
  if(missing==2)signer.csCertChain=0;if(missing==3)chain.pCert=nullptr;
  if(missing==4)fake.hash=false;if(missing==5)fake.bytes=20;
  std::array<BYTE,32> hash{};assert(!SignedPublisher(L"installed.dll",hash));assert(fake.closes==1);
 }
 std::cout<<"PASS: actual helper signature verifier: valid/pinned-chain statuses, other trust failures, missing provider/chain, hash errors and unconditional trust-state closure\n";
}
'''
with tempfile.TemporaryDirectory(prefix='glass-installed-native-') as d:
 cpp=Path(d)/'test.cpp';exe=Path(d)/'test';cpp.write_text(prefix+method+main)
 subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(cpp),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
assert 'SignedPublisher(self,expected) && SignedPublisher(path,actual) && expected==actual' in s
assert 'TrustedDirectory(parent)' in s and 'TrustedPath(path,false)' in s
assert 'FILE_SHARE_READ,nullptr,OPEN_EXISTING' in s
