"""Actual CreateSnapshot directory/work selection with filesystem/MSI seams.
Does not test Windows ACL, MSI transactions, real copies or signatures.
"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassInstallRollback.cpp').read_text()
a=s.index('bool CreateSnapshot(');b=s.index('\nvoid NoteModified(',a);body=s[a:b]
h=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cwchar>
#include <functional>
#include <string>
#include <vector>
#include <iostream>
using DWORD=unsigned;using uint64_t=std::uint64_t;
constexpr unsigned ERROR_CANCELLED=1223,ERROR_INVALID_DATA=13,ERROR_ACCESS_DENIED=5,ERROR_FILE_NOT_FOUND=2,
 ERROR_PATH_NOT_FOUND=3,ERROR_GEN_FAILURE=31,ERROR_ALREADY_EXISTS=183,ERROR_NOT_SUPPORTED=50,
 INVALID_FILE_ATTRIBUTES=0xffffffff;constexpr unsigned MAX_PATH=260;
namespace GlassInstallRollback {
struct SessionState {unsigned productMask=0x7f,copyError=0;bool hadPriorInstall=false,productPathUnverified=false;
 std::wstring workspaceDir,x64Dir,x86Dir,openGlassDir,backupX64,backupX86,backupOpenGlass,resourcesDir,backupResources,productPathCode,productPathDetail;
 std::vector<std::wstring> oldProducts,installedThisSession,modifiedProducts;};
using CopyProgressFn=std::function<void(int)>;
}
namespace GlassInstallMsi {
constexpr const wchar_t* kProductSettingsX64=L"s64",*kProductSettingsX86=L"s32",*kProductSecureUxThemeX64=L"sut64",*kProductSecureUxThemeX86=L"sut32",
 *kProductOpenGlassResourcesX64=L"res",*kProductPlatformX64=L"p64",*kProductPlatformX86=L"p32",*kProductOpenGlass=L"og";
}
struct State {std::vector<std::wstring> copies,measured;unsigned mask=0;bool safe=true;}f;
DWORD GetLastError(){return 5;}
unsigned GetFileAttributesW(const wchar_t*){return 16;}
bool CreateDirectoryW(const wchar_t*,void*){return true;}
wchar_t* PathCombineW(wchar_t* out,const wchar_t* a,const wchar_t* b){auto path=std::wstring(a)+L"\\"+b;std::wcscpy(out,path.c_str());return out;}
namespace GlassInstallUtil {
bool IsSafeInstallPath(const std::wstring&){return f.safe;}
bool ResolveOpenGlassResourcesRoot(std::wstring& out){out=L"fixed64\\OpenGlass\\Resources";return f.safe;}
bool CreateSecureTempDirectory(const wchar_t*,std::wstring& out){out=L"workspace";return true;}
}
namespace GlassInstallLog {template<class... T>void LineFormat(const wchar_t*,T...){} }
void CleanupSnapshot(GlassInstallRollback::SessionState&){}
bool VerifyProductRegistrations(GlassInstallRollback::SessionState& s,const std::atomic_bool*){f.mask=s.productMask;return true;}
bool CaptureRegistryValues(GlassInstallRollback::SessionState&){return true;}
bool TrySnapshotProduct(GlassInstallRollback::SessionState&,const wchar_t*,const std::wstring&,const std::atomic_bool*){return true;}
bool IsNestedSubPath(const std::wstring&,const std::wstring&){return true;}
uint64_t DirBytes(const std::wstring& path,const std::wstring& ={},const std::atomic_bool* =nullptr){if(!path.empty())f.measured.push_back(path);return path.empty()?0:10;}
bool CopyTree(const std::wstring& from,const std::wstring&,const std::wstring&,uint64_t&,uint64_t,GlassInstallRollback::CopyProgressFn,unsigned&,const std::atomic_bool*){f.copies.push_back(from);return true;}
namespace GlassInstallRollback {
// BODY
}
int main(){using namespace GlassInstallRollback;
 for(unsigned mask=0;mask<128;++mask){if(mask&16)continue;f={};SessionState s;std::atomic_bool cancel{false};
  assert(CreateSnapshot(s,L"root64",L"root32",L"root64\\OpenGlass",false,nullptr,&cancel,mask));
  std::vector<std::wstring> expected;
  if(mask&5)expected.push_back(mask&1?L"root64":L"root64\\GlassSettings");
  if(mask&34)expected.push_back(mask&2?L"root32":L"root32\\GlassSettings");
  if(mask&8)expected.push_back(L"root64\\OpenGlass");
  if(mask&64)expected.push_back(L"fixed64\\OpenGlass\\Resources");
  assert(f.copies==expected&&f.measured==expected&&f.mask==mask);
  assert(s.backupResources.empty()==!(mask&64));
  assert(s.backupX64.empty()==!(mask&5));assert(s.backupX86.empty()==!(mask&34));assert(s.backupOpenGlass.empty()==!(mask&8));
 }
 std::cout<<"PASS: actual CreateSnapshot: all 64 row unions including independent fixed Resources; only touched roots/Settings leaves measured and copied, OG independent\n";
}
'''.replace('// BODY',body)
with tempfile.TemporaryDirectory(prefix='ScopedSnapshot-') as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(h)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
