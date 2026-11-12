"""Actual streamed verifier with fake CryptoAPI/file APIs; no Windows execution."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassReleaseClient.cpp').read_text()
body=s[s.index('bool VerifyHash('):s.rfind('\n}')]
prefix=r'''
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
using HANDLE=int;using DWORD=std::uint32_t;using BYTE=unsigned char;using HCRYPTPROV=int;using HCRYPTHASH=int;using BOOL=int;
constexpr BOOL FALSE=0;constexpr DWORD FILE_ATTRIBUTE_DIRECTORY=16,FILE_ATTRIBUTE_REPARSE_POINT=32,ERROR_INVALID_DATA=13,ERROR_CRC=23,
 ERROR_SUCCESS=0,FILE_BEGIN=0,PROV_RSA_AES=24,CRYPT_VERIFYCONTEXT=1,CALG_SHA_256=2,HP_HASHVAL=3;
struct LARGE_INTEGER{long long QuadPart=0;};struct BY_HANDLE_FILE_INFORMATION{DWORD dwFileAttributes=0,nNumberOfLinks=1;};
struct State{std::string bytes="abc",hashed;std::size_t pos=0;DWORD attributes=0,links=1,error=0;bool readFail=false,hashFail=false,queryFail=false,providerFail=false;int providers=0,hashes=0;} f;
DWORD GetLastError(){return f.error;}
BOOL GetFileInformationByHandle(HANDLE,BY_HANDLE_FILE_INFORMATION* out){out->dwFileAttributes=f.attributes;out->nNumberOfLinks=f.links;return 1;}
BOOL GetFileSizeEx(HANDLE,LARGE_INTEGER* out){out->QuadPart=f.bytes.size();return 1;}
BOOL SetFilePointerEx(HANDLE,LARGE_INTEGER,void*,DWORD){f.pos=0;return 1;}
BOOL ReadFile(HANDLE,void* out,DWORD count,DWORD* read,void*){if(f.readFail){f.error=111;return 0;}*read=std::min<std::size_t>(count,f.bytes.size()-f.pos);std::memcpy(out,f.bytes.data()+f.pos,*read);f.pos+=*read;return 1;}
BOOL CryptAcquireContextW(HCRYPTPROV* out,void*,void*,DWORD,DWORD){if(f.providerFail){f.error=222;return 0;}++f.providers;*out=1;return 1;}
BOOL CryptCreateHash(HCRYPTPROV,DWORD,int,DWORD,HCRYPTHASH* out){++f.hashes;*out=1;return 1;}
BOOL CryptHashData(HCRYPTHASH,const BYTE* data,DWORD count,DWORD){if(f.hashFail){f.error=333;return 0;}f.hashed.append(reinterpret_cast<const char*>(data),count);return 1;}
BOOL CryptGetHashParam(HCRYPTHASH,DWORD,BYTE* out,DWORD* count,DWORD){if(f.queryFail){f.error=444;return 0;}for(unsigned i=0;i<32;++i)out[i]=static_cast<BYTE>(i);*count=32;return 1;}
BOOL CryptDestroyHash(HCRYPTHASH){--f.hashes;f.error=999;return 1;}BOOL CryptReleaseContext(HCRYPTPROV,DWORD){--f.providers;f.error=999;return 1;}
bool Fail(std::wstring& error,const wchar_t* text,DWORD code=GetLastError()){error=std::wstring(text)+L" "+std::to_wstring(code);return false;}
bool Cancelled(const std::atomic<bool>& cancel,std::wstring& error){if(!cancel.load())return false;return !Fail(error,L"Cancelled",1223);}
'''
main=r'''
void reset(){assert(f.providers==0 && f.hashes==0);f=State{};}
int main(){
 const std::string expected="000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
 std::atomic<bool> cancel{false};std::wstring error;
 assert(VerifyHash(1,3,expected,cancel,error));assert(f.hashed=="abc"&&f.pos==0);
 reset();f.bytes=std::string(100000,'x');assert(VerifyHash(1,f.bytes.size(),expected,cancel,error));assert(f.hashed==f.bytes);
 reset();assert(!VerifyHash(1,4,expected,cancel,error));assert(f.hashed.empty());
 reset();f.attributes=FILE_ATTRIBUTE_DIRECTORY;assert(!VerifyHash(1,3,expected,cancel,error));
 reset();f.attributes=FILE_ATTRIBUTE_REPARSE_POINT;assert(!VerifyHash(1,3,expected,cancel,error));
 reset();f.links=2;assert(!VerifyHash(1,3,expected,cancel,error));
 reset();assert(!VerifyHash(1,3,std::string(64,'f'),cancel,error));assert(error.find(L"23")!=std::wstring::npos);assert(error.find(L"expected="+std::wstring(64,L'f'))!=std::wstring::npos);assert(error.find(L"actual="+std::wstring(expected.begin(),expected.end()))!=std::wstring::npos);
 reset();f.readFail=true;assert(!VerifyHash(1,3,expected,cancel,error));assert(error.find(L"111")!=std::wstring::npos);
 reset();f.hashFail=true;assert(!VerifyHash(1,3,expected,cancel,error));assert(error.find(L"333")!=std::wstring::npos);
 reset();f.queryFail=true;assert(!VerifyHash(1,3,expected,cancel,error));assert(error.find(L"444")!=std::wstring::npos);
 reset();f.providerFail=true;assert(!VerifyHash(1,3,expected,cancel,error));assert(error.find(L"222")!=std::wstring::npos);
 reset();cancel=true;assert(!VerifyHash(1,3,expected,cancel,error));assert(f.hashed.empty());
 reset();std::cout<<"PASS: streamed verifier, size/type/link/hash/cancellation guards and error preservation\n";
}
'''
with tempfile.TemporaryDirectory(prefix='GlassReleaseHash-') as temp:
    path=Path(temp)/'test.cpp';exe=Path(temp)/'test';path.write_text(prefix+body+main)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-no-pie',str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
