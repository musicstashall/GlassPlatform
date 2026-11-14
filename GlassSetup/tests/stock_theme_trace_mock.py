"""Exercise actual fixed-width x86/x64 diagnostic mapping transport."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassShared/include/StockThemeMaintenanceTrace.h').read_text()
s='\n'.join(x for x in s.splitlines() if not x.startswith(('#include','#pragma')))
h=r'''#include <cassert>
#include <string>
#include <iostream>
#include <cstdint>
using DWORD=uint32_t;using HRESULT=int32_t;using HANDLE=void*;
constexpr HRESULT E_PENDING=-1;
constexpr int FILE_MAP_WRITE=2,FILE_MAP_ALL_ACCESS=3,FALSE=0,PAGE_READWRITE=4,ERROR_ALREADY_EXISTS=183;
HANDLE INVALID_HANDLE_VALUE=reinterpret_cast<void*>(-1);
alignas(8) unsigned char bytes[32];bool failCreate=false,failMap=false,exists=false;int handles=0,views=0;
DWORD GetCurrentProcessId(){return 123;}uint64_t GetTickCount64(){return 456;}
DWORD GetLastError(){return exists?183:0;}
HANDLE CreateFileMappingW(HANDLE,void*,int,int,size_t size,const wchar_t*){assert(size==32);if(failCreate)return nullptr;++handles;return bytes;}
HANDLE OpenFileMappingW(int,int,const wchar_t*){++handles;return bytes;}
void* MapViewOfFile(HANDLE,int,int,int,size_t size){assert(size==32);if(failMap)return nullptr;++views;return bytes;}
void UnmapViewOfFile(void*){--views;}void CloseHandle(HANDLE){--handles;}
// SOURCE
int main(){using namespace StockThemeMaintenanceTrace;
 assert(!ValidName(nullptr)&&!ValidName(L"bad")&&!ValidName(L"Local\\GlassStockTheme-")&&!ValidName(L"Local\\GlassStockTheme-1 x"));
 assert(ValidName(L"Local\\GlassStockTheme-123-456"));
 {Reader r;assert(std::wstring(r.Name())==L"Local\\GlassStockTheme-123-456"&&!r.Result());
  {Writer w(r.Name());assert(w.Ready());Data d;d.active=1;d.hr=-50;d.comHr=-51;d.verified=0;w.Complete(d);}
  assert(r.Result()&&r.Result()->hr==-50&&r.Result()->comHr==-51&&r.Result()->active==1);
 }
 assert(handles==0&&views==0);
 for(int failure=0;failure<3;++failure){failCreate=failure==0;failMap=failure==1;exists=failure==2;
  {Reader r;assert(!*r.Name()&&!r.Result());}assert(handles==0&&views==0);
 }
 std::cout<<"PASS: actual stock-theme trace: cross-bitness layout, name validation, read/write, missing result, mapping failures/collision and cleanup\n";
}
'''.replace('// SOURCE',s)
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
