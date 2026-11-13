"""Actual same-version Native offer comparison; no MSI execution or mutations."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassProductInspect.cpp').read_text()
a=s.index('void CompareNativeWithOffer(');b=s.index('// Windows Installer strips cached signatures',a)
h=r'''
#include <cassert>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include <iostream>
using UINT=unsigned;using DWORD=unsigned;using BYTE=unsigned char;using MSIHANDLE=unsigned;
#define _wcsicmp wcscasecmp
constexpr UINT ERROR_SUCCESS=0,MSIDBOPEN_READONLY=0;
struct MsiFileHashBuffer {DWORD dwFileHashInfoSize;DWORD words[4];};
struct FileCheck {std::wstring fileName;DWORD hash[4]{1,2,3,4};bool hasHash=true;};
std::wstring authored;
std::wstring QueryMsiProperty(MSIHANDLE,const wchar_t*){return authored;}
bool openOk=true,enumOk=true,hashOk=true;int closes=0,hashCalls=0;DWORD actual[4]{1,2,3,4};std::vector<FileCheck> metadata;
UINT MsiOpenDatabaseW(const wchar_t*,UINT,MSIHANDLE* out){*out=1;return openOk?0:5;}
bool EnumProductFiles(MSIHANDLE,std::vector<FileCheck>& out){out=metadata;return enumOk;}
void MsiCloseHandle(MSIHANDLE){++closes;}
UINT MsiGetFileHashW(const wchar_t* path,int,MsiFileHashBuffer* out){++hashCalls;assert(std::wstring(path)==L"installed.dll"&&out->dwFileHashInfoSize==sizeof(*out));memcpy(out->words,actual,sizeof(actual));return hashOk?0:5;}
namespace GlassInstallLog {template<class... T>void LineFormat(const wchar_t*,T...) {}}
// SOURCE
int main(){bool compared=true,differs=true;
 metadata={{L"GlassSettings.Native.dll"}};CompareNativeWithOffer(L"verified.msi",L"installed.dll",compared,differs);assert(compared&&!differs&&closes==1);
 actual[0]=99;CompareNativeWithOffer(L"verified.msi",L"installed.dll",compared,differs);assert(compared&&differs);actual[0]=1;
 for(int fail=0;fail<6;fail++){
  openOk=enumOk=hashOk=true;metadata={{L"GlassSettings.Native.dll"}};compared=differs=true;hashCalls=closes=0;
  if(fail==0)openOk=false;if(fail==1)enumOk=false;if(fail==2)metadata[0].fileName=L"other.dll";
  if(fail==3)metadata[0].hasHash=false;if(fail==4)metadata.push_back(metadata[0]);if(fail==5)hashOk=false;
  CompareNativeWithOffer(L"verified.msi",L"installed.dll",compared,differs);assert(!compared&&!differs);assert(closes==(fail?1:0));assert(hashCalls==(fail==5?1:0));
 }
 openOk=hashOk=true;enumOk=false;metadata.clear();authored=L"00000001000000020000000300000004";
 CompareNativeWithOffer(L"verified.msi",L"installed.dll",compared,differs);assert(compared&&!differs);
 actual[3]=9;CompareNativeWithOffer(L"verified.msi",L"installed.dll",compared,differs);assert(compared&&differs);actual[3]=4;
 for(const auto* bad:{L"123",L"0000000100000002000000030000000Z",L"000000010000000200000003000000040"}) {
  authored=bad;int before=hashCalls;CompareNativeWithOffer(L"verified.msi",L"installed.dll",compared,differs);assert(!compared&&!differs&&hashCalls==before);
 }
 std::cout<<"PASS: actual Native offer comparison: equal/different bytes independently of ProductVersion; MSI hash buffer initialization, missing/ambiguous metadata, unreadable hash, cleanup, no false equality\n";
}
'''.replace('// SOURCE',s[a:b])
assert 'job.rep->sourceVerified&&!offerPath.empty()' in s
harvest=(root/'GlassSetup/msi/scripts/Generate-HarvestWxs.ps1').read_text()
assert 'GLASSNATIVEMSIHASH' in harvest and 'MsiGetFileHashW($nativeFiles[0].FullName' in harvest
assert '$hash.Size = 20' in harvest and '$rc -ne 0' in harvest and '$nativeFiles.Count -gt 1' in harvest
assert harvest.index('GLASSNATIVEMSIHASH') < harvest.index('<ComponentGroup Id=')
# PowerShell/Win32/WiX runtime remains a Windows acceptance check.
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
