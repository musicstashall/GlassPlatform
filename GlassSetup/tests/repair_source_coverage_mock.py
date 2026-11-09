"""Execute actual repair registration boundary; coverage gap is not publication failure."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassMaintenanceCache.h').read_text()
a=s.index('inline bool RegisterRepairSource(');b=s.index('inline bool GarbageCollect(',a)
h=r'''
#include <cassert>
#include <string>
#include <vector>
#include <cwchar>
#include <iostream>
#define _wcsicmp wcscasecmp
using UINT=unsigned;
constexpr unsigned ERROR_SUCCESS=0,MSIINSTALLCONTEXT_MACHINE=4,MSICODE_PRODUCT=0,MSISOURCETYPE_NETWORK=1;
constexpr const wchar_t* INSTALLPROPERTY_PACKAGENAME=L"PackageName",*INSTALLPROPERTY_LASTUSEDSOURCE=L"LastUsedSource";
struct Registration {std::wstring local=L"local.msi",version=L"1.0",sid=L"";unsigned context=4;};
std::vector<Registration> registered{Registration{}};
bool trust=true,identity=true,registrations=true,digest=true,emptyPackage=false;
unsigned apiFailure=0;int apiCalls=0,failures=0,gaps=0;
bool TrustedPath(const std::wstring&,bool){return trust;}
bool Identity(const std::wstring&,std::wstring& code,std::wstring& version){code=L"product";version=L"1.0";return identity;}
bool Registrations(const std::wstring&,std::vector<Registration>& all){all=registered;return registrations;}
std::wstring PackageCode(const std::wstring&){return emptyPackage?L"":L"package";}
bool PublishedDigestMatches(const std::wstring&){return digest;}
namespace Diag {bool Fail(const wchar_t*,const std::wstring&,unsigned,const std::wstring&,const wchar_t*){++failures;return false;}}
namespace GlassInstallLog {template<class... T>void LineFormat(const wchar_t*,T...){++gaps;}}
UINT MsiSourceListAddSourceExW(const wchar_t*,const wchar_t* user,unsigned context,unsigned,const wchar_t* folder,unsigned){
 assert((context==4)==(user==nullptr));assert(std::wstring(folder)==L"Current\\");++apiCalls;return apiFailure;
}
UINT MsiSourceListSetInfoW(const wchar_t*,const wchar_t*,unsigned,unsigned,const wchar_t*,const wchar_t*){++apiCalls;return apiFailure;}
// SOURCE
void reset(){registered={Registration{}};trust=identity=registrations=digest=true;emptyPackage=false;apiFailure=0;apiCalls=failures=gaps=0;}
int main(){std::vector<std::wstring> missing;
 reset();assert(RegisterRepairSource(L"Current\\Glass-x64.msi",&missing));assert(apiCalls==3&&failures==0&&missing.empty());
 reset();digest=false;assert(RegisterRepairSource(L"Current\\Glass-x64.msi",&missing));assert(apiCalls==0&&failures==0&&gaps==1&&missing==std::vector<std::wstring>{L"Current\\Glass-x64.msi"});
 // No result sink must still never bless or register legacy bytes.
 assert(RegisterRepairSource(L"Current\\Glass-x64.msi"));assert(apiCalls==0&&failures==0);
 reset();registered[0].context=2;registered[0].sid=L"user";assert(RegisterRepairSource(L"Current\\Glass-x64.msi"));assert(apiCalls==3);
 reset();apiFailure=5;assert(!RegisterRepairSource(L"Current\\Glass-x64.msi"));assert(failures==1);
 reset();registered[0].version=L"old";assert(RegisterRepairSource(L"Current\\Glass-x64.msi"));assert(apiCalls==0);
 for(int i=0;i<4;++i){reset();if(i==0)trust=false;if(i==1)identity=false;if(i==2)registrations=false;if(i==3)emptyPackage=true;
  assert(!RegisterRepairSource(L"Current\\Glass-x64.msi"));assert(apiCalls==0);}
 std::cout<<"PASS: actual repair registration: verified context-aware registration, unbound legacy coverage without source registration, real MSI errors preserved\n";
}
'''.replace('// SOURCE',s[a:b])
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
