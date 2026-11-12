"""Real MSI version reader + Unins projection, mocked registration APIs.
Also checks build-property wiring; does NOT execute MSBuild/WiX or Windows MSI.
"""
from pathlib import Path
import subprocess,tempfile,xml.etree.ElementTree as ET
root=Path(__file__).resolve().parents[2]
msi=(root/'GlassSetup/src/InstallSupport/GlassInstallMsi.cpp').read_text()
unins=(root/'GlassSetup/GlassUnins/main.cpp').read_text()
def function(s,token):
 a=s.index(token);b=s.index('{',a);depth=0
 for i in range(b,len(s)):
  if s[i]=='{':depth+=1
  elif s[i]=='}':
   depth-=1
   if not depth:return s[a:i+1]
 raise ValueError(token)
reader=function(msi,'std::wstring QueryDisplayVersion(')
older=function((root/'GlassSetup/src/main.cpp').read_text(),'bool VersionOlder(')
projection=function(unins,'    std::wstring InstalledMsiVersion(')
# Overrides are per MSI project and feed the Property-table ProductVersion,
# not just a caption, PE resource or GLASSBUNDLEVERSION marker.
target=ET.parse(root/'GlassSetup/msi/GlassSetupPayload.targets').getroot()
metadata=target.find("Target[@Name='GlassWriteProductMetadata']")
for project,prop in [('Glass-x64','GlassPlatform64MsiVersion'),('Glass-x86','GlassPlatform32MsiVersion'),
 ('GlassSettings-x64','GlassSettings64MsiVersion'),('GlassSettings-x86','GlassSettings32MsiVersion')]:
 nodes=[n for n in metadata.findall('./PropertyGroup/_GlassMsiVersion') if f"== '{project}'" in n.get('Condition','')]
 assert len(nodes)==1 and nodes[0].text==f'$({prop})'
for name in ['PlatformPackage','GlassSettingsPackage']:
 package=ET.parse(root/f'GlassSetup/msi/{name}.template.wxs').getroot()[0]
 assert package.get('Version')=='$(var.GlassMsiVersion)'
ET.parse(root/'GlassSetup/msi/ProductVersions.props')
h=r'''
#include "GlassSetup/src/InstallSupport/GlassPackageRules.h"
#include <cassert>
#include <cwchar>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#include <iostream>
using DWORD=unsigned;constexpr unsigned ERROR_SUCCESS=0;
constexpr const wchar_t* INSTALLPROPERTY_VERSIONSTRING=L"VersionString";
std::map<std::wstring,std::wstring> versions;std::vector<std::wstring> official;unsigned inventoryStatus=0;
unsigned MsiGetProductInfoW(const wchar_t* code,const wchar_t* property,wchar_t* out,DWORD* size){
 assert(std::wstring(property)==INSTALLPROPERTY_VERSIONSTRING);auto it=versions.find(code);
 if(it==versions.end())return 1605;if(it->second.size()>=*size)return 234;
 std::wcscpy(out,it->second.c_str());return 0;}
namespace GlassInstallMsi {
constexpr const wchar_t* kProductPlatformX64=L"p64",*kProductPlatformX86=L"p32",*kLegacyPlatformX64=L"old64",*kLegacyPlatformX86=L"old32",
 *kProductSettingsX64=L"s64",*kProductSettingsX86=L"s32",*kProductOpenGlass=L"og",*kLegacyOpenGlass=L"oldog",
 *kProductSecureUxThemeX64=L"sut64",*kProductSecureUxThemeX86=L"sut32";
bool IsProductInstalled(const wchar_t* code){return versions.count(code);}
// READER
}
namespace Shared::SecureUxThemeInventory{
unsigned QueryInstalled(std::vector<std::wstring>& out){out=official;return inventoryStatus;}}
namespace GlassOpenGlassFiles {bool HasReceipt(const std::wstring&){return false;}struct Record{std::wstring version;};Record Inspect(const std::wstring&){return {};}}
struct App{std::wstring openGlassDir_;
// PROJECTION
};
// OLDER
int main(){App app;
 assert(VersionOlder(L"0.9.9",L"0.10.0"));
 assert(VersionOlder(L"1.9.0",L"1.10.0"));
 assert(!VersionOlder(L"3.2.1",L"3.2.1.0"));
 assert(!VersionOlder(L"invalid",L"9.0.0"));
 assert(!VersionOlder(L"\u0131.0.0",L"9.0.0"));
 assert(!VersionOlder(L"9.0.0",L"1.0.0"));
 versions={{L"p64",L"2.0.1.9"},{L"p32",L"2.0.2.10"},{L"s64",L"3.1.4.11"},{L"s32",L"3.1.5.12"},{L"og",L"1.7.0.13"}};
 const wchar_t* ids[]={L"x64",L"x86",L"settings64",L"settings32",L"openglass"};
 const wchar_t* values[]={L"2.0.1.9",L"2.0.2.10",L"3.1.4.11",L"3.1.5.12",L"1.7.0.13"};
 for(unsigned i=0;i<5;++i)assert(app.InstalledMsiVersion(ids[i])==values[i]);
 versions[L"old64"]=L"9.0.0";assert(app.InstalledMsiVersion(L"x64").empty());versions.erase(L"p64");
 assert(app.InstalledMsiVersion(L"x64")==L"9.0.0");
 assert(GlassInstallMsi::QueryDisplayVersion(nullptr).empty());assert(GlassInstallMsi::QueryDisplayVersion(L"absent").empty());
 versions[L"s32"]=std::wstring(80,L'9');assert(app.InstalledMsiVersion(L"settings32").empty());
 official={L"vendor"};versions[L"vendor"]=L"4.0.0";assert(app.InstalledMsiVersion(L"secureUxTheme")==L"4.0.0");
 official.push_back(L"another");assert(app.InstalledMsiVersion(L"secureUxTheme").empty());official.clear();
 versions[L"sut64"]=L"2.9.0";assert(app.InstalledMsiVersion(L"secureUxTheme")==L"2.9.0");
 inventoryStatus=5;assert(app.InstalledMsiVersion(L"secureUxTheme").empty());
 std::cout<<"PASS: actual MSI version API/Unins projection: five distinct versions, legacy, unknown, oversized, ambiguous SUT; independent MSI build-property wiring\n";
}
'''.replace('// READER',reader).replace('// PROJECTION',projection).replace('// OLDER',older)
with tempfile.TemporaryDirectory(prefix='MsiVersions-') as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(h)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I',str(root),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
