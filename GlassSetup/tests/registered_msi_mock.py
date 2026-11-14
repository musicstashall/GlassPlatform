#!/usr/bin/env python3
"""Run actual shared Recheck control flow; Hash/MSI APIs are mocked, no Windows/MSI operations."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassRegisteredMsi.h').read_text()
a=s.index('inline bool Recheck(');method=s[a:s.rfind('\n}')]
prefix=r'''
#include <array>
#include <atomic>
#include <string>
#include <cassert>
#include <cwchar>
#include <iostream>
using BYTE=unsigned char;
struct Fake {bool context=true,present=true,copy=true,paths=true,hash=true,same=true;int copies=0,hashes=0,deletes=0,roots=0;} fake;
namespace GlassInstallMsi {
 enum class ProductPathCheck{Safe,Unverifiable};
 ProductPathCheck CheckProductContexts(const wchar_t*,std::wstring&,const std::atomic_bool*){return fake.context?ProductPathCheck::Safe:ProductPathCheck::Unverifiable;}
 bool IsProductInstalled(const wchar_t*){return fake.present;}
 bool CopyLocalPackage(const wchar_t*,const std::wstring&,const std::atomic_bool*){++fake.copies;return fake.copy;}
 ProductPathCheck CheckProductFileRoots(const wchar_t*,const std::wstring&,const std::wstring&,std::wstring&,const std::atomic_bool*){++fake.roots;return fake.paths?ProductPathCheck::Safe:ProductPathCheck::Unverifiable;}
}
bool Hash(const std::wstring&,std::array<BYTE,32>& digest,const std::atomic_bool*){++fake.hashes;digest={};if(!fake.same&&fake.hashes==2)digest[0]=1;return fake.hash;}
void DeleteFileW(const wchar_t*){++fake.deletes;}
'''
main=r'''
int main(){
 const auto code=L"{11111111-2222-3333-4444-555555555555}";
 std::atomic_bool cancel{false};std::wstring detail;
 auto check=[&](bool expected=true,const wchar_t* baseline=L"snapshot.msi") {return Recheck(code,L"root",expected,baseline,L"protected",detail,&cancel);};
 for(unsigned test=0;test<8;++test) {
  fake={};cancel=false;
  if(test==1)fake.context=false;if(test==2)fake.present=false;if(test==3)fake.copy=false;
  if(test==4)fake.paths=false;if(test==5)fake.hash=false;if(test==6)fake.same=false;if(test==7)cancel=true;
  assert(check()==(test==0));
  if(test==0)assert(fake.roots==1&&fake.hashes==2);
  if(test==1||test==2||test==7)assert(fake.copies==0);
  if(test==3||test==4||test==5||test==6)assert(fake.deletes==1);
  if(test==5||test==6)assert(fake.roots==0);
 }
 fake={};cancel=false;fake.present=false;assert(check(false,L""));assert(!fake.copies&&!fake.hashes);
 fake={};assert(!check(true,L""));assert(!fake.copies);
 fake={};assert(!Recheck(L"../../evil",L"root",true,L"snapshot",L"protected",detail,&cancel));assert(!fake.copies);
 std::cout<<"PASS: shared registered MSI guard: context/presence, exact cached-byte baseline, hash/read/path errors, cancellation, absence, unsafe identity and current-file cleanup\n";
}
'''
with tempfile.TemporaryDirectory(prefix='glass-registered-msi-') as d:
 cpp=Path(d)/'test.cpp';exe=Path(d)/'test';cpp.write_text(prefix+method+main)
 subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',str(cpp),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
assert 'GlassRegisteredMsi::Recheck' in (root/'GlassSetup/src/InstallEngine.cpp').read_text()
assert 'GlassRegisteredMsi::Recheck' in (root/'GlassSetup/GlassUnins/main.cpp').read_text()
