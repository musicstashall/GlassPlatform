#!/usr/bin/env python3
"""Compile the complete TInputIcons.cpp with portable Win32/IAT doubles.

Checks routing, native parameters, buffered-paint ownership and failure paths;
this is not a Windows ImageList renderer or visual/alpha-pixel validation.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CPP = r'''
#include <array>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#define WINAPI
#define _countof(a) (sizeof(a)/sizeof((a)[0]))
#define _wcsicmp wcscasecmp
using BOOL=int; using UINT=unsigned; using DWORD=std::uint32_t; using LONG=std::int32_t;
using COLORREF=DWORD; using HRESULT=std::int32_t; using BYTE=unsigned char; using ULONGLONG=std::uint64_t;
using LPCWSTR=const wchar_t*; using HMODULE=void*; using FARPROC=void(*)();
constexpr BOOL TRUE=1,FALSE=0; constexpr HRESULT S_OK=0,E_FAIL=-1;
bool SUCCEEDED(HRESULT h){return h>=0;} bool FAILED(HRESULT h){return h<0;}
constexpr UINT ILD_TRANSPARENT=1,ILD_MASK=0x10,ILD_ROP=0x40,ILD_PRESERVEALPHA=0x1000,ILD_DPISCALE=0x4000;
constexpr int MM_TEXT=1,OBJ_MEMDC=10,OBJ_BITMAP=7,BPBF_TOPDOWNDIB=2,DBG_NORMAL=1;
constexpr DWORD GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS=4,GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT=2;
constexpr BYTE AC_SRC_OVER=0,AC_SRC_ALPHA=1;
struct RECT{LONG left,top,right,bottom;};
struct BLENDFUNCTION{BYTE BlendOp,BlendFlags,SourceConstantAlpha,AlphaFormat;};
struct BP_PAINTPARAMS{DWORD cbSize,dwFlags;const RECT* prcExclude;const BLENDFUNCTION* pBlendFunction;};
namespace GlassShared { enum class GlassControlKind{None,Edit}; }
struct Window{const wchar_t* cls;Window* parent=nullptr;GlassShared::GlassControlKind kind=GlassShared::GlassControlKind::Edit;bool backdrop=true;};
using HWND=Window*;
struct DC{HWND window=nullptr,registered=nullptr;bool top=true;int bpp=32,map=MM_TEXT;};
using HDC=DC*;using HGDIOBJ=DC*;
struct BITMAP{int bmBitsPixel=32;};
struct Images{int width=16,height=18;};using HIMAGELIST=Images*;
using HPAINTBUFFER=void*;
struct RGBQUAD{BYTE rgbBlue=0,rgbGreen=0,rgbRed=0,rgbReserved=0;};
struct IMAGELISTDRAWPARAMS{DWORD cbSize;HIMAGELIST himl;int i;HDC hdcDst;int x,y,cx,cy,xBitmap,yBitmap;COLORREF rgbBk,rgbFg;UINT fStyle;DWORD dwRop,fState,Frame,crEffect;};
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
namespace Mock {
 bool full=true,failSize=false,failModule=false,failBegin=false,failMemory=false,failClear=false,failEnd=false,failNative=false,failBits=false,failFlush=false,noAlpha=false;
 DWORD error=9;int calls=0,begins=0,ends=0,live=0;BOOL update=0;UINT flags=0;HDC target=nullptr;
 RECT bounds{};BLENDFUNCTION blend{};IMAGELISTDRAWPARAMS indirect{};int index=-1,x=0,y=0,width=0,height=0,tag=0;
 COLORREF background=0,foreground=0;std::function<void()> duringNative;bool reentered=false;
 DC memory;int missing=-1;std::vector<RGBQUAD> pixels;
}
DWORD GetLastError(){return Mock::error;}void SetLastError(DWORD e){Mock::error=e;}
HWND WindowFromDC(HDC dc){return dc?dc->window:nullptr;}
int GetMapMode(HDC dc){return dc->map;}
int GetObjectType(HDC dc){return dc&& !dc->window?OBJ_MEMDC:3;}
int GetClassNameW(HWND w,wchar_t* out,int size){if(!w||std::wcslen(w->cls)>=unsigned(size))return 0;std::wcscpy(out,w->cls);return std::wcslen(out);}
HWND GetParent(HWND w){return w?w->parent:nullptr;}
HGDIOBJ GetCurrentObject(HDC dc,int){return dc;}
int GetObjectW(HGDIOBJ dc,int size,void* out){static_cast<BITMAP*>(out)->bmBitsPixel=dc->bpp;return size;}
BOOL ImageList_GetIconSize(HIMAGELIST images,int* w,int* h){if(Mock::failSize)return FALSE;*w=images->width;*h=images->height;return TRUE;}
BOOL Record(int tag,int index,HDC dc,int x,int y,int w,int h,COLORREF bg,COLORREF fg,UINT flags){
 ++Mock::calls;Mock::tag=tag;Mock::index=index;Mock::target=dc;Mock::flags=flags;Mock::x=x;Mock::y=y;Mock::width=w;Mock::height=h;Mock::background=bg;Mock::foreground=fg;
 if(Mock::duringNative&&!Mock::reentered){Mock::reentered=true;Mock::duringNative();Mock::reentered=false;}
 if(dc==&Mock::memory&&!Mock::pixels.empty()&&!Mock::noAlpha)Mock::pixels[0].rgbReserved=255; // opaque BLACK glyph
 SetLastError(123);return !Mock::failNative;
}
BOOL ImageList_Draw(HIMAGELIST,int index,HDC dc,int x,int y,UINT flags){return Record(0,index,dc,x,y,0,0,0,0,flags);}
BOOL ImageList_DrawEx(HIMAGELIST,int index,HDC dc,int x,int y,int w,int h,COLORREF bg,COLORREF fg,UINT flags){return Record(1,index,dc,x,y,w,h,bg,fg,flags);}
BOOL ImageList_DrawIndirect(IMAGELISTDRAWPARAMS* p){if(!p)return FALSE;Mock::indirect=*p;return Record(2,p->i,p->hdcDst,p->x,p->y,p->cx,p->cy,p->rgbBk,p->rgbFg,p->fStyle);}
BOOL GetModuleHandleExW(DWORD,LPCWSTR,HMODULE* out){*out=reinterpret_cast<void*>(1);return !Mock::failModule;}
FARPROC GetProcAddress(HMODULE,const char* name){return std::strcmp(name,"ImageList_GetIconSize")==0?reinterpret_cast<FARPROC>(&ImageList_GetIconSize):nullptr;}
HPAINTBUFFER BeginBufferedPaint(HDC,const RECT* r,int format,BP_PAINTPARAMS* p,HDC* memory){
 ++Mock::begins;CHECK(format==BPBF_TOPDOWNDIB);CHECK(p->dwFlags==0);Mock::bounds=*r;Mock::blend=*p->pBlendFunction;
 Mock::pixels.assign((r->right-r->left)*(r->bottom-r->top),{});
 if(Mock::failBegin)return nullptr;++Mock::live;*memory=Mock::failMemory?nullptr:&Mock::memory;return reinterpret_cast<void*>(1);
}
HRESULT BufferedPaintClear(HPAINTBUFFER,const RECT*){return Mock::failClear?E_FAIL:S_OK;}
HRESULT GetBufferedPaintBits(HPAINTBUFFER,RGBQUAD** pixels,int* stride){*pixels=Mock::pixels.data();*stride=Mock::bounds.right-Mock::bounds.left;return Mock::failBits?E_FAIL:S_OK;}
BOOL GdiFlush(){return !Mock::failFlush;}
HRESULT EndBufferedPaint(HPAINTBUFFER,BOOL update){++Mock::ends;--Mock::live;Mock::update=update;return Mock::failEnd?E_FAIL:S_OK;}
ULONGLONG GetTickCount64(){return 1;}
template<class...T>void DebugLog(int,const wchar_t*,T...){}
namespace wil {
 struct srwlock{struct Guard{~Guard(){}};Guard lock_shared(){return{};}Guard lock_exclusive(){return{};}};
 template<class F>struct Exit{F f;~Exit(){f();}};template<class F>auto scope_exit(F f){return Exit<F>{f};}
}
namespace GlassShared {
 GlassControlKind GlassGetWindowKind(HWND w){return w?w->kind:GlassControlKind::None;}
 bool GlassIsBackdropAllowedForHwnd(HWND w){return w&&w->backdrop;}
}
namespace GlassCore::Root{bool IsFullInjectionMode(){return Mock::full;}}
namespace GlassCore::PaintDispatcher{
 HWND GetStackOwnerForDc(HDC dc){return dc->registered;}
 bool IsTopWindowDrawingSession(HWND w){return w&&Mock::memory.top;}
}
namespace GlassCore::ModuleHandler{HMODULE g_entryPointModule=reinterpret_cast<void*>(2);}
namespace GlassCore::Hooks {
 template<size_t N,size_t M>using HookDispatcherDependency=std::array<std::tuple<std::array<std::string_view,M>,const char*,void*>,N>;
 template<size_t N,size_t M>struct HookDispatcher{
  const HookDispatcherDependency<N,M>& dep;void* moduleAddress;std::array<int,N> refs{};
  HookDispatcher(const HookDispatcherDependency<N,M>& d,void* m):dep(d),moduleAddress(m){}
  void CacheHookData(){}bool EnableHook(size_t i,bool enable){if(!moduleAddress||int(i)==Mock::missing)return false;refs[i]+=enable?1:-1;return true;}
  template<size_t I,class F>F GetOrg(){std::array<void*,3> fn{reinterpret_cast<void*>(&ImageList_Draw),reinterpret_cast<void*>(&ImageList_DrawEx),reinterpret_cast<void*>(&ImageList_DrawIndirect)};return refs[I]?reinterpret_cast<F>(fn[I]):nullptr;}
  void DisableAllHooks(){refs.fill(0);}
 };
}
#include "PRODUCTION"
using namespace GlassCore::DelphiInputIcons;
struct Fixture {
 Window edit{L"TButtonedEdit"},search{L"TSearchBox"},glyph{L"TGlyph",&edit},other{L"TEdit"};DC dc{&search};Images images;
 Fixture(){Shutdown();Mock::full=true;Mock::failSize=Mock::failModule=Mock::failBegin=Mock::failMemory=Mock::failClear=Mock::failEnd=Mock::failNative=Mock::failBits=Mock::failFlush=Mock::noAlpha=false;Mock::calls=Mock::begins=Mock::ends=Mock::live=0;Mock::duringNative={};Mock::memory={};Mock::missing=-1;Startup();}
 ~Fixture(){Shutdown();}
 BOOL Ex(HDC dest=nullptr,UINT style=0){return DrawEx(&images,7,dest?dest:&dc,4,5,0,0,0x112233,0x445566,style);}
 void Alpha(){CHECK(Mock::begins==1&&Mock::ends==1&&Mock::live==0&&Mock::update);CHECK(Mock::target==&Mock::memory);CHECK((Mock::flags&(ILD_TRANSPARENT|ILD_PRESERVEALPHA))==(ILD_TRANSPARENT|ILD_PRESERVEALPHA));CHECK(Mock::blend.SourceConstantAlpha==255&&Mock::blend.AlphaFormat==AC_SRC_ALPHA);CHECK(Mock::bounds.left==4&&Mock::bounds.right==20&&Mock::bounds.bottom==23);CHECK(!g_inside);}
};
void Targets(){for(int target=0;target<3;++target){Fixture f;f.dc.window=target==0?&f.search:target==1?&f.glyph:&f.edit;CHECK(f.Ex());f.Alpha();CHECK(Mock::index==7&&Mock::background==0x112233&&Mock::foreground==0x445566);CHECK(GetLastError()==123);}}
void Isolation(){
 for(int which=0;which<11;++which){Fixture f;Window fake{L"TButtonedEditExtra"},foreign{L"TGlyph",&f.other};
  switch(which){case 0:f.dc.window=&f.other;break;case 1:f.dc.window=&fake;break;case 2:f.dc.window=&foreign;break;case 3:Mock::full=false;break;case 4:f.search.backdrop=false;break;case 5:f.search.kind=GlassShared::GlassControlKind::None;break;case 6:f.dc.window=nullptr;break;case 7:f.dc.map=2;break;case 8:g_inside=true;break;case 9:f.dc.window=&f.glyph;f.edit.backdrop=false;break;case 10:f.dc.window=&f.glyph;f.glyph.backdrop=false;break;}
  CHECK(f.Ex());CHECK(Mock::begins==0&&Mock::target==&f.dc&&Mock::flags==0);g_inside=false;
 }
 {Fixture f;f.dc.window=nullptr;f.dc.registered=&f.glyph;CHECK(f.Ex());f.Alpha();}
 {Fixture f;f.dc.window=nullptr;f.dc.registered=&f.glyph;Mock::memory.top=false;CHECK(f.Ex());CHECK(Mock::begins==0);}
 {Fixture f;f.dc.window=nullptr;f.dc.registered=&f.glyph;f.dc.bpp=1;CHECK(f.Ex());CHECK(Mock::begins==0);}
}
void Params(){
 {Fixture f;CHECK(Draw(&f.images,3,&f.dc,4,5,0x100));f.Alpha();CHECK(Mock::tag==0&&Mock::index==3&&(Mock::flags&0x100));}
 {Fixture f;IMAGELISTDRAWPARAMS p{sizeof(p),&f.images,11,&f.dc,4,5,0,0,2,3,44,55,0x100,66,77,88,99};auto before=p;CHECK(DrawIndirect(&p));f.Alpha();CHECK(p.hdcDst==before.hdcDst&&p.fStyle==before.fStyle);CHECK(Mock::indirect.i==11&&Mock::indirect.xBitmap==2&&Mock::indirect.yBitmap==3&&Mock::indirect.fState==77&&Mock::indirect.Frame==88&&Mock::indirect.crEffect==99&&Mock::indirect.dwRop==66);}
 for(UINT style:{ILD_MASK,ILD_ROP,ILD_DPISCALE}){Fixture f;CHECK(f.Ex(nullptr,style));CHECK(Mock::begins==0&&Mock::flags==style);}
 {Fixture f;CHECK(DrawEx(&f.images,1,&f.dc,4,5,-1,20,0,0,0));CHECK(Mock::begins==0);}
 {Fixture f;CHECK(DrawEx(&f.images,1,&f.dc,INT32_MAX,5,2,20,0,0,0));CHECK(Mock::begins==0);}
 {Fixture f;IMAGELISTDRAWPARAMS p{};CHECK(DrawIndirect(&p));CHECK(Mock::begins==0);CHECK(!DrawIndirect(nullptr));}
}
void Failures(){
 for(int which=0;which<9;++which){Fixture f;switch(which){case 0:Mock::failBegin=true;break;case 1:Mock::failMemory=true;break;case 2:Mock::failClear=true;break;case 3:Mock::failEnd=true;break;case 4:Mock::failSize=true;break;case 5:Mock::failModule=true;break;case 6:Mock::failBits=true;break;case 7:Mock::failFlush=true;break;case 8:Mock::noAlpha=true;break;}
  CHECK(f.Ex());CHECK(Mock::target==&f.dc&&Mock::flags==0&&Mock::live==0&&!g_inside);CHECK(Mock::calls==((which==3||which>=6)?2:1));}
 {Fixture f;Mock::failNative=true;CHECK(!f.Ex());CHECK(Mock::calls==1&&Mock::live==0&&!Mock::update&&!g_inside&&GetLastError()==123);}
}
void Reentry(){Fixture f;Mock::duringNative=[&]{CHECK(Draw(&f.images,2,&f.dc,4,5,0));CHECK(Mock::begins==1);};CHECK(f.Ex());CHECK(Mock::calls==2&&Mock::begins==1&&Mock::ends==1&&!g_inside);}
void Lifecycle(){Fixture f;Startup();for(auto r:g_dispatcher.refs)CHECK(r==1);Shutdown();for(auto r:g_dispatcher.refs)CHECK(r==0);CHECK(f.Ex());CHECK(Mock::begins==0);Mock::missing=0;Startup();CHECK(g_dispatcher.refs[0]==0&&g_dispatcher.refs[1]==1);CHECK(f.Ex());f.Alpha();}
int main(int argc,char** argv){try{CHECK(argc==2);std::string c=argv[1];if(c=="targets")Targets();else if(c=="isolation")Isolation();else if(c=="params")Params();else if(c=="failures")Failures();else if(c=="reentry")Reentry();else if(c=="lifecycle")Lifecycle();else return 2;std::cout<<"PASS "<<c<<'\n';return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
'''


class InputIconsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='input-icons-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        inc = tmp / 'include'
        for header in ('CorePch.h', 'Hooks/delphi/TInputIcons.h',
                       'Core/Intercept/HookDispatcher.h', 'Module/ModuleHandler.h',
                       'Subsystems/PaintDispatcher.h', 'Subsystems/RootSwcaGradient.h',
                       'GlassControlKind.h', 'GlassBackdropMark.h', 'CommCtrl.h', 'uxtheme.h', 'wil.h', 'DebugLog.h'):
            path = inc / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#pragma once\n', encoding='utf-8')
        cpp = tmp / 'test.cpp'
        cpp.write_text(CPP.replace('PRODUCTION', str(ROOT / 'GlassCore/src/Hooks/delphi/TInputIcons.cpp')), encoding='utf-8')
        cls.binaries = []
        for debug in (False, True):
            binary = tmp / ('debug' if debug else 'release')
            command = compiler + ['-std=c++20', '-Wall', '-Wextra', '-Werror',
                                  '-Wno-unknown-pragmas', '-Wno-sign-compare',
                                  '-Wno-misleading-indentation', '-I', str(inc), str(cpp), '-o', str(binary)]
            if debug:
                command.append('-D_DEBUG')
            result = subprocess.run(command, capture_output=True, text=True, timeout=90)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.binaries.append(binary)

    def run_case(self, name):
        for binary in self.binaries:
            with self.subTest(build=binary.name):
                result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_exact_edit_glyph_and_search_window_dc(self):
        self.run_case('targets')

    def test_foreign_unregistered_mono_and_disabled_glass_are_native(self):
        self.run_case('isolation')

    def test_all_three_apis_preserve_native_image_parameters(self):
        self.run_case('params')

    def test_buffer_and_native_failures_do_not_commit_partial_icons(self):
        self.run_case('failures')

    def test_nested_image_calls_do_not_reenter_composition(self):
        self.run_case('reentry')

    def test_missing_slots_repeated_startup_and_shutdown_are_balanced(self):
        self.run_case('lifecycle')


if __name__ == '__main__':
    unittest.main()
