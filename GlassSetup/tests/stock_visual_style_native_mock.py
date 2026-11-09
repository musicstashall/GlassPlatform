"""Execute the actual Native stock .theme pipeline with mocked Windows/COM."""
from pathlib import Path
import subprocess,tempfile,re
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSettings/Native/StockVisualStyle.h').read_text()
s='\n'.join(line for line in s.splitlines() if not line.startswith(('#include','#pragma')))
h=r'''
#include <cassert>
#include <cwchar>
#include <string>
#include <thread>
#include <iostream>
#define STDMETHODCALLTYPE
#define SUCCEEDED(x) ((x)>=0)
#define FAILED(x) ((x)<0)
#define HRESULT_FROM_WIN32(x) (-long(x))
#define _wcsicmp wcscasecmp
using DWORD_PTR=unsigned long;using HRESULT=long;using DWORD=unsigned;using UINT=unsigned;using HWND=void*;using LPCWSTR=const wchar_t*;using LPWSTR=wchar_t*;
constexpr HRESULT E_PENDING=-100,E_FAIL=-1,E_NOINTERFACE=-2,S_OK=0;
constexpr int ERROR_PATH_NOT_FOUND=3,ERROR_FILE_NOT_FOUND=2,ERROR_INVALID_STATE=5023,COINIT_APARTMENTTHREADED=2,CLSCTX_INPROC_SERVER=1,CLSCTX_LOCAL_SERVER=4,CLSCTX_REMOTE_SERVER=16,HKEY_CURRENT_USER=1,RRF_RT_REG_DWORD=1;
constexpr DWORD INVALID_FILE_ATTRIBUTES=~0u,FILE_ATTRIBUTE_DIRECTORY=16,FILE_ATTRIBUTE_REPARSE_POINT=1024;
struct GUID {unsigned a;unsigned short b,c;unsigned char d[8];};
struct IUnknown {virtual HRESULT QueryInterface(const GUID&,void**)=0;virtual unsigned AddRef()=0;virtual unsigned Release()=0;};
HRESULT apartmentHr=0,createHr=0,initHr=0,applyHr=0,fontHr=0,readHr=0,forceHr=0,currentHr=0;int currentIndex=2,forces=0,broadcasts=0;
bool windowsOk=true,missing=false,wrongPath=false;DWORD fonts=0;int comCalls=0,applies=0,fontCalls=0,uninitializes=0,releases=0,reads=0;
std::thread::id caller;
const HWND HWND_BROADCAST=reinterpret_cast<void*>(1);constexpr int WM_THEMECHANGED=10,WM_SYSCOLORCHANGE=11,SMTO_ABORTIFHUNG=2;
int SendMessageTimeoutW(HWND hwnd,int msg,int,int,int flags,int timeout,DWORD_PTR*){assert(hwnd==HWND_BROADCAST&&(msg==10||msg==11)&&flags==2&&timeout==1000&&forces==1&&reads==1);++broadcasts;return 1;}

UINT GetWindowsDirectoryW(wchar_t* p,UINT){wcscpy(p,L"C:\\Windows");return windowsOk?10:0;}
DWORD GetFileAttributesW(LPCWSTR){return missing?INVALID_FILE_ATTRIBUTES:0;}
HRESULT CoInitializeEx(void*,int flags){assert(std::this_thread::get_id()!=caller&&flags==2);return apartmentHr;}
void CoUninitialize(){++uninitializes;}
HRESULT CoCreateInstance(const GUID&,void*,int,const GUID&,void**);
int RegGetValueW(int,LPCWSTR,LPCWSTR,int,void*,DWORD* p,DWORD*){*p=fonts;return 0;}
namespace Shared::ThemeHelper {HRESULT SetSystemVisualStyle(LPCWSTR path,LPCWSTR,LPCWSTR,int flags){++fontCalls;assert(applies==1&&forces==1&&flags==65&&std::wstring(path)==L"C:\\Windows\\Resources\\Themes\\Aero\\aero.msstyles");return fontHr;}}
HRESULT GetCurrentThemeName(wchar_t* p,int,wchar_t*,int,wchar_t*,int){++reads;assert(applies==1&&forces==1);wcscpy(p,wrongPath?L"C:\\custom\\aero.msstyles":L"C:\\Windows\\Resources\\Themes\\Aero\\aero.msstyles");return readHr;}
// SOURCE
// COM MOCK
void reset(){forceHr=currentHr=0;currentIndex=2;forces=broadcasts=0;windowsOk=true;missing=wrongPath=false;fonts=0;apartmentHr=createHr=initHr=applyHr=fontHr=readHr=0;comCalls=applies=fontCalls=uninitializes=releases=reads=0;caller=std::this_thread::get_id();}
int main(){NativeStockVisualStyle::Result r;
 reset();assert(NativeStockVisualStyle::Apply(r)==0&&r.verified&&applies==1&&fontCalls==0&&uninitializes==1&&releases==1&&forces==1&&broadcasts==2);
 reset();fonts=1;assert(NativeStockVisualStyle::Apply(r)==0&&r.verified&&fontCalls==1);
 for(int fail=0;fail<8;fail++){
  reset();if(fail==0)windowsOk=false;if(fail==1)missing=true;if(fail==2)apartmentHr=-20;if(fail==3)createHr=-30;if(fail==4)initHr=-40;if(fail==5)applyHr=-50;
  if(fail==6){fonts=1;fontHr=-60;}if(fail==7)readHr=-70;
  const HRESULT hr=NativeStockVisualStyle::Apply(r);assert(hr<0&&!r.verified);
  if(fail>=2)assert(hr==-(fail*10));assert(fontCalls==(fail==6?1:0));
  assert(uninitializes==(fail>=3?1:0));assert(releases==(fail>=4?1:0));
 }
 reset();forceHr=-80;assert(NativeStockVisualStyle::Apply(r)==-80&&!r.verified&&fontCalls==0&&broadcasts==0);
 reset();currentHr=-81;assert(NativeStockVisualStyle::Apply(r)==-81&&!r.verified&&forces==0&&broadcasts==0);
 reset();currentIndex=-1;assert(NativeStockVisualStyle::Apply(r)<0&&!r.verified&&forces==0);
 reset();wrongPath=true;assert(NativeStockVisualStyle::Apply(r)<0&&!r.verified);
 std::cout<<"PASS: actual Native primary stock pipeline: dedicated STA, COM ABI/flags/.theme + forced immediate apply, broadcasts before teardown, font-only ordinal, strict API + exact-path verification, HRESULT propagation and cleanup\n";
}
'''.replace('// SOURCE',s)
mock='struct ManagerMock : NativeStockVisualStyle::Detail::ThemeManager {\n'
mock+='HRESULT QueryInterface(const GUID&,void**) override{return E_FAIL;} unsigned AddRef() override{return 1;} unsigned Release() override{++releases;return 1;}\n'
interface=s[s.index('struct ThemeManager :'):s.index('inline HRESULT ApplyOnSta')]
for name,args in re.findall(r'virtual HRESULT STDMETHODCALLTYPE (\w+)\((.*?)\)=0;',interface):
 body='return E_FAIL;'
 if name=='Init':body='return initHr;'
 if name=='GetCurrentTheme':
  args='int* out';body='*out=currentIndex;return currentHr;'
 if name=='SetCurrentTheme':
  args='HWND hwnd,int index,int applyNow,DWORD flags,DWORD pack';body='assert(applies==1&&hwnd==nullptr&&index==currentIndex&&applyNow==1&&flags==32&&pack==4);++forces;return forceHr;'
 if name=='AddAndSelectTheme':
  args='HWND hwnd,LPCWSTR path,DWORD flags,DWORD pack'
  body='assert(hwnd==nullptr&&std::wstring(path)==L"C:\\\\Windows\\\\Resources\\\\Themes\\\\aero.theme"&&flags==32&&pack==4);++applies;return applyHr;'
 mock+=f'HRESULT {name}({args}) override {{{body}}}\n'
