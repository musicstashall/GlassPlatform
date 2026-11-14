"""Actual PE certificate-table gate and Authenticode decision with fake Win32 APIs."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassOpenGlassTrust.h').read_text()
functions=s[s.index('inline bool HasNoEmbeddedSignature('):s.rindex('\n}')]
prefix=r'''
#include <array>
#include <string>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <cassert>
#include <cstdint>
using HANDLE=int;using LONGLONG=long long;using LONG=int;using DWORD=unsigned;using WORD=unsigned short;
struct LARGE_INTEGER{LONGLONG QuadPart;};
struct IMAGE_DOS_HEADER{WORD e_magic;LONG e_lfanew;};
struct IMAGE_FILE_HEADER{WORD SizeOfOptionalHeader;};
struct IMAGE_DATA_DIRECTORY{DWORD VirtualAddress,Size;};
struct IMAGE_OPTIONAL_HEADER32{WORD Magic;DWORD NumberOfRvaAndSizes;IMAGE_DATA_DIRECTORY DataDirectory[16];};
struct IMAGE_OPTIONAL_HEADER64{WORD Magic;DWORD NumberOfRvaAndSizes;IMAGE_DATA_DIRECTORY DataDirectory[16];};
constexpr unsigned IMAGE_DOS_SIGNATURE=0x5a4d,IMAGE_NT_SIGNATURE=0x4550,IMAGE_NT_OPTIONAL_HDR32_MAGIC=0x10b,IMAGE_NT_OPTIONAL_HDR64_MAGIC=0x20b,IMAGE_DIRECTORY_ENTRY_SECURITY=4,FILE_BEGIN=0;
std::vector<unsigned char> bytes;unsigned cursor=0;bool readDenied=false;
bool SetFilePointerEx(HANDLE,LARGE_INTEGER p,void*,unsigned){if(p.QuadPart<0)return false;cursor=p.QuadPart;return true;}
bool ReadFile(HANDLE,void* out,unsigned size,unsigned* got,void*){if(readDenied||cursor+size>bytes.size())return false;std::memcpy(out,bytes.data()+cursor,size);*got=size;return true;}
struct WINTRUST_FILE_INFO{unsigned cbStruct;const wchar_t* pcwszFilePath;HANDLE hFile;};
struct WINTRUST_DATA{unsigned cbStruct,dwUIChoice,fdwRevocationChecks,dwUnionChoice;WINTRUST_FILE_INFO* pFile;unsigned dwStateAction,dwProvFlags;void* hWVTStateData;};
constexpr unsigned WTD_UI_NONE=0,WTD_REVOKE_NONE=0,WTD_CHOICE_FILE=1,WTD_STATEACTION_VERIFY=1,WTD_STATEACTION_CLOSE=2,WTD_CACHE_ONLY_URL_RETRIEVAL=4,WTD_REVOCATION_CHECK_NONE=8,ERROR_SUCCESS=0;
struct GUID{int x;};const GUID WINTRUST_ACTION_GENERIC_VERIFY_V2{0};
unsigned long trustStatus=0;unsigned closes=0;
unsigned long WinVerifyTrust(void*,GUID*,WINTRUST_DATA* t){if(t->dwStateAction==WTD_STATEACTION_CLOSE){++closes;return 0;}return trustStatus;}
struct Cert{unsigned char* pbCertEncoded=nullptr;unsigned cbCertEncoded=0;};Cert cert;
struct Chain{Cert* pCert=&cert;};Chain chain;
struct Signer{unsigned csCertChain=1;Chain* pasCertChain=&chain;};Signer signer;
void* WTHelperProvDataFromStateData(void*){return &signer;}
Signer* WTHelperGetProvSignerFromChain(void*,int,int,int){return &signer;}
constexpr bool FALSE=false;bool publisherOk=true;
constexpr const char *szOID_COMMON_NAME="cn",*szOID_ORGANIZATION_NAME="org",*szOID_LOCALITY_NAME="city",*szOID_STATE_OR_PROVINCE_NAME="state",*szOID_COUNTRY_NAME="country";
std::wstring Attribute(Cert*,const char* oid){if(!publisherOk)return L"other";if(oid==szOID_COMMON_NAME)return L"Open Source Developer, Zhe Yang";if(oid==szOID_ORGANIZATION_NAME)return L"Open Source Developer";if(oid==szOID_LOCALITY_NAME)return L"Guangzhou";if(oid==szOID_STATE_OR_PROVINCE_NAME)return L"Guangdong";return L"CN";}
std::array<unsigned char,32> Hash(unsigned char*,unsigned){std::array<unsigned char,32> v{};v[0]=1;return v;}
void Require(bool ok,const char* error){if(!ok)throw std::runtime_error(error);}
'''
main=r'''
template<class T>void put(unsigned offset,const T& value){if(bytes.size()<offset+sizeof(T))bytes.resize(offset+sizeof(T));std::memcpy(bytes.data()+offset,&value,sizeof(T));}
void pe(bool x64=false,unsigned certOffset=0,unsigned certSize=0){bytes.clear();readDenied=false;put(0,IMAGE_DOS_HEADER{IMAGE_DOS_SIGNATURE,64});put(64,DWORD(IMAGE_NT_SIGNATURE));
 IMAGE_FILE_HEADER file{sizeof(IMAGE_OPTIONAL_HEADER32)};put(68,file);IMAGE_OPTIONAL_HEADER32 optional{};optional.Magic=x64?IMAGE_NT_OPTIONAL_HDR64_MAGIC:IMAGE_NT_OPTIONAL_HDR32_MAGIC;optional.NumberOfRvaAndSizes=16;optional.DataDirectory[4]={certOffset,certSize};put(68+sizeof(file),optional);}
bool accepted(bool fallback){try{(void)VerifyPublisher(L"exe",1,fallback);return true;}catch(const std::exception&){return false;}}
int main(){
 for(bool x64:{false,true}){pe(x64);assert(HasNoEmbeddedSignature(1));trustStatus=0x800B0100UL;assert(!accepted(false)&&accepted(true));
  pe(x64,200,32);assert(!HasNoEmbeddedSignature(1)&&!accepted(true));
  pe(x64,0,32);assert(!HasNoEmbeddedSignature(1)&&!accepted(true));}
 pe();readDenied=true;assert(!accepted(true));readDenied=false;bytes.resize(2);assert(!accepted(true));
 pe();bytes[0]=0;assert(!accepted(true));pe();
 for(auto status:{0x80096010UL,0x800B0109UL,0x800B0101UL}){trustStatus=status;assert(!accepted(true)&&!accepted(false));}
 trustStatus=0;publisherOk=false;assert(!accepted(true));publisherOk=true;assert(accepted(false)&&accepted(true));assert(closes>0);
}
'''
uninstall=(root/'GlassSetup/src/InstallSupport/GlassOpenGlassUninstall.cpp').read_text()
assert uninstall.index('TrustedDirectory(record.directory)')<uninstall.index('VerifyPublisher(record.uninstaller,executable.h,true)')
for p in (root/'GlassSetup/src').rglob('*'):
 if p.suffix not in ('.cpp','.h'):continue
 text=p.read_text()
 if p.name!='GlassOpenGlassUninstall.cpp':assert 'VerifyPublisher(record.uninstaller,executable.h,true)' not in text
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(prefix+functions+main);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: actual PE/Authenticode gate: protected unsigned opt-in only; x86/x64, present/malformed certificate table, malformed PE, read denial, bad signature/chain/expiry, publisher mismatch, strict default and trust-state cleanup')
