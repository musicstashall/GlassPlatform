"""Actual cached Setup entrypoint identity guard, with fake path/hash/ACL APIs."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassMaintenanceLaunch.cpp').read_text()
a=s.index('[[nodiscard]] bool IsTrustedCachedSetupImage()');b=s.index('\n} // namespace',a)
fn=s[a:b]
h=r'''
#include <string>
#include <cassert>
bool trusted=true,dirs=true;std::wstring self,active=L"root\\Installer\\active\\GlassSetup.exe";std::string digest(64,'a');
std::wstring ModulePath(){return self;}std::wstring LongPath(const std::wstring& p){return p;}
bool PathsEqualCanonical(const std::wstring& a,const std::wstring& b){return a==b;}
namespace GlassMaintenanceCache {
std::wstring CurrentFile(const wchar_t*){return active;}bool TrustedPath(const std::wstring&,bool){return trusted;}
std::string ControllerDigest(const std::wstring&){return digest;}std::wstring Root(){return L"root";}
bool TrustedDirectory(const std::wstring&){return dirs;}
}
// FN
int main(){
 self=active;assert(IsTrustedCachedSetupImage());
 const auto retained=L"root\\Installer\\"+std::wstring(64,L'a')+L"\\GlassSetup.exe";
 self=retained;assert(IsTrustedCachedSetupImage());
 trusted=false;assert(!IsTrustedCachedSetupImage());trusted=true;
 dirs=false;assert(!IsTrustedCachedSetupImage());dirs=true;
 self=L"download\\GlassSetup.exe";assert(!IsTrustedCachedSetupImage());
 self=retained;digest=std::string(64,'b');assert(!IsTrustedCachedSetupImage());digest=std::string(64,'a');
 self=L"root\\Installer\\"+std::wstring(64,L'a')+L"\\GlassUnins.exe";assert(!IsTrustedCachedSetupImage());
 self=retained;active.clear();assert(!IsTrustedCachedSetupImage());
}
'''.replace('// FN',fn)
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(h)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
# This identity check must not remove the subsequent signer/launch authorization.
auth=s[s.index('bool AuthorizeMaintenanceModifyLaunch()'):s.index('// Independent immutable')]
for token in ['IsTrustedCachedSetupImage()', 'FileHasSameSignerAsSetup(self)', 'IsShellMaintenanceParent()', 'TryConsumeMaintenanceTicket()']:
 assert token in auth
print('PASS: retained cached Setup entrypoint identity, hash-folder/ACL/path refusal; signer and parent/ticket checks retained')
