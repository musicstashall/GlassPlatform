"""Actual ComponentsJson metadata projection: remote offer is not verified-local media."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/main.cpp').read_text()
a=s.index('            std::wstring remoteVersion,targetVersion=')
b=s.index('            const bool openGlass=',a)
projection=s[a:b]
h=r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
namespace GlassProducts {enum class ProductCondition {Healthy,UpdateAvailable};}
struct Report {bool sourceVerified=false,installed=false,damaged=false;std::wstring targetVersion,installedPackageVersion;};
struct Asset {bool downloadable=true;struct {std::array<unsigned,4> parts{2,0,0,117};}version;std::uint64_t size=1000;};
struct {std::array<std::optional<Asset>,6> products;}releaseCatalog_;
bool VersionOlder(const std::wstring& a,const std::wstring& b){return a<b;}
std::wstring Project(const Report* rep){int catalogIndex=0;auto condition=GlassProducts::ProductCondition::Healthy;
// ACTUAL
 (void)condition;(void)packageSize;return targetVersion;
}
int main(){
 Report r;r.targetVersion=L"2.0.0.115";
 assert(!r.sourceVerified&&Project(&r)==r.targetVersion); // server candidate, not local
 r.sourceVerified=true;assert(Project(&r)==r.targetVersion);
 releaseCatalog_.products[0]=Asset{};assert(Project(&r)==L"2.0.0.117");
 releaseCatalog_.products[0]->downloadable=false;assert(Project(&r)==r.targetVersion);
 r.targetVersion.clear();assert(Project(&r).empty()); // no eligible source is still unavailable
 releaseCatalog_.products[0]->downloadable=true;assert(Project(nullptr)==L"2.0.0.117");
 releaseCatalog_.products[0].reset();assert(Project(nullptr).empty());
}
'''.replace('// ACTUAL',projection)
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
assert 'selectedLocal=!openGlass&&rep&&rep->sourceVerified' in s
print('PASS: actual UI source projection preserves discovered remote metadata, latest downloadable catalog target and unavailable-source gate; verified-local classification remains separate')
