#!/usr/bin/env python3
"""Compile actual UI Preview; execution planner remains untouched."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/GlassProductUiPolicy.h').read_text()
preview=s[s.index('inline GlassProductPlan::Plan Preview('):s.index('inline std::wstring ShellSuffix(')]
h=r'''
#include "GlassSetup/src/GlassProductPlan.h"
#include "GlassSetup/src/GlassProducts.h"
#include <cassert>
// PREVIEW
int main(){
 using namespace GlassProducts;namespace P=GlassProductPlan;
 Selection empty;empty.platform64=empty.platform32=empty.glassSettings=empty.openGlass=false;
 empty.independentSettings=true;
 for(bool os64:{false,true})for(unsigned mask=0;mask<64;++mask){
  auto selected=empty;selected.platform64=mask&1;selected.platform32=mask&2;
  selected.settings64=mask&4;selected.openGlass=mask&8;selected.secureUxTheme=mask&16;selected.settings32=mask&32;
  auto result=Preview(selected,empty,os64,false,false);
  assert(result.warnings==P::Warning::None);
  for(const auto& row:result.rows)assert(row.warnings==P::Warning::None);
 }
 auto sutOnly=empty;sutOnly.secureUxTheme=true;
 auto idle=Preview(empty,sutOnly,true,false,false);
 assert(idle.warnings==P::Warning::None);
 for(const auto& row:idle.rows)assert(row.warnings==P::Warning::None);
 auto present=empty;present.platform64=true;
 auto retained=Preview(empty,present,true,false,false);
 assert(retained.warnings!=P::Warning::None); // Real retained-product warnings survive.
}
'''.replace('// PREVIEW',preview)
with tempfile.TemporaryDirectory() as tmp:
 cpp=Path(tmp)/'preview.cpp';exe=Path(tmp)/'preview';cpp.write_text(h)
 subprocess.run(['g++','-std=c++17','-I',str(root),str(cpp),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: actual preview: empty inventory has no row/summary warnings across 128 plans; installed dependency warnings retained')
