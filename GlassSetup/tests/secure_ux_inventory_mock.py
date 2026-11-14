"""Compile the actual shared MSI inventory with fake read-only MSI APIs.
No Windows registry, MSI transaction, or System32 filesystem is exercised.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'GlassShared/include/SecureUxThemeInventory.h').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith(('#pragma', '#include <windows.h>', '#include <msi.h>')))
harness = r'''
#include <cassert>
#include <cwchar>
#include <map>
#include <string>
#include <vector>
#include <iostream>
using UINT=unsigned; using DWORD=unsigned; using INSTALLSTATE=int;
constexpr UINT ERROR_SUCCESS=0,ERROR_NO_MORE_ITEMS=259,ERROR_ACCESS_DENIED=5,
    ERROR_NOT_ENOUGH_MEMORY=8,ERROR_BAD_CONFIGURATION=1610;
constexpr INSTALLSTATE INSTALLSTATE_DEFAULT=5,INSTALLSTATE_ABSENT=2,
    INSTALLSTATE_ADVERTISED=1,INSTALLSTATE_UNKNOWN=-1,INSTALLSTATE_BADCONFIG=-6;
#define _wcsicmp wcscasecmp
std::map<std::wstring,std::vector<std::wstring>> families;
std::map<std::wstring,INSTALLSTATE> states;
UINT enumerationError=0;
UINT MsiEnumRelatedProductsW(const wchar_t* family,DWORD,DWORD index,wchar_t* product){
 if(enumerationError)return enumerationError;
 const auto& list=families[family];if(index>=list.size())return ERROR_NO_MORE_ITEMS;
 std::wcscpy(product,list[index].c_str());return 0;
}
INSTALLSTATE MsiQueryProductStateW(const wchar_t* product){auto it=states.find(product);return it==states.end()?INSTALLSTATE_UNKNOWN:it->second;}
// SOURCE
int main(){
 using namespace Shared::SecureUxThemeInventory;
 const wchar_t* f=L"{965B5B74-4040-4E89-947F-AF8D99A76706}";
 const wchar_t* g=L"{3FF010A5-2E94-4621-A260-3A7ABCDDB37C}";
 std::vector<std::wstring> products={L"previous result"};
 assert(Query(products)==0&&products.empty());assert(!IsInstalled());
 families[f]={L"stale",L"advertised"};
 states={{L"stale",INSTALLSTATE_UNKNOWN},{L"advertised",INSTALLSTATE_ADVERTISED}};
 assert(Query(products)==0&&products.empty());assert(!IsInstalled());
 families[f].push_back(L"installed");states[L"installed"]=INSTALLSTATE_DEFAULT;
 families[g]={L"installed"};
 assert(Query(products)==0&&products==std::vector<std::wstring>{L"installed"});assert(IsInstalled());
 families[g]={L"foreign"};states[L"foreign"]=INSTALLSTATE_ABSENT;
 assert(Query(products)==0&&products.size()==2);assert(IsInstalled());
 assert(QueryInstalled(products)==0&&products==std::vector<std::wstring>{L"installed"});
 families[f].clear();
 assert(Query(products)==0&&products==std::vector<std::wstring>{L"foreign"});
 assert(QueryInstalled(products)==0&&products.empty());assert(!IsInstalled());
 families[g]={L"broken"};states[L"broken"]=INSTALLSTATE_BADCONFIG;
 assert(Query(products)==ERROR_BAD_CONFIGURATION&&products.empty());
 enumerationError=ERROR_ACCESS_DENIED;products={L"previous result"};
 assert(Query(products)==ERROR_ACCESS_DENIED&&products.empty());
 std::cout<<"PASS: actual inventory: absent, stale, advertised, installed, duplicate, foreign, broken, enumeration failure\n";
}
'''.replace('// SOURCE', source)
compiler = shutil.which('clang++') or shutil.which('g++')
if not compiler:
    raise SystemExit('clang++ or g++ required')
with tempfile.TemporaryDirectory(prefix='GlassSutInventory-') as directory:
    path=Path(directory)/'inventory.cpp'
    exe=Path(directory)/'inventory'
    path.write_text(harness)
    subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-pedantic',str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
