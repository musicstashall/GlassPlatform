"""Actual diagnostic probe and UI classification: leftovers do not imply OG presence.
Fake APIs only; not Windows SCM/MSI acceptance.
"""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'GlassSetup/src/InstallSupport/GlassProductInspect.cpp').read_text()
start=source.index('    // Service/task leftovers')
probe=source[start:source.index('    GlassInstallUtil::InstallRoots roots{};',start)]
main=(root/'GlassSetup/src/main.cpp').read_text()
condition=main[main.index('    static GlassProducts::ProductCondition ConditionOf('):main.index('    static UINT InspectReasonId(')]
h=r'''
#include "GlassProducts.h"
#include <cassert>
#include <initializer_list>
using DWORD=unsigned;
struct State{bool exists=false,stopped=false;DWORD error=0;int probes=0;} s;
namespace GlassProductInspect {struct ProductReport {bool installed=false,residual=false,residualSafe=false,damaged=false,updateAvailable=false;};
struct Report{ProductReport og;};struct Inputs{bool os64=true;};}
namespace GlassOpenGlassService {DWORD Probe(bool& exists,bool& stopped){++s.probes;exists=s.exists;stopped=s.stopped;return s.error;}}
namespace GlassInstallLog {template<class... T>void LineFormat(const wchar_t*,T...){} }
GlassProductInspect::Report Inspect(bool os64=true){GlassProductInspect::Inputs in{os64};GlassProductInspect::Report report;
// PROBE
return report;}
// CONDITION
int main(){using C=GlassProducts::ProductCondition;
 for(bool exists:{false,true})for(bool stopped:{false,true})for(DWORD error:{0u,5u,1072u}){
  s={exists,stopped,error,0};auto r=Inspect();assert(s.probes==1);
  assert(!r.og.installed&&!r.og.residual&&!r.og.residualSafe&&!r.og.damaged);
  assert(ConditionOf(&r.og,false)==C::NotInstalled);
 }
 s={};Inspect(false);assert(s.probes==0);
}
'''.replace('// PROBE',probe).replace('// CONDITION',condition)
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(h)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(root/'GlassSetup/src'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
assert 'report.og.residual=serviceExists' not in source
assert 'report.og=ProductReport{};' in source
assert source.index('report.og=ProductReport{};') < source.index('GlassOpenGlassInstall::Query(official,inventoryError)')
assert 'report.openGlassRetiredMsi=report.og.installed' in source
assert 'report.openGlassRetiredReceipt=' in source
assert '!report.openGlassResourcesPrerequisite||report.og.installedPayloadVersion.empty()' in source
assert 'AddReason(report.og, Reason::AutostartMissing)' not in source
assert 'sanity.openGlassIssues && report.og.installed' in source
print('PASS: OG leftover service incl.1072 is diagnostic only; retired facts separate; official DLL presence guards and no service-based damage')
