"""Maintenance UI entry policy; not --modify authorization or EULA consent."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/main.cpp').read_text()
a=s.index('    bool MaintenanceUi() const noexcept {');b=s.index('\n    }',a)+6
policy=s[a:b]
source='''#include <cassert>
struct Wizard {bool maintenanceMode_=false,existingInstallationDetected_=false,accepted_=false;
'''+policy+'''
};
int main(){
 for(int auth=0;auth<2;++auth)for(int installed=0;installed<2;++installed)for(int accepted=0;accepted<2;++accepted){
  Wizard w;w.maintenanceMode_=auth;w.existingInstallationDetected_=installed;w.accepted_=accepted;
  assert(w.MaintenanceUi()==(auth||(installed&&accepted)));
 }
 Wizard regular;regular.existingInstallationDetected_=true;regular.accepted_=true;
 assert(regular.MaintenanceUi()&&!regular.maintenanceMode_);
}
'''
assert 'if(MaintenanceUi()&&page_==Page::Components)return 0;' in s
assert 'EnableWindow(back_,!demoLocked && !(MaintenanceUi() && page_==Page::Components));' in s
assert '&& !(MaintenanceUi() && page_==Page::Components);' in s
assert 'if(MaintenanceUi()&&(page==Page::License||page==Page::Welcome))page=Page::Components;' in s
assert 'EnsureCurrentController(prepared,maintenanceMode_,maintenanceFocusProduct_,error)' in s
inspection=s[s.index('    void OnInspectReady()'):s.index('    void SetPage(Page page)')]
for field in ['x64','x86','settings64','settings32','og','openGlassResources','secureUxTheme']:
 assert 'inspect_.'+field+'.installed' in inspection
assert 'inspect_.og.residual' not in inspection[inspection.index('existingInstallationDetected_='):inspection.index('selection_.platform64=')]
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(source);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: normal installed/modify/fresh navigation policy, Back visibility+disable+handler guards, entry redirect; authorization and EULA kept separate (not native Windows UI coverage)')
