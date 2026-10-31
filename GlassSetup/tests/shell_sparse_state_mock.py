#!/usr/bin/env python3
"""Exercise the actual installer sparse-package state/removal code with fake WinRT."""
from pathlib import Path
import os,subprocess,tempfile
repo=Path(__file__).resolve().parents[2]
src=(repo/'GlassSettings/Native/ShellSparsePackage.cpp').read_text(encoding='utf-8')
def function(token):
    start=src.index(token);brace=src.index('{',start);depth=0
    for i in range(brace,len(src)):
        if src[i]=='{':depth+=1
        if src[i]=='}':
            depth-=1
            if not depth:return src[start:i+1]
    raise ValueError(token)
query=function('InstallerSparseState QueryInstallerSparseState(')
remove=function('bool UnregisterInstallerSparsePackageQuiet(')
prefix=r'''
#include <algorithm>
#include <cassert>
#include <cwchar>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
constexpr int MAX_PATH=260;
#define _countof(a) (sizeof(a)/sizeof(a[0]))
#define _wcsicmp wcscasecmp
template<size_t N>int wcscpy_s(wchar_t (&out)[N],const wchar_t* value){
 if(!value || wcslen(value)>=N)return 1;wcscpy(out,value);return 0;}
namespace GlassShared::GlassShellMenu{
 constexpr wchar_t kSparsePackageFamilyName[]=L"GlassProject.GlassShellMenu_t6mpd150h6a5c";
 constexpr wchar_t kSparsePackageName[]=L"GlassProject.GlassShellMenu";
}
namespace winrt {using hstring=std::wstring;}
struct FakePackage{std::wstring name=L"GlassProject.GlassShellMenu",full=L"package_v1",root=L"D:\\Glass";bool healthy=true;};
struct Fake{std::vector<FakePackage> current,global;bool apartment=true,throwCurrent=false,throwGlobal=false,removeOk=true;
 int removals=0;std::wstring family,removed;} f;
namespace winmd {
 enum class RemovalOptions{None};
 struct PackageId{FakePackage p;std::wstring Name()const{return p.name;}std::wstring FullName()const{return p.full;}};
 struct PackageStatus{bool ok;bool VerifyIsOK()const{return ok;}};
 struct Package{FakePackage p;PackageId Id()const{return {p};}PackageStatus Status()const{return {p.healthy};}
  std::wstring EffectiveExternalPath()const{return p.root;}};
 struct Result{bool ok;};struct Op{Result get(){return {f.removeOk};}};
 struct PackageManager{
  std::vector<Package> FindPackagesForUser(const wchar_t* user,const wchar_t* family){
   assert(!*user);f.family=family;if(f.throwCurrent)throw std::runtime_error("current");
   std::vector<Package> out;for(auto const& p:f.current)out.push_back({p});return out;}
  std::vector<Package> FindPackages(){if(f.throwGlobal)throw std::runtime_error("global");
   std::vector<Package> out;for(auto const& p:f.global)out.push_back({p});return out;}
  Op RemovePackageAsync(const winrt::hstring& full,RemovalOptions){++f.removals;f.removed=full;
   if(f.removeOk)f.current.erase(std::remove_if(f.current.begin(),f.current.end(),[&](auto const& p){return p.full==full;}),f.current.end());
   return {};}
 };
}
struct WinRtApartmentScope{bool CanUsePackageManager(){return f.apartment;}};
bool CanonicalizeOutputDirectory(wchar_t* p,size_t cap){size_t n=wcslen(p);if(!n || n+1>=cap)return false;
 if(p[n-1]!=L'\\'){p[n]=L'\\';p[n+1]=0;}return true;}
bool DeploymentSucceeded(winmd::Result r){return r.ok;}
enum class InstallerSparseState{Absent,Matching,Conflict,Unknown};
'''
main=r'''
int main(){using S=InstallerSparseState;auto reset=[](){f=Fake{};};auto q=[](){return QueryInstallerSparseState(L"D:\\Glass");};
 reset();assert(q()==S::Absent && f.family==GlassShared::GlassShellMenu::kSparsePackageFamilyName);
 reset();FakePackage p;f.current={p};f.global={p};assert(q()==S::Matching);
 reset();p.root=L"E:\\Other";f.current={p};f.global={p};assert(q()==S::Conflict);
 reset();p=FakePackage{};p.healthy=false;f.current={p};f.global={p};assert(q()==S::Conflict);
 reset();p=FakePackage{};f.current={p,p};f.global={p};assert(q()==S::Conflict);
 reset();f.global={p};assert(q()==S::Conflict); // only another user's package
 reset();FakePackage other=p;other.full=L"package_v2";f.current={p};f.global={p,other};assert(q()==S::Conflict);
 reset();other.name=L"unrelated";f.global={other};assert(q()==S::Absent);
 reset();f.apartment=false;assert(q()==S::Unknown);
 reset();f.throwCurrent=true;assert(q()==S::Unknown);
 reset();f.throwGlobal=true;assert(q()==S::Unknown);
 reset();f.current={p};f.global={p};assert(UnregisterInstallerSparsePackageQuiet(L"D:\\Glass"));
 assert(f.removals==1 && f.current.empty() && f.global.size()==1 && f.removed==p.full);
 reset();f.global={p};assert(!UnregisterInstallerSparsePackageQuiet(L"D:\\Glass") && f.removals==0);
 reset();f.current={p};f.global={p,other};other.name=L"GlassProject.GlassShellMenu";other.full=L"v2";f.global={p,other};
 assert(!UnregisterInstallerSparsePackageQuiet(L"D:\\Glass") && f.removals==0);
 reset();f.current={p};f.global={p};f.removeOk=false;assert(!UnregisterInstallerSparsePackageQuiet(L"D:\\Glass") && f.current.size()==1);
 std::cout<<"PASS: real sparse state and removal code: family/root/status, conflicts, exceptions, current-user only\n";
}
'''
with tempfile.TemporaryDirectory(prefix='glass-sparse-state-') as d:
    cpp=Path(d)/'test.cpp';exe=Path(d)/'test';cpp.write_text(prefix+query+'\n'+remove+'\n'+main,encoding='utf-8')
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-g','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer','-no-pie',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
