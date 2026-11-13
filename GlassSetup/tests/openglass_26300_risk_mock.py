"""Actual known-release risk policy; future releases/builds are not blanket-banned."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
h=r'''
#include "GlassSetup/src/InstallSupport/GlassOpenGlassRelease.h"
#include <cassert>
int main(){using GlassOpenGlassRelease::Known26300Risk;
 assert(Known26300Risk(26300,L"3.0.2.3749"));
 for(unsigned build:{0u,19045u,22000u,26100u,26200u,28000u})assert(!Known26300Risk(build,L"3.0.2.3749"));
 for(const auto* version:{L"",L"3.0.1.3747",L"3.0.2.3750",L"3.1.0.4266",L"garbage",L"3.0.2.3749-extra",L"３.0.2.3749"})assert(!Known26300Risk(26300,version));
}
'''
s=(root/'GlassSetup/src/main.cpp').read_text()
assert 'openGlass&&checked&&!removing&&GlassOpenGlassRelease::Known26300Risk(windowsBuild,targetVersion)' in s
assert 'warning+=GlassOpenGlassRelease::Known26300RiskText' in s
header=(root/'GlassSetup/src/InstallSupport/GlassOpenGlassRelease.h').read_text()
assert '26300.9818' in header and 'https://github.com/ALTaleX531/OpenGlass/releases/tag/v3.0.2.3749' in header
assert 'targetVersion=rep?rep->targetVersion:L""' in s # remote candidate is not verified-local; preserve discovered metadata
assert 'selectedLocal=!openGlass&&rep&&rep->sourceVerified' in s # media trust remains independent
select=s[s.index('if(message==L"components:select-all")'):s.index('else if (message==L"component:x64:1"')]
assert 'selection_.openGlass=' not in select and 'selection_.openGlassResources=' not in select
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(root),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: actual OG 26300 known-release risk; selected non-removal only, native recommendations exclude OG/Resources; server metadata does not require verified-local flag')
