"""Actual managed removal IPC plus Apply readiness, all removal subsets on x64."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/main.cpp').read_text()
a=s.index('            unsigned managedRow=7;',s.index('HRESULT HandleWebMessage('));b=s.index('            if(message==L"components:select-all")',a)
ipc=s[a:b]
def fn(t):
 a=s.index(t);b=s.index('{',a);depth=1;i=b+1
 while depth:depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]
methods='\n'.join(fn(t) for t in ['bool HasSelectedComponent()', 'bool OpenGlassApplyPlanSupported()', 'bool ComponentsReady()'])
code='\n#include "GlassProducts.h"\n#include <string>\n#include <mutex>\n#include <atomic>\n#include <cassert>\n#include <utility>\nnamespace GlassInstallUtil{bool IsOs64Bit(){return true;}}\nenum class Page{Components,Certificate};\nstruct Fact{bool installed=true,residual=false,residualSafe=false;};\nstruct UI{\n GlassProducts::Selection selection_;Page page_=Page::Components;\n bool ogMetadataBlocked_=false,sutMetadataBlocked_=false,ogMetadataLocal_=false,sutMetadataLocal_=false;bool resourcesPrerequisiteReady_=false,downloadContinuePending_=false;\n std::atomic_bool inspectReady_{true};std::mutex inspectMutex_;\n struct{Fact x64,x86,settings64,og,openGlassResources,settings32;}inspect_;\n bool DownloadsReady()const{return true;}\n UI(){selection_.platform64=selection_.platform32=selection_.glassSettings=selection_.openGlass=false;selection_.independentSettings=true;}\n void Remove(const std::wstring& message){const bool os64=true;\n // IPC\n }\n // METHODS\n};\nint main(){\n const wchar_t* ids[]={L"x64",L"x86",L"settings64",L"openglass",L"unused",L"settings32",L"openglassResources"};\n for(unsigned mask=1;mask<128;++mask){if(mask&~GlassProducts::Selection::kManagedRemovalMask)continue;\n  UI ui;for(unsigned i=0;i<7;++i)if(mask&(1u<<i))ui.Remove(L"component:"+std::wstring(ids[i])+L":remove");\n  assert(ui.selection_.removeMask==mask);assert(ui.ComponentsReady());\n  assert(!ui.selection_.openGlassResources&&!ui.selection_.openGlass);\n }\n UI resources;resources.selection_.openGlassResources=true;\n resources.Remove(L"component:openglassResources:remove");\n assert(resources.selection_.removeMask==64&&!resources.selection_.openGlassResources&&resources.ComponentsReady());\n resources.inspectReady_=false;assert(!resources.ComponentsReady());\n}\n'
code=code.replace('// IPC',ipc).replace('// METHODS',methods)
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(code)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(root/'GlassSetup/src'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: actual Resources remove IPC clears Apply flag; all 63 managed removal subsets enable Apply; inspection-not-ready stays blocked')
