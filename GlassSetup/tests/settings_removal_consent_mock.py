"""Actual Setup Settings removal risk and confirmation methods, fake UI only."""
from pathlib import Path
import tempfile,subprocess
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/main.cpp').read_text()
code=s[s.index('    unsigned ManagedSettingsRemovalRisk()'):s.index('    void StartInstallation()')]
prefix=r'''
#include "GlassProducts.h"
#include "GlassProductPlan.h"
#include <cassert>
#include <string>
#include <iostream>
#define PRIMARYLANGID(x) (x)
constexpr unsigned LANG_RUSSIAN=25;unsigned gLanguage=LANG_RUSSIAN;
enum class Page{Components,Certificate,SettingsRemovalRisk};
struct App{
 GlassProducts::Selection selection_,installedSelection_;Page page_=Page::Components,beforeSettingsRemovalRisk_=Page::Components;
 unsigned pendingSettingsRemovalRisk_=0;int transitions=0;
 void SetPage(Page page){page_=page;++transitions;}
'''
main=r'''
};
int main(){
 for(unsigned present=0;present<4;++present)for(unsigned remove=0;remove<64;++remove){
  App a;a.installedSelection_.platform64=(present&1)!=0;a.installedSelection_.platform32=(present&2)!=0;a.selection_.removeMask=remove;
  auto risk=GlassProductPlan::SettingsRemovalRiskMask(remove&4,remove&32,present&1,present&2,remove&1,remove&2);
  assert(a.ManagedSettingsRemovalRisk()==risk);assert(a.ConfirmManagedSettingsRemoval()==!risk);
  assert(a.transitions==(risk?1:0));assert(a.selection_.removeMask==remove); // never expands removal selection
  if(risk){assert(a.page_==Page::SettingsRemovalRisk);assert(a.pendingSettingsRemovalRisk_==risk);assert(a.SettingsRemovalRiskText().find(L'\n')!=std::wstring::npos);
   if(risk&1)assert(a.SettingsRemovalRiskText().find(L"GlassSettings x64")!=std::wstring::npos);
   if(risk&2)assert(a.SettingsRemovalRiskText().find(L"GlassSettings x86")!=std::wstring::npos);
   a.selection_.confirmedSettingsRemovalRisk=risk;a.page_=Page::Components;assert(a.ConfirmManagedSettingsRemoval());}
 }
 {App a;a.page_=Page::Certificate;a.installedSelection_.platform64=true;a.selection_.removeMask=4;
  assert(!a.ConfirmManagedSettingsRemoval());assert(a.beforeSettingsRemovalRisk_==Page::Certificate);
  a.selection_.confirmedSettingsRemovalRisk=2;assert(!a.ConfirmManagedSettingsRemoval());} // other architecture is not consent
 gLanguage=0;{App a;a.pendingSettingsRemovalRisk_=1;assert(a.SettingsRemovalRiskText().find(L"Removing")!=std::wstring::npos);}
 std::cout<<"PASS: actual Setup consent: independent architectures, 256 presence/removal cases, explicit confirmation, no cascade, correct return page, RU/EN\n";
}
'''
with tempfile.TemporaryDirectory(prefix='SettingsConsent-') as d:
    path=Path(d)/'test.cpp';exe=Path(d)/'test';path.write_text(prefix+code+main)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(root/'GlassSetup/src'),str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
