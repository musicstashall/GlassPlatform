"""Actual FetchCatalog: reuse valid snapshots; bounded recovery of empty/invalid inherited catalogs."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassPackageAcquisition.cpp').read_text()
a=s.index('bool FetchCatalog(');b=s.index('// Shared read-only discovery',a)
h=r'''
#include <atomic>
#include <string>
#include <cassert>
#include <iostream>
#include "GlassSetup/src/InstallSupport/GlassSetupSourceLayout.h"
using HANDLE=int;constexpr int INVALID_HANDLE_VALUE=-1;
int requests=0;bool network=true,valid=true;
namespace GlassPackageCatalog {struct Catalog {bool parsed=false;};bool Parse(const std::string&,Catalog& c,std::string&){c.parsed=valid;return valid;}}
namespace GlassReleaseClient {
std::wstring Wide(const std::string& text){return std::wstring(text.begin(),text.end());}
bool Get(const wchar_t*,int,std::string* text,int,const std::atomic<bool>&,std::wstring&){++requests;*text="catalog";return network;}
}
using GlassReleaseClient::Wide;
bool Fail(std::wstring& error,const std::wstring& text){error=text;return false;}
// SOURCE
void reset(){requests=0;network=valid=true;GlassSetupSourceLayout::catalogChecked=false;GlassSetupSourceLayout::catalogInherited=false;GlassSetupSourceLayout::catalogText.clear();}
int main(){std::atomic<bool> cancel=false;GlassPackageCatalog::Catalog catalog;std::wstring error;
 reset();assert(FetchCatalog(cancel,catalog,error)&&requests==1);assert(FetchCatalog(cancel,catalog,error)&&requests==1); // Product inspection reuses the setup check.
 GlassSetupSourceLayout::catalogInherited=true;GlassSetupSourceLayout::catalogChecked=false;network=false;
 assert(FetchCatalog(cancel,catalog,error)&&catalog.parsed&&requests==1); // Child, no network even when now offline.
 reset();network=false;assert(!FetchCatalog(cancel,catalog,error)&&requests==1);assert(!FetchCatalog(cancel,catalog,error)&&requests==1);
 GlassSetupSourceLayout::catalogInherited=true;GlassSetupSourceLayout::catalogChecked=false;network=true;
 assert(FetchCatalog(cancel,catalog,error)&&requests==2); // Child recovers a parent failure once.
 assert(FetchCatalog(cancel,catalog,error)&&requests==2);
 reset();GlassSetupSourceLayout::catalogInherited=true;network=false;
 assert(!FetchCatalog(cancel,catalog,error)&&requests==1);assert(!FetchCatalog(cancel,catalog,error)&&requests==1);
 reset();GlassSetupSourceLayout::catalogInherited=true;GlassSetupSourceLayout::catalogText="malformed";valid=false;
 assert(!FetchCatalog(cancel,catalog,error)&&requests==1);assert(!FetchCatalog(cancel,catalog,error)&&requests==1);
 reset();valid=false;assert(!FetchCatalog(cancel,catalog,error)&&requests==1);assert(!FetchCatalog(cancel,catalog,error)&&requests==1);
 std::cout<<"PASS: actual catalog fetch: one request, immutable product-inspection reuse, child snapshot reuse, offline/malformed result not retried\n";
}
'''.replace('// SOURCE',s[a:b])
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(root),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
