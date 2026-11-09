"""Actual local-SAM deletion classifier; no Windows account/registry changes."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassProfileMaintenance.h').read_text();a=s.index('inline bool DeletedLocalProfile(');b=s.index('\nstruct Profile {',a)
h=r'''
#include <cassert>
#include <string>
#include <cstring>
#include <iostream>
using BYTE=unsigned char;using DWORD=unsigned;
struct SID_IDENTIFIER_AUTHORITY {BYTE Value[6];};
struct Sid {SID_IDENTIFIER_AUTHORITY auth{{0,0,0,0,0,5}};BYTE count=4;DWORD sub[5]{21,11,22,33,1001};};
using PSID=Sid*;Sid account,domain;
using SID_NAME_USE=int;using LSA_HANDLE=void*;
struct LSA_OBJECT_ATTRIBUTES {unsigned Length;};
struct POLICY_ACCOUNT_DOMAIN_INFO {PSID DomainSid;};using PPOLICY_ACCOUNT_DOMAIN_INFO=POLICY_ACCOUNT_DOMAIN_INFO*;
constexpr int POLICY_VIEW_LOCAL_INFORMATION=1,PolicyAccountDomainInformation=1,ERROR_SUCCESS=0,ERROR_NONE_MAPPED=1332;
bool parse=true,policyOk=true,queryOk=true,valid=true,mapped=false;int lookupError=1332,error=0,freed=0,lookups=0;
int ConvertStringSidToSidW(const wchar_t*,PSID* out){*out=&account;return parse;}
void LocalFree(PSID){++freed;}
int LsaOpenPolicy(void*,LSA_OBJECT_ATTRIBUTES*,int,LSA_HANDLE* out){*out=reinterpret_cast<void*>(1);return policyOk?0:5;}
POLICY_ACCOUNT_DOMAIN_INFO info{&domain};
int LsaQueryInformationPolicy(LSA_HANDLE,int,void** out){*out=&info;return queryOk?0:5;}
void LsaClose(LSA_HANDLE){}void LsaFreeMemory(PPOLICY_ACCOUNT_DOMAIN_INFO){}
bool IsValidSid(PSID){return valid;}
BYTE* GetSidSubAuthorityCount(PSID s){return &s->count;}
SID_IDENTIFIER_AUTHORITY* GetSidIdentifierAuthority(PSID s){return &s->auth;}
DWORD* GetSidSubAuthority(PSID s,BYTE i){return &s->sub[i];}
void SetLastError(int v){error=v;}int GetLastError(){return error;}
bool LookupAccountSidW(void*,PSID,void*,DWORD*,void*,DWORD*,SID_NAME_USE*){++lookups;error=lookupError;return mapped;}
// METHOD
void reset(){account=Sid{};account.count=5;domain=Sid{};parse=policyOk=queryOk=valid=true;mapped=false;lookupError=1332;freed=lookups=0;}
int main(){
 reset();assert(DeletedLocalProfile(L"local-deleted"));assert(lookups==1&&freed==1);
 for(int fail=0;fail<8;++fail){reset();
  if(fail==0)parse=false;if(fail==1)policyOk=false;if(fail==2)queryOk=false;if(fail==3)valid=false;
  if(fail==4)account.sub[2]=999; // Foreign domain: unreachable is not deleted.
  if(fail==5)account.count=4;if(fail==6)mapped=true;if(fail==7)lookupError=1355;
  assert(!DeletedLocalProfile(L"not-proven-deleted"));if(fail==4)assert(lookups==0);
 }
 std::cout<<"PASS: actual deleted-local classifier: local SAM SID + NONE_MAPPED only; existing/foreign/domain-unavailable/unknown preserved\n";
}
'''.replace('// METHOD',s[a:b])
with tempfile.TemporaryDirectory(prefix='DeletedLocal-') as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(h)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
