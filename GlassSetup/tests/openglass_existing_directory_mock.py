"""Compile the actual fresh OG destination gate; no filesystem mutations."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
a=s.index('            // Resources/configuration may survive')
block=s[a:s.index('\n        }\n        if(!removing)',a)]
h=r'''
#include <cassert>
#include <string>
using DWORD=unsigned;
constexpr DWORD INVALID_FILE_ATTRIBUTES=~0u,ERROR_SUCCESS=0,ERROR_FILE_NOT_FOUND=2,ERROR_PATH_NOT_FOUND=3,
 FILE_ATTRIBUTE_DIRECTORY=16,FILE_ATTRIBUTE_REPARSE_POINT=1024,ERROR_INSTALL_PACKAGE_REJECTED=1625;
DWORD attrs=16,last=0,failure=0;bool safe=true,trusted=true,receipt=false;
DWORD GetFileAttributesW(const wchar_t*){return attrs;}
DWORD GetLastError(){return last;}
void SetFailure(const wchar_t*,DWORD code){failure=code;}
namespace GlassInstallUtil{bool IsSafeInstallPath(const std::wstring&){return safe;}}
namespace GlassMaintenanceCache{bool TrustedDirectory(const std::wstring&){return trusted;}}
namespace GlassOpenGlassFiles{bool HasReceipt(const std::wstring&){return receipt;}}
bool gate(bool ru){std::wstring target=L"C:\\Program Files\\GlassPlatform\\OpenGlass";
// BLOCK
return true;}
void reset(){attrs=16;last=failure=0;safe=trusted=true;receipt=false;}
int main(){for(bool ru:{false,true}){
 reset();assert(gate(ru)); // existing empty/Resources/config directory
 for(DWORD missing:{2u,3u}){reset();attrs=INVALID_FILE_ATTRIBUTES;last=missing;assert(gate(ru));}
 reset();attrs=0;assert(!gate(ru)&&failure==1625); // file, not directory
 reset();attrs=16|1024;assert(!gate(ru)&&failure==1625);
 reset();safe=false;assert(!gate(ru));
 reset();trusted=false;assert(!gate(ru));
 reset();receipt=true;assert(!gate(ru));
 reset();attrs=INVALID_FILE_ATTRIBUTES;last=5;assert(!gate(ru)&&failure==5);
}}
'''.replace('// BLOCK',block)
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(h)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
for forbidden in ('DeleteFile', 'RemoveDirectory', 'CreateDirectory'):
 assert forbidden not in block
print('PASS: actual OG destination gate: existing safe directory allowed; missing allowed; file/reparse/unsafe/ACL/receipt/access failures refused; RU/EN; no cleanup')
