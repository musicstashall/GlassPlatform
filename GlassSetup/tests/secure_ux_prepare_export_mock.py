"""Actual V2 export returns real HRESULTs; keeps native-bitness and vendor guards."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSettings/Native/SecureUxThemeNative.cpp').read_text()
a=s.index('extern "C" void CALLBACK GlassSettings_SecureUxThemePrepareRemovalV2W(');b=s.index('// Небольшие протоколы',a)
h=r'''
#include <cassert>
#include <csetjmp>
#include <iostream>
using DWORD=unsigned;using UINT=unsigned;using HRESULT=int;using HWND=void*;using HINSTANCE=void*;using LPWSTR=wchar_t*;
#define CALLBACK
#define FAILED(x) ((x)<0)
#define HRESULT_FROM_WIN32(x) (-int(x))
constexpr HRESULT S_OK=0,E_INVALIDARG=-22;constexpr int ERROR_INSTALL_PLATFORM_UNSUPPORTED=1633,ERROR_NOT_SUPPORTED=50,ERROR_INVALID_STATE=5023,DBG_NORMAL=0;
struct GlassSettings_SecureUxThemeNativeQuery {bool WinlogonHasOurToken=false,LoadedEventPresent=false,ComProxyHijacked=false,SystemDll4Present=false;}observed;
bool wow=false,legacy=false;HRESULT queried=0;int calls=0;
bool IsWow64ProcessNow(){return wow;}
bool HasUnsupportedLegacySecureUxTheme(const GlassSettings_SecureUxThemeNativeQuery&){return legacy;}
HRESULT GlassSettings_SecureUxThemeQuery(GlassSettings_SecureUxThemeNativeQuery* out){*out=observed;return queried;}
namespace NativeStockVisualStyle {
 struct Result {HRESULT comHr=0,fontHr=0,readHr=0;bool verified=false;};
 HRESULT hr=0;bool verified=true;
 HRESULT Apply(Result& r){++calls;r.verified=verified;return hr;}
}
template<class... T>void DebugLog(int,const wchar_t*,T...){}
UINT code=99;std::jmp_buf target;
[[noreturn]]void ExitProcess(UINT value){code=value;std::longjmp(target,1);}
// SOURCE
UINT run(LPWSTR args=nullptr){if(setjmp(target)==0)GlassSettings_SecureUxThemePrepareRemovalV2W(nullptr,nullptr,args,0);return code;}
int main(){
 assert(run()==0&&calls==0);observed.LoadedEventPresent=true;assert(run()==0&&calls==1);
 NativeStockVisualStyle::hr=-1053;assert(run()==static_cast<UINT>(-1053));
 NativeStockVisualStyle::hr=0;NativeStockVisualStyle::verified=false;assert(run()==static_cast<UINT>(-ERROR_INVALID_STATE));
 int old=calls;wow=true;assert(run()==static_cast<UINT>(-ERROR_INSTALL_PLATFORM_UNSUPPORTED)&&calls==old);wow=false;
 queried=-75;assert(run()==static_cast<UINT>(-75)&&calls==old);queried=0;
 legacy=true;assert(run()==static_cast<UINT>(-ERROR_NOT_SUPPORTED)&&calls==old);legacy=false;
 wchar_t args[]=L"bad";assert(run(args)==static_cast<UINT>(E_INVALIDARG)&&calls==old);
 std::cout<<"PASS: actual V2 preparation export: no V1 0xC collapse, HRESULT/readback errors, inactive no-op, WOW64/legacy/query/argument guards\n";
}
'''.replace('// SOURCE',s[a:b])
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
