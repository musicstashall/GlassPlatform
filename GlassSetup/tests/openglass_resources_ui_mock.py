"""Compile actual UI selection/Next adapters, no Windows/network operations."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/main.cpp').read_text()
def fn(token):
 a=s.index(token);b=s.index('{',a);i=b+1;depth=1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]
methods='\n'.join(fn(token) for token in ('std::array<bool,7> DownloadSelection()', 'bool HasSelectedComponent()', 'bool ComponentsReady()'))
code=r'''
#include "GlassProducts.h"
#include "GlassProcedurePlan.h"
#include <array>
#include <atomic>
#include <cassert>
bool native64=true;
namespace GlassInstallUtil {bool IsOs64Bit(){return native64;}}
enum class Page {Components,Certificate,Installing};
struct UI {
 GlassProducts::Selection selection_;Page page_=Page::Components;
 bool resourcesPrerequisiteReady_=false,downloadContinuePending_=false,downloadsReady=false;
 std::atomic<bool> inspectReady_{true};
 bool DownloadsReady() const {return downloadsReady;}
 // METHODS
};
int main(){
 using namespace GlassProducts;
 for(bool os64:{false,true})for(bool installed:{false,true})for(unsigned mask=0;mask<16;++mask){
  UI ui;native64=os64;auto& s=ui.selection_;s.platform64=s.platform32=s.glassSettings=s.openGlass=false;
  s.independentSettings=true;ui.resourcesPrerequisiteReady_=installed;
  s.openGlassResources=mask&1;s.openGlass=mask&2;s.removeMask=(mask&4?8u:0u)|(mask&8?64u:0u);
  assert(ResourcesApplyReady(s,os64,installed)==(os64&&(installed||s.openGlass)&&!s.Removes(3)&&(!s.openGlass||!s.removeMask)));
  const auto selected=ui.DownloadSelection();assert(selected.size()==7);
  assert(selected[6]==(os64&&s.openGlassResources&&!s.Removes(6)));
  assert(!selected[5]); // SUT is not the catalog's Resources index.
  const bool expected=(mask!=0)&&ResourcesPlanReady(s,os64,installed);
  assert(ui.ComponentsReady()==expected);
  ui.inspectReady_=false;assert(!ui.ComponentsReady());ui.inspectReady_=true;
  ui.downloadContinuePending_=true;assert(!ui.ComponentsReady());
  ui.downloadsReady=true;assert(ui.ComponentsReady()==expected);
 }
 // SUT apply/removal may precede Resources and OG in the same plan.
 for(bool resources:{false,true})for(bool sutRemove:{false,true}){
  UI blocked;native64=true;auto& b=blocked.selection_;
  b.openGlass=true;b.openGlassResources=resources;
  b.secureUxTheme=!sutRemove;b.secureUxThemeRemove=sutRemove;
  assert(blocked.ComponentsReady());
  assert(MixedOpenGlassApply(b,false,true));
  b.secureUxTheme=b.secureUxThemeRemove=false;
  assert(blocked.ComponentsReady());
  b.openGlass=b.openGlassResources=false;b.secureUxTheme=true;
  assert(blocked.ComponentsReady()); // managed Glass + SUT remains supported
 }
 UI ui;native64=true;ui.resourcesPrerequisiteReady_=true;
 auto& s=ui.selection_;s.platform64=s.platform32=s.glassSettings=s.openGlass=false;s.independentSettings=true;s.openGlassResources=true;
 assert(ui.HasSelectedComponent()&&ui.ComponentsReady());
 assert(GlassProcedurePlan::MutationMask(s,true)==64);
 assert(!GlassProducts::NeedsHostLaunch(s,{}));
 s.openGlassResources=false;s.removeMask=64;ui.resourcesPrerequisiteReady_=false;
 assert(ui.ComponentsReady());assert(!ui.DownloadSelection()[6]); // orphan removal has no download/prerequisite
}
'''.replace('// METHODS',methods)
# Wiring/architecture guards are source contracts, not an MSVC build.
assert 'item(L"openglassResources",IDS_COMP_OGR_TITLE,IDS_COMP_OGR_DESC,selection_.openGlassResources,false,!os64,&inspect_.openGlassResources)' in s
assert '{L"openglassResources",6}' in s
assert 'if(managedRow==6)selection_.openGlassResources=false' in s
assert 'ShowWindow(componentResources_,nativeComponents && os64?SW_SHOWNA:SW_HIDE)' in s
assert 'resourcesPrerequisiteReady_=os64&&inspect_.openGlassResourcesPrerequisite' in s
assert 'for(std::size_t i=0;i<std::size(downloadIds);++i)' in s
with tempfile.TemporaryDirectory(prefix='ResourcesUi-') as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(code)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(root/'GlassSetup/src'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: actual Resources UI selection/Next gates, seven-slot selection, x86/prerequisite/conflict refusal, orphan removal, native/IPC wiring')
