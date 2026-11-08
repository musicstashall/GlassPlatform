"""Exhaustive union-of-actions procedure map, not Windows runtime."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
h=r'''
#include "GlassProcedurePlan.h"
#include <cassert>
#include <iostream>
int main(){unsigned cases=0;
 for(bool os64:{false,true})for(unsigned combination=0;combination<243;++combination){
  auto n=combination;GlassProducts::Selection s;
  s.platform64=s.platform32=s.glassSettings=s.openGlass=false;s.independentSettings=true;
  bool* choices[]={&s.platform64,&s.platform32,&s.settings64,&s.openGlass,&s.settings32};
  const unsigned bits[]={1,2,4,8,32};unsigned expected=0;
  for(unsigned row=0;row<5;++row){const auto action=n%3;n/=3;
   if(!os64&&(row==0||row==2||row==3))continue;
   if(action){expected|=bits[row];if(action==1)*choices[row]=true;else s.removeMask|=bits[row];}}
  s.SyncSettings(os64);const auto mask=GlassProcedurePlan::MutationMask(s,os64);assert(mask==expected);
  assert(GlassProcedurePlan::Platform64(mask)==bool(expected&5));
  assert(GlassProcedurePlan::Platform32(mask)==bool(expected&34));
  assert(GlassProcedurePlan::OpenGlass(mask)==bool(expected&8));
  assert(GlassProcedurePlan::Settings(mask)==bool(expected&36));
  assert(GlassProcedurePlan::ModuleScan(mask)==bool(expected&39));
  ++cases;
 }
 std::cout<<"PASS: "<<cases<<" Keep/Apply/Remove procedure maps on both OS architectures; no unrelated host expansion\n";
}
'''
with tempfile.TemporaryDirectory(prefix='ProcedureMap-') as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(h)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(root/'GlassSetup/src'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
