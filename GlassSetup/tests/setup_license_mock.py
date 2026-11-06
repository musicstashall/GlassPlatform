"""Actual license identity/registry functions on fake APIs, not Windows/crypto tests."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
shared = (root/'GlassShared/src/GlassInstallerLaunch.cpp').read_text()
setup = (root/'GlassSetup/src/main.cpp').read_text()
def function(text, token):
    start = text.index(token)
    brace = text.index('{', start)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == '{': depth += 1
        elif text[i] == '}':
            depth -= 1
            if not depth: return text[start:i+1]
    raise ValueError(token)
prefix = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <map>
#include <string>
using DWORD=std::uint32_t; using BYTE=unsigned char; using HKEY=int; using HCRYPTPROV=int; using HCRYPTHASH=int;
constexpr int HKEY_CURRENT_USER=1, HKEY_LOCAL_MACHINE=2, KEY_QUERY_VALUE=1, KEY_SET_VALUE=2,
 REG_OPTION_NON_VOLATILE=0, REG_DWORD=4, RRF_RT_REG_DWORD=4, ERROR_SUCCESS=0, FALSE=0,
 PROV_RSA_AES=1, CRYPT_VERIFYCONTEXT=2, CALG_SHA_256=3, HP_HASHVAL=4,
 LANG_RUSSIAN=1, LANG_ENGLISH=2, SUBLANG_DEFAULT=1, SUBLANG_ENGLISH_US=1, IDR_EULA=1, IDR_GPL=2;
constexpr int MAKELANGID(int lang,int){return lang;}
namespace wil {struct unique_hkey {HKEY key=0; HKEY get()const{return key;} HKEY* operator&(){return &key;}};}
const wchar_t* kKeyPath=L"Installer";
std::map<std::pair<int,std::wstring>,DWORD> registry;
bool denyRead=false,denyWrite=false; int providers=0,hashes=0; bool providerFail=false,createFail=false,hashFail=false,queryFail=false;
bool OpenInstallerKey(HKEY root,int,wil::unique_hkey& key){key.key=root;return !denyRead;}
int RegGetValueW(HKEY key,void*,const wchar_t* name,int,void*,DWORD* value,DWORD*){
 auto it=registry.find({key,name}); if(denyRead||it==registry.end())return 5;*value=it->second;return 0;}
int RegCreateKeyExW(HKEY key,const wchar_t*,int,void*,int,int,void*,HKEY* out,void*){*out=key;return denyWrite?5:0;}
int RegSetValueExW(HKEY key,const wchar_t* name,int,int,const BYTE* bytes,DWORD){if(denyWrite)return 5;DWORD value;std::memcpy(&value,bytes,4);registry[{key,name}]=value;return 0;}
std::string ru="Russian terms",en="English terms",gpl="GPL terms",hashed;
std::wstring version=L"1.4.3";
std::wstring SelfProductVersion(){return version;}
struct Bytes {const BYTE* data=nullptr;DWORD size=0;};
Bytes Resource(int id,int lang=0){const auto& text=id==IDR_GPL?gpl:lang==LANG_RUSSIAN?ru:en;return {reinterpret_cast<const BYTE*>(text.data()),static_cast<DWORD>(text.size())};}
int CryptAcquireContextW(HCRYPTPROV* out,void*,void*,int,int){if(providerFail)return 0;*out=1;++providers;return 1;}
int CryptCreateHash(HCRYPTPROV,int,int,int,HCRYPTHASH* out){if(createFail)return 0;*out=1;++hashes;hashed.clear();return 1;}
int CryptHashData(HCRYPTHASH,const BYTE* data,DWORD size,int){if(hashFail)return 0;hashed.append(reinterpret_cast<const char*>(data),size);return 1;}
// Deterministic fake only: test digest use, never the SHA-256 primitive.
int CryptGetHashParam(HCRYPTHASH,int,BYTE* out,DWORD* size,int){if(queryFail)return 0;std::uint32_t h=0;for(unsigned char c:hashed)h=h*31+c;for(int i=0;i<32;++i){out[i]=h&255;h=(h>>1)^(h*33);}*size=32;return 1;}
int CryptDestroyHash(HCRYPTHASH){--hashes;return 1;}
int CryptReleaseContext(HCRYPTPROV,int){--providers;return 1;}
'''
body = '\n'.join([function(shared,'std::wstring EulaValueName('),
    function(shared,'bool IsEulaAccepted('), function(shared,'bool MarkEulaAccepted('),
    function(setup,'std::wstring SetupLicenseIdentity(')])
main = r'''
int main(){
 registry[{HKEY_LOCAL_MACHINE,L"EulaAccepted"}]=1;
 registry[{HKEY_CURRENT_USER,L"EulaAccepted"}]=1;
 auto first=SetupLicenseIdentity();assert(first.size()==version.size()+65);
 assert(!IsEulaAccepted(first.c_str()));
 assert(MarkEulaAccepted(first.c_str()));assert(IsEulaAccepted(first.c_str()));
 assert(registry.count({HKEY_LOCAL_MACHINE,EulaValueName(first.c_str())})==0);
 version=L"1.4.4";auto newer=SetupLicenseIdentity();assert(!IsEulaAccepted(newer.c_str()));
 assert(MarkEulaAccepted(newer.c_str()));assert(IsEulaAccepted(first.c_str()));
 ru+="changed";auto changed=SetupLicenseIdentity();assert(changed!=newer&&!IsEulaAccepted(changed.c_str()));
 assert(!IsEulaAccepted(nullptr)&&!MarkEulaAccepted(L""));
 assert(!MarkEulaAccepted(std::wstring(257,L'x').c_str()));
 registry[{HKEY_CURRENT_USER,EulaValueName(changed.c_str())}]=2;assert(!IsEulaAccepted(changed.c_str()));
 denyRead=true;assert(!IsEulaAccepted(first.c_str()));denyRead=false;
 denyWrite=true;assert(!MarkEulaAccepted(changed.c_str()));denyWrite=false;
 for(int i=0;i<5;++i){
  providerFail=i==0;createFail=i==1;hashFail=i==2;queryFail=i==3;
  if(i==4)en.clear();
  assert(SetupLicenseIdentity().empty());assert(providers==0&&hashes==0);
 }
 providerFail=createFail=hashFail=queryFail=false;en="terms";version.clear();assert(SetupLicenseIdentity().empty());
}
'''
# Route checks complement the executable registry tests; no claim of GUI execution.
assert 'IsEulaAccepted()' not in setup and 'MarkEulaAccepted()' not in setup
assert 'MarkEulaAccepted' not in function(setup, 'void ContinueAfterLicenseCheck(')
assert 'ContinueAfterLicenseCheck();' in function(setup, 'void CompleteVersionCheck(')
assert 'ContinueAfterLicenseCheck();' in function(setup, 'void ContinueVersionCheck(')
assert 'MarkEulaAccepted' not in function(shared, 'bool TryConsumeMaintenanceTicket(')
with tempfile.TemporaryDirectory() as directory:
    source = Path(directory)/'license.cpp'
    source.write_text(prefix+'\n'+body+'\n'+main)
    binary = Path(directory)/'license'
    subprocess.run(['g++','-std=c++17','-fsanitize=address,undefined','-g',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: version/terms-scoped user consent, historical identities, legacy/ticket rejection, failures and handle cleanup (fake APIs)')
