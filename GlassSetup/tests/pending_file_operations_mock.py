"""Actual conservative reboot guard, fake read-only registry; no Windows/MSI IO."""
from pathlib import Path
import shutil, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassInstallUtil.cpp').read_text()
body=s[s.index('bool QueryPendingFileOperations('):s.index('bool QuerySecureUxThemeState(')]
harness=r'''
#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
using DWORD=unsigned;using LONG=long;
constexpr int HKEY_LOCAL_MACHINE=1,RRF_RT_REG_MULTI_SZ=32,ERROR_SUCCESS=0,ERROR_FILE_NOT_FOUND=2,ERROR_ACCESS_DENIED=5;
LONG first=0,second=0;int calls=0;DWORD sizeOverride=0;bool overrideSize=false;
std::vector<wchar_t> data;
LONG RegGetValueW(int,const wchar_t*,const wchar_t*,int,void*,void* out,DWORD* bytes){
 ++calls;if(!out){*bytes=overrideSize?sizeOverride:DWORD(data.size()*sizeof(wchar_t));return first;}
 if(second)return second;
 const DWORD size=DWORD(data.size()*sizeof(wchar_t));if(*bytes<size)return 234;
 if(size){std::memcpy(out,data.data(),size);}
 *bytes=size;return 0;
}
// BODY
void reset(){first=second=0;overrideSize=false;calls=0;data={0,0};}
int main(){bool pending=false;
 reset();assert(QueryPendingFileOperations(pending)&&!pending&&calls==2);
 reset();first=ERROR_FILE_NOT_FOUND;assert(QueryPendingFileOperations(pending)&&!pending&&calls==1);
 reset();data={L'a',0,0,0};assert(QueryPendingFileOperations(pending)&&pending);
 reset();first=ERROR_ACCESS_DENIED;assert(!QueryPendingFileOperations(pending)&&pending);
 reset();second=ERROR_ACCESS_DENIED;assert(!QueryPendingFileOperations(pending)&&pending);
 for(unsigned size:{0u,1u,3u,1048577u}){reset();overrideSize=true;sizeOverride=size;assert(!QueryPendingFileOperations(pending)&&pending&&calls==1);}
 reset();data={L'a',L'b'};assert(!QueryPendingFileOperations(pending)&&pending);
 reset();data={L'a',0,L'b',0,0};assert(QueryPendingFileOperations(pending)&&pending);
 std::cout<<"PASS: actual read-only reboot guard: absent, empty, queued operations, malformed/type/access/size/race refusal\n";
}
'''.replace('// BODY',body)
compiler=shutil.which('clang++') or shutil.which('g++')
if not compiler:raise SystemExit('C++17 compiler required')
with tempfile.TemporaryDirectory(prefix='GlassPendingRename-') as directory:
 p=Path(directory)/'test.cpp';exe=Path(directory)/'test';p.write_text(harness)
 subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pedantic',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