mock+='};\nHRESULT CoCreateInstance(const GUID& cls,void*,int context,const GUID& iid,void** out){assert(cls.a==0x9324da94&&iid.a==0xc1e8c83e&&context==(CLSCTX_INPROC_SERVER|CLSCTX_LOCAL_SERVER|CLSCTX_REMOTE_SERVER));++comCalls;static ManagerMock m;if(createHr>=0)*out=&m;return createHr;}\n'
h=h.replace('// COM MOCK',mock)
# Lock down the working managed path's flags and ABI so they cannot silently diverge.
managed=(root/'GlassSettings/Services/ThemeManager2.cs').read_text()
slots=re.findall(r'virtual HRESULT STDMETHODCALLTYPE (\w+)\(',interface)
cs=managed[managed.index('private interface IThemeManager2'):managed.index('[Guid("26e4185f')]
assert slots==re.findall(r'void (\w+)\(',cs)[:len(slots)]
assert 'ThemeApplyFlags.IgnoreScreensaver,' in managed and 'ThemePackFlags.Silent);' in managed
native=(root/'GlassSettings/Native/SecureUxThemeNative.cpp').read_text()
v2=native[native.index('extern "C" void CALLBACK GlassSettings_SecureUxThemePrepareRemovalV2W'):native.index('// Небольшие протоколы')]
assert 'NativeStockVisualStyle::Apply(result)' in v2 and 'IsWow64ProcessNow()' in v2
assert 'GlassSettings_SecureUxThemeSetupV1W(hwnd' not in v2
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-pthread','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
