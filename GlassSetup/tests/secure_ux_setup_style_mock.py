"""Setup-owned actual style switch: no installed Native or V2 export required."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassInstallUtil.cpp').read_text()
a=s.index('bool PrepareSecureUxRemovalVisualStyle(');b=s.index('bool QuerySecureUxPendingFileOperations(',a);method=s[a:b]
engine=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
assert 'SecureUxRemovalStyleHelperAvailable' not in engine
assert 'style-only removal helper V2 is required' not in engine
assert 'InstalledSecureUxThemeNativeDll' not in method and 'RunHiddenCommand' not in method
assert '#include <Helpers/ThemeHelper.h>' in s
assert (root/'GlassShared/include/Helpers/ThemeHelper.h').is_file()
h=r'''
#include <cassert>
#include <cwchar>
#include <string>
#include <iostream>
using UINT=unsigned;using DWORD=unsigned;using HRESULT=long;
constexpr DWORD INVALID_FILE_ATTRIBUTES=~0u,FILE_ATTRIBUTE_DIRECTORY=16,FILE_ATTRIBUTE_REPARSE_POINT=1024,RRF_RT_REG_DWORD=1;
constexpr int HKEY_CURRENT_USER=1;
#define SUCCEEDED(hr) ((hr)>=0)
#define _wcsicmp wcscasecmp
bool windowsOk=true,themeRead=true,wrongStyle=false;DWORD attr=0,fonts=0;HRESULT result=0;int calls=0,flags=-1;
UINT GetWindowsDirectoryW(wchar_t* out,UINT){std::wcscpy(out,L"C:\\Windows");return windowsOk?10:0;}
DWORD GetFileAttributesW(const wchar_t*){return attr;}
int RegGetValueW(int,const wchar_t*,const wchar_t*,int,void*,DWORD* out,DWORD*){*out=fonts;return 0;}
namespace Shared::ThemeHelper {HRESULT SetSystemVisualStyle(const wchar_t* path,const wchar_t* color,const wchar_t* size,int f){++calls;flags=f;assert(std::wstring(path)==L"C:\\Windows\\Resources\\Themes\\Aero\\aero.msstyles"&&std::wstring(color)==L"NormalColor"&&std::wstring(size)==L"NormalSize");return result;}}
HRESULT GetCurrentThemeName(wchar_t* out,int,wchar_t*,int,wchar_t*,int){std::wcscpy(out,wrongStyle?L"C:\\Custom\\aero.msstyles":L"C:\\Windows\\Resources\\Themes\\Aero\\aero.msstyles");return themeRead?0:-1;}
namespace GlassInstallLog {template<class... T>void LineFormat(const wchar_t*,T...) {}}
struct SecureUxThemeDeactivateRun {bool ran=false;};
// METHOD
int main(){std::wstring detail;SecureUxThemeDeactivateRun run;
 for(int fail=0;fail<7;++fail){windowsOk=themeRead=true;wrongStyle=false;attr=0;result=0;calls=0;run.ran=true;
  if(fail==0)windowsOk=false;if(fail==1)attr=INVALID_FILE_ATTRIBUTES;if(fail==2)attr=FILE_ATTRIBUTE_DIRECTORY;if(fail==3)attr=FILE_ATTRIBUTE_REPARSE_POINT;
  if(fail==4)result=-1;if(fail==5)themeRead=false;if(fail==6)wrongStyle=true;
  assert(!PrepareSecureUxRemovalVisualStyle(L"missing Settings",L"",&run,&detail));assert(!run.ran);assert(calls==(fail>=4?1:0));assert(!detail.empty());
 }
 windowsOk=themeRead=true;wrongStyle=false;attr=0;result=0;
 for(DWORD font:{0u,1u}){fonts=font;assert(PrepareSecureUxRemovalVisualStyle(L"",L"",&run,&detail));assert(run.ran&&flags==(font?65:0));}
 std::cout<<"PASS: Setup-owned stock-style switch without Settings/V2; Windows path, font parity, apply/readback errors and partial result\n";
}
'''.replace('// METHOD',method)
with tempfile.TemporaryDirectory(prefix='SetupStyle-') as directory:
 p=Path(directory)/'test.cpp';exe=Path(directory)/'test';p.write_text(h)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
