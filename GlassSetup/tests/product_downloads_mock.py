#!/usr/bin/env python3
"""Production coordinator, concurrent fake providers; no network/Windows/product changes."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'GlassSetup/src/GlassProductDownloads.h').read_text()
body='\n'.join(line for line in source.splitlines() if not line.startswith(('#include','#pragma')))
prefix=r'''
#include <atomic>
#include <array>
#include <thread>
#include <vector>
#include <memory>
#include <algorithm>
#include <functional>
#include <filesystem>
#include <string>
#include <mutex>
#include <chrono>
#include <cassert>
#include <iostream>
using ULONGLONG=unsigned long long;using HRESULT=int;constexpr int COINIT_MULTITHREADED=0;
ULONGLONG GetTickCount64(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
HRESULT CoInitializeEx(void*,int){return 0;}void CoUninitialize(){}
bool FAILED(HRESULT h){return h<0;}bool SUCCEEDED(HRESULT h){return h>=0;}
namespace InstallLog {template<class... T>void LineFormat(const wchar_t*,T...) {}}
namespace GlassReleaseClient {inline thread_local std::function<void(std::uint64_t,std::uint64_t)> packageProgress;std::wstring Wide(const char*){return L"package.msi";}}
namespace GlassSetupSourceLayout {std::array<std::wstring,6> prepared;void SetPrepared(std::size_t i,const std::wstring& path){prepared[i]=path;}}
namespace GlassMaintenanceCache {std::atomic<int> removed{0};bool RemoveFlat(const std::wstring&){++removed;return true;}}
namespace GlassPackageCatalog {enum class Product:std::size_t {A,B,C,D,E};struct Asset{unsigned size=100;};struct Catalog{std::array<std::shared_ptr<Asset>,5> products{};};constexpr const char* kFiles[]={"a","b","c","d","e"};}
std::array<std::atomic<int>,6> calls{};std::array<std::atomic<bool>,6> release{},fail{};
std::atomic<int> serial{0};
bool provider(std::size_t i,const std::atomic<bool>& cancel,std::wstring& error){
 ++calls[i];if(GlassReleaseClient::packageProgress)GlassReleaseClient::packageProgress(50,100);
 while(!release[i]&&!cancel)std::this_thread::sleep_for(std::chrono::milliseconds(1));
 if(cancel){error=L"cancelled";return false;}if(fail[i]){error=L"download failed";return false;}return true;
}
namespace GlassPackageAcquisition {
bool CreateWorkspace(std::wstring& path){path=L"private-"+std::to_wstring(++serial);return true;}
bool Prepare(GlassPackageCatalog::Product p,const GlassPackageCatalog::Catalog&,const std::wstring&,const std::atomic<bool>& cancel,std::wstring& error){return provider(static_cast<std::size_t>(p),cancel,error);}
}
namespace GlassSecureUxPackage {struct Package{std::wstring path;};bool Prepare(const std::wstring& dir,const std::atomic<bool>& cancel,Package& p,std::wstring& error){p.path=dir+L"/original.msi";return provider(5,cancel,error);}}
'''
main=r'''
template<class P>void wait(P predicate){auto deadline=GetTickCount64()+3000;while(!predicate()){assert(GetTickCount64()<deadline);std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
int main(){
 GlassProductDownloads downloads;GlassPackageCatalog::Catalog catalog;
 std::array<bool,6> selected{{true,true,true,true,true,true}};
 downloads.Reconcile(selected,catalog);wait([]{for(auto& c:calls)if(c.load()!=1)return false;return true;});
 assert(!downloads.Ready(selected));wait([&]{return downloads.jobs[0]->bytes==50;});
 auto stale=downloads.jobs[1];selected[1]=false;downloads.Reconcile(selected,catalog);assert(stale->cancel&&GlassSetupSourceLayout::prepared[1].empty());
 selected[1]=true;downloads.Reconcile(selected,catalog);assert(downloads.jobs[1]!=stale);wait([]{return calls[1]==2;});
 fail[2]=true;for(auto& gate:release)gate=true;
 wait([&]{for(auto& job:downloads.jobs)if(!job->done.load())return false;return true;});
 downloads.Reconcile(selected,catalog);
 assert(GlassSetupSourceLayout::prepared[1].find(downloads.jobs[1]->directory)==0);
 assert(GlassSetupSourceLayout::prepared[2].empty()&&!downloads.Ready(selected));
 fail[2]=false;downloads.Retry(2);downloads.Reconcile(selected,catalog);wait([&]{return downloads.jobs[2]->done.load();});downloads.Reconcile(selected,catalog);
 assert(downloads.Ready(selected));assert(calls[2]==2);assert(!GlassSetupSourceLayout::prepared[5].empty());
 selected[0]=false;downloads.Reconcile(selected,catalog);assert(GlassSetupSourceLayout::prepared[0].empty());
 stale.reset();downloads.Stop();downloads.Join();for(auto& p:GlassSetupSourceLayout::prepared)assert(p.empty());assert(GlassMaintenanceCache::removed==serial);
 std::cout<<"PASS: six parallel production jobs, cancellation/reselection generations, failed retry, ready gate, verified hints, cleanup\n";
}
'''
with tempfile.TemporaryDirectory(prefix='glass-product-downloads-') as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(prefix+body+main)
 subprocess.run(['g++','-std=c++17','-pthread','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
