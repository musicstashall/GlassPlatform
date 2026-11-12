"""Compile actual Resources ARP adapter with fake registry/MSI/cache APIs."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassInstallMsi.cpp').read_text()
a=s.index('static bool PatchResourcesMaintenance()')
fn=s[a:s.index('// PublishProduct',a)]
h=r'''
#include <string>
#include <map>
#include <cassert>
#include <cwchar>
using DWORD=unsigned;using UINT=unsigned;using BYTE=unsigned char;using HKEY=void*;
constexpr unsigned MAX_PATH=260,ERROR_SUCCESS=0,ERROR_FILE_NOT_FOUND=2,KEY_SET_VALUE=2,KEY_WOW64_64KEY=256,REG_SZ=1,REG_DWORD=4;
const HKEY HKEY_LOCAL_MACHINE=reinterpret_cast<HKEY>(1);
const wchar_t* kProductOpenGlassResourcesX64=L"{6D6046DB-6B28-4F2C-8A7D-EB6CF13CBBD9}";
enum class ProductPathCheck {Safe,Unsafe};
bool installed=true,safe=true,available=true,open=true,writes=true;int handles=0;
std::map<std::wstring,std::wstring> values;
bool IsProductInstalled(const wchar_t*){return installed;}
ProductPathCheck CheckProductContexts(const wchar_t*,std::wstring&){return safe?ProductPathCheck::Safe:ProductPathCheck::Unsafe;}
namespace GlassMaintenanceCache {std::wstring CurrentFile(const wchar_t*){return available?L"C:\\Protected\\GlassSetup.exe":L"";}}
namespace GlassInstallLog {template<class... T>void LineFormat(const wchar_t*,T...){} }
UINT GetSystemDirectoryW(wchar_t* dst,UINT){std::wcscpy(dst,L"C:\\Windows\\System32");return 19;}
int RegOpenKeyExW(HKEY,const wchar_t*,unsigned,unsigned,HKEY* out){if(!open)return 5;*out=reinterpret_cast<HKEY>(2);++handles;return 0;}
int RegSetValueExW(HKEY,const wchar_t* key,unsigned,unsigned type,const BYTE* value,DWORD){
 if(!writes)return 5;
 if(type==REG_SZ)values[key]=reinterpret_cast<const wchar_t*>(value);
 else {values[key]=std::to_wstring(*reinterpret_cast<const DWORD*>(value));}
 return 0;
}
int RegDeleteValueW(HKEY,const wchar_t* key){values.erase(key);return 0;}
void RegCloseKey(HKEY){--handles;}
// FN
void reset(){installed=safe=available=open=writes=true;values.clear();}
int main(){reset();values[L"WindowsInstaller"]=L"1";assert(PatchResourcesMaintenance()&&handles==0);
 assert(values[L"ModifyPath"]==L"\"C:\\Protected\\GlassSetup.exe\" --modify --product \"{6D6046DB-6B28-4F2C-8A7D-EB6CF13CBBD9}\"");
 assert(values[L"UninstallString"].find(L"msiexec.exe\" /x")!=std::wstring::npos);
 assert(values[L"QuietUninstallString"].find(L"/qn /norestart")!=std::wstring::npos);
 assert(values[L"NoModify"]==L"0"&&!values.count(L"WindowsInstaller"));
 for(bool* flag:{&installed,&safe,&available,&open,&writes}){reset();*flag=false;assert(!PatchResourcesMaintenance()&&handles==0);}
}
'''.replace('// FN',fn)
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(h)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
refresh=s[s.index('bool RefreshManagedArp('):s.index('DWORD CALLBACK SnapshotCopyProgress')]
assert 'PatchResourcesMaintenance()' in refresh
print('PASS: actual Resources Change -> trusted Setup, uninstall -> MSI, safe contexts and failure/handle gates')
