#!/usr/bin/env python3
"""Compile real RichEdit selection brush handlers and hook activation with doubles.

Covers native parameters, alpha brush bytes, DC isolation, failure cleanup and
balanced hooks. Not a Windows RichEdit renderer, real IAT or text-quality test.
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
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#define WINAPI
#define _ReturnAddress() nullptr
using BOOL=int;using BYTE=unsigned char;using DWORD=std::uint32_t;using UINT=unsigned;
using COLORREF=DWORD;using ULONG_PTR=std::uintptr_t;using ULONGLONG=std::uint64_t;
using INT=int;using LPCWSTR=const wchar_t*;using LPCSTR=const char*;using PVOID=void*;
constexpr BOOL TRUE=1,FALSE=0;
constexpr int COLOR_WINDOW=5,COLOR_BTNFACE=15,COLOR_HIGHLIGHT=13,COLOR_SCROLLBAR=0,COLOR_MENUBAR=30;
constexpr int OBJ_BRUSH=2,OBJ_BITMAP=7,OBJ_MEMDC=10,DC_BRUSH=18,BS_SOLID=0,BS_PATTERN=3,BS_HATCHED=2,DBG_NORMAL=1;
constexpr DWORD PATCOPY=0xf00021,BLACKNESS=0x42,DSTINVERT=0x550009;
struct RECT{int left,top,right,bottom;};
struct LOGBRUSH{UINT lbStyle=BS_SOLID;COLORREF lbColor=0;ULONG_PTR lbHatch=0;};
struct BITMAP{int bmBitsPixel=32;};
namespace GlassShared{enum class GlassControlKind{None,Edit,WinForms_Edit,RichEdit20W,WinForms_RichEdit20W,RichEdit50W,WinForms_RichEdit50W};}
using Kind=GlassShared::GlassControlKind;
struct Window{Kind kind=Kind::RichEdit50W;};using HWND=Window*;
struct Object{enum Type{Brush,Bitmap}type;explicit Object(Type t):type(t){}};using HGDIOBJ=Object*;
struct Brush:Object{LOGBRUSH lb;BYTE alpha;Brush(COLORREF c=0,UINT style=BS_SOLID,BYTE a=0):Object(Object::Brush),lb{style,c,0},alpha(a){}};
struct Bitmap:Object{int bpp=32;Bitmap():Object(Object::Bitmap){}};
using HBRUSH=Brush*;
inline Brush dcBrush;
inline const HGDIOBJ HGDI_ERROR=reinterpret_cast<HGDIOBJ>(std::intptr_t(-1));
using Pixel=std::array<BYTE,4>;
struct DC{HWND window=nullptr,registered=nullptr;HBRUSH brush=&dcBrush;Bitmap bitmap;COLORREF dcColor=0;RECT clip{0,0,80,60};std::array<Pixel,80*60> pixels{};};using HDC=DC*;
struct Module{std::array<int,4> refs{};std::array<bool,4> missing{};};using HMODULE=Module*;
namespace Mock{
 inline Module rich,msft,gdi,user;inline HMODULE caller=&rich;
 inline bool richLoaded=true,msftLoaded=true,gdiLoaded=true;
 inline COLORREF highlight=0xd77800;inline DWORD error=9;
 inline bool failBrush=false,failSelect=false,failNative=false,setsError=true,hotKeyClear=false;
 inline int live=0,fills=0,pats=0,blackens=0,hotKeyCalls=0;inline HWND top=nullptr;
 inline HBRUSH passed=nullptr;inline BYTE alpha=0;inline RECT rect{};inline DWORD rop=0;
 inline std::function<void()> duringNative;inline bool nested=false;
}
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(__func__)+":"+std::to_string(__LINE__)+": " #x);}while(false)
DWORD GetLastError(){return Mock::error;}void SetLastError(DWORD e){Mock::error=e;}
HWND WindowFromDC(HDC dc){return dc?dc->window:nullptr;}
int GetObjectType(HDC dc){return dc&& !dc->window?OBJ_MEMDC:3;}
HGDIOBJ GetCurrentObject(HDC dc,int type){return type==OBJ_BRUSH?static_cast<HGDIOBJ>(dc->brush):&dc->bitmap;}
HGDIOBJ GetStockObject(int){return &dcBrush;}
COLORREF GetDCBrushColor(HDC dc){return dc->dcColor;}
COLORREF GetSysColor(int index){if(index==COLOR_HIGHLIGHT)return Mock::highlight;if(index==COLOR_WINDOW)return 0xffffff;if(index==COLOR_BTNFACE)return 0xf0f0f0;return 0xaaaaaa;}
int GetObjectW(HGDIOBJ object,int size,void* out){
 if(!object||reinterpret_cast<ULONG_PTR>(object)<=COLOR_MENUBAR+1)return 0;
 if(object->type==Object::Brush){if(size!=sizeof(LOGBRUSH))return 0;*static_cast<LOGBRUSH*>(out)=static_cast<HBRUSH>(object)->lb;}
 else {if(size!=sizeof(BITMAP))return 0;static_cast<BITMAP*>(out)->bmBitsPixel=static_cast<Bitmap*>(object)->bpp;}
 SetLastError(456);return size;
}
int GetObject(HGDIOBJ object,int size,void* out){return GetObjectW(object,size,out);}
HGDIOBJ SelectObject(HDC dc,HGDIOBJ object){
 if(Mock::failSelect)return HGDI_ERROR;
 SetLastError(555);return std::exchange(dc->brush,static_cast<HBRUSH>(object));
}
void DeleteBrush(HBRUSH brush){--Mock::live;delete brush;SetLastError(444);}
namespace wil{struct Delete{void operator()(HBRUSH b)const{DeleteBrush(b);}};using unique_hbrush=std::unique_ptr<Brush,Delete>;}
namespace Shared::WinApiUtils{HBRUSH CreateSolidColorBrushWithAlpha(COLORREF c,BYTE a){if(Mock::failBrush)return nullptr;++Mock::live;SetLastError(888);return new Brush(c,BS_PATTERN,a);}}
namespace GlassShared{GlassControlKind GlassGetWindowKind(HWND w){return w?w->kind:Kind::None;}}
namespace GlassCore::PaintDispatcher{
 bool IsPaintStackDcForHwnd(HWND w,HDC dc){return dc->registered==w;}
 bool IsTopWindowDrawingSession(HWND w){return Mock::top==w;}
}
namespace GlassCore::DelphiInput{bool ClearHotKeyBackground(HWND,HDC,const RECT*,HBRUSH){++Mock::hotKeyCalls;return Mock::hotKeyClear;}}
ULONGLONG GetTickCount64(){return 1;}
template<class...T>void DebugLog(int,const wchar_t*,T...){SetLastError(666);}
void Paint(HDC dc,const RECT* rect,HBRUSH brush){
 if(!dc||!rect||!brush)return;
 COLORREF color;BYTE alpha;
 if(reinterpret_cast<ULONG_PTR>(brush)<=COLOR_MENUBAR+1){color=GetSysColor(int(reinterpret_cast<ULONG_PTR>(brush))-1);alpha=0;}
 else{color=brush==&dcBrush?dc->dcColor:brush->lb.lbColor;alpha=brush->alpha;}
 Mock::passed=brush;Mock::alpha=alpha;Mock::rect=*rect;
 const Pixel pixel{static_cast<BYTE>((((color>>16)&255)*(alpha+1))>>8),static_cast<BYTE>((((color>>8)&255)*(alpha+1))>>8),static_cast<BYTE>(((color&255)*(alpha+1))>>8),alpha};
 for(int y=std::max({0,rect->top,dc->clip.top});y<std::min({60,rect->bottom,dc->clip.bottom});++y)
  for(int x=std::max({0,rect->left,dc->clip.left});x<std::min({80,rect->right,dc->clip.right});++x)dc->pixels[y*80+x]=pixel;
}
int NativeFill(HDC dc,const RECT* rect,HBRUSH brush){
 ++Mock::fills;if(!Mock::failNative)Paint(dc,rect,brush);
 if(Mock::duringNative&&!Mock::nested){Mock::nested=true;Mock::duringNative();Mock::nested=false;}
 if(Mock::setsError)SetLastError(123);return Mock::failNative?0:7;
}
BOOL NativePat(HDC dc,int x,int y,int w,int h,DWORD rop){
 ++Mock::pats;Mock::rop=rop;
 if(rop==BLACKNESS){++Mock::blackens;return 1;}
 RECT rect{x,y,x+w,y+h};if(!Mock::failNative&&dc)Paint(dc,&rect,dc->brush);
 if(Mock::duringNative&&!Mock::nested){Mock::nested=true;Mock::duringNative();Mock::nested=false;}
 if(Mock::setsError)SetLastError(123);return Mock::failNative?0:7;
}
int FillRect(HDC dc,const RECT* r,HBRUSH b){return NativeFill(dc,r,b);}
BOOL PatBlt(HDC dc,int x,int y,int w,int h,DWORD r){return NativePat(dc,x,y,w,h,r);}
HMODULE GetModuleHandleW(const wchar_t* name){const std::wstring n=name;if(n==L"riched20.dll")return Mock::richLoaded?&Mock::rich:nullptr;if(n==L"msftedit.dll")return Mock::msftLoaded?&Mock::msft:nullptr;if(n==L"gdi32full.dll")return Mock::gdiLoaded?&Mock::gdi:nullptr;if(n==L"user32.dll")return &Mock::user;return nullptr;}
HMODULE GetCallerModule(void*){SetLastError(777);return Mock::caller;}
namespace GlassCore::Hooks{template<size_t N,size_t M>using HookDispatcherDependency=std::array<std::tuple<std::array<std::string_view,M>,const char*,void*>,N>;}
using namespace GlassCore::Hooks;using namespace std::literals;
namespace GlassCore::EditControl {
PRODUCTION_DECL
BOOL WINAPI Edit_MyExtTextOutW(HDC,int,int,UINT,const RECT*,LPCWSTR,UINT,const INT*){return 1;}
BOOL WINAPI Edit_MyExtTextOutA(HDC,int,int,UINT,const RECT*,LPCSTR,UINT,const INT*){return 1;}
bool EnableHooksOnModule(void* ptr,size_t index,bool enable){
 if(!ptr)return false;auto* m=static_cast<Module*>(ptr);if(m->missing[index])return false;
 if(enable)++m->refs[index];else if(m->refs[index])--m->refs[index];return true;
}
template<size_t Slot,class Fn>Fn GetOverallOrigSafe(HMODULE m){if(!m||!m->refs[Slot])return nullptr;return reinterpret_cast<Fn>(Slot==EHK_FILLRECT?reinterpret_cast<void*>(&NativeFill):reinterpret_cast<void*>(&NativePat));}
int GetScrollBandSizeForDpiCached(HDC){return 17;}
PRODUCTION_IMPL
}
using namespace GlassCore::EditControl;
struct Fixture{
 Window window,foreign;DC dc;Brush original;RECT r{4,5,34,25};
 explicit Fixture(Kind kind=Kind::RichEdit50W){window.kind=kind;g_editSessionStack.clear();g_editDcStack.clear();g_shutdown=false;Mock::rich={};Mock::msft={};Mock::gdi={};Mock::user={};Mock::richLoaded=Mock::msftLoaded=Mock::gdiLoaded=true;Mock::caller=&Mock::rich;Mock::failBrush=Mock::failSelect=Mock::failNative=Mock::hotKeyClear=false;Mock::setsError=true;Mock::fills=Mock::pats=Mock::blackens=Mock::hotKeyCalls=0;Mock::highlight=0xd77800;Mock::error=9;Mock::duringNative={};Mock::top=&window;dc.window=&window;dc.brush=&original;original.lb.lbColor=Mock::highlight;g_editSessionStack.push_back({&window,nullptr,window.kind});EnableEditHooksForModule(nullptr,window.kind,true);}
 ~Fixture(){g_editSessionStack.clear();g_editDcStack.clear();Mock::duringNative={};}
 int Fill(HBRUSH b=nullptr){return Edit_MyFillRect(&dc,&r,b?b:&original);}
 int Pat(DWORD rop=PATCOPY){return Edit_MyPatBlt(&dc,r.left,r.top,r.right-r.left,r.bottom-r.top,rop);}
 BYTE Alpha(int x=6,int y=7){return dc.pixels[y*80+x][3];}
};
void Routes(){
 for(Kind kind:{Kind::RichEdit20W,Kind::WinForms_RichEdit20W,Kind::RichEdit50W,Kind::WinForms_RichEdit50W})
  for(HMODULE caller:{&Mock::rich,&Mock::msft,&Mock::gdi})for(bool paint:{true,false}){
   Fixture f{kind};Mock::caller=caller;if(!paint){g_editSessionStack.clear();g_editDcStack.push_back({&f.window,caller,kind});}
   CHECK(f.Fill()==7&&f.Alpha()==255&&Mock::fills==1);CHECK(f.Pat()==7&&f.Alpha()==255&&Mock::pats==1);CHECK(f.dc.brush==&f.original&&Mock::live==0);CHECK(Mock::blackens==0);
  }
}
void Isolation(){
 for(int scenario=0;scenario<9;++scenario){Fixture f;
  switch(scenario){case 0:f.window.kind=Kind::Edit;break;case 1:f.window.kind=Kind::None;break;case 2:f.dc.window=&f.foreign;break;case 3:f.dc.window=nullptr;break;case 4:g_editSessionStack.clear();break;case 5:g_shutdown=true;break;case 6:f.dc.window=nullptr;f.dc.registered=&f.window;Mock::top=&f.foreign;break;case 7:f.dc.window=nullptr;f.dc.registered=&f.window;f.dc.bitmap.bpp=1;break;case 8:f.dc.window=nullptr;f.dc.registered=&f.foreign;break;}
  CHECK(f.Fill()==7&&Mock::passed==&f.original&&f.Alpha()==0);CHECK(f.Pat()==7&&f.Alpha()==0&&f.dc.brush==&f.original);CHECK(Mock::live==0);
 }
 {Fixture f;f.dc.window=nullptr;f.dc.registered=&f.window;CHECK(f.Fill()==7&&f.Alpha()==255);CHECK(f.Pat()==7&&f.Alpha()==255);}
}
void Brushes(){
 for(COLORREF c:{0u,0xffffffu,0xf0f0f0u,0x123456u}){Fixture f;f.original.lb.lbColor=c;CHECK(f.Fill()==7&&Mock::passed==&f.original);CHECK(f.Pat()==7&&f.dc.brush==&f.original&&f.Alpha()==0);}
 for(UINT style:{UINT(BS_PATTERN),UINT(BS_HATCHED)}){Fixture f;f.original.lb.lbStyle=style;CHECK(f.Fill()==7&&Mock::passed==&f.original&&f.Alpha()==0);CHECK(f.Pat()==7&&f.Alpha()==0);}
 {Fixture f;CHECK(f.Fill(reinterpret_cast<HBRUSH>(ULONG_PTR(COLOR_HIGHLIGHT+1)))==7&&f.Alpha()==255);}
 {Fixture f;f.dc.dcColor=Mock::highlight;CHECK(f.Fill(&dcBrush)==7&&f.Alpha()==255);f.dc.brush=&dcBrush;CHECK(f.Pat()==7&&f.Alpha()==255&&f.dc.brush==&dcBrush);}
 {Fixture f;f.original.lb.lbColor|=0xff000000u;CHECK(f.Fill()==7&&f.Alpha()==255);}
 {Fixture f;Mock::highlight=0;f.original.lb.lbColor=0;CHECK(f.Fill()==7&&f.Alpha()==255);}
 {Fixture f;CHECK(f.Pat(DSTINVERT)==7&&Mock::rop==DSTINVERT&&f.Alpha()==0);}
 // Arming RichEdit FillRect must not also arm the old scrollbar-corner policy.
 for(bool owned:{false,true}){Fixture f;f.r={4,5,21,22};f.original.lb.lbColor=GetSysColor(COLOR_SCROLLBAR);if(!owned)f.dc.window=&f.foreign;CHECK(f.Fill()==7&&Mock::passed==&f.original&&Mock::blackens==0);}
}
void FailureSemantics(){
 for(bool pat:{false,true})for(int failure=0;failure<3;++failure){Fixture f;if(failure==0)Mock::failBrush=true;if(failure==1)Mock::failNative=true;if(failure==2)Mock::failSelect=true;
  const int result=pat?f.Pat():f.Fill();CHECK(result==(failure==1?0:7));CHECK(Mock::fills+Mock::pats==1);CHECK(Mock::live==0&&f.dc.brush==&f.original);CHECK(GetLastError()==123);}
 for(bool pat:{false,true})for(int scenario=0;scenario<6;++scenario){Fixture f;Mock::setsError=false;
  switch(scenario){case 1:f.original.lb.lbColor=0x123456;break;case 2:f.dc.window=&f.foreign;break;case 3:Mock::failBrush=true;break;case 4:Mock::rich.refs.fill(0);break;case 5:g_editSessionStack.clear();break;}
  SetLastError(9);CHECK((pat?f.Pat():f.Fill())==7);CHECK(GetLastError()==9);CHECK(Mock::live==0);
 }
 {Fixture f;Mock::rich.refs.fill(0);CHECK(f.Fill()==7&&Mock::passed==&f.original);CHECK(f.Pat()==7&&f.dc.brush==&f.original);}
}
void HotKeyCompatibility(){
 for(bool clear:{false,true}){Fixture f;f.window.kind=Kind::Edit;Mock::caller=&Mock::user;EnableHooksOnModule(Mock::caller,EHK_FILLRECT,true);
  auto& s=g_editSessionStack.back();s.mod=Mock::caller;s.kind=Kind::Edit;s.hotKeyFillHook=true;s.paintDc=&f.dc;
  f.original.lb.lbColor=GetSysColor(COLOR_SCROLLBAR);f.r={4,5,21,22};Mock::hotKeyClear=clear;
  CHECK(f.Fill()==TRUE&&Mock::hotKeyCalls==1&&Mock::fills==0);CHECK(Mock::blackens==int(!clear));
 }
 // A new RichEdit module ref is not an old HotKey caller-module registration.
 {Fixture f;auto& s=g_editSessionStack.back();s.mod=&Mock::user;s.hotKeyFillHook=true;s.paintDc=&f.dc;
  Mock::hotKeyClear=true;f.original.lb.lbColor=GetSysColor(COLOR_SCROLLBAR);f.r={4,5,21,22};
  CHECK(f.Fill()==7&&Mock::hotKeyCalls==0&&Mock::blackens==0&&Mock::passed==&f.original);
 }
}
void ClipAndReentry(){
 {Fixture f;f.dc.clip={10,10,20,20};CHECK(f.Fill()==7&&f.Alpha(11,11)==255&&f.Alpha(6,7)==0);CHECK(Mock::rect.left==f.r.left&&Mock::rect.top==f.r.top&&Mock::rect.right==f.r.right&&Mock::rect.bottom==f.r.bottom);}
 {Fixture f;Mock::duringNative=[&]{CHECK(f.Pat()==7);};CHECK(f.Pat()==7);CHECK(Mock::pats==2&&Mock::live==0&&f.dc.brush==&f.original);}
}
void HookLifetimes(){
 for(Kind kind:{Kind::RichEdit20W,Kind::WinForms_RichEdit20W,Kind::RichEdit50W,Kind::WinForms_RichEdit50W}){Fixture f{kind};
  for(auto* m:{&Mock::rich,&Mock::msft,&Mock::gdi})CHECK(m->refs[EHK_FILLRECT]==1&&m->refs[EHK_PATBLT]==1);
  EnableEditHooksForModule(nullptr,kind,true);for(auto* m:{&Mock::rich,&Mock::msft,&Mock::gdi})CHECK(m->refs[EHK_FILLRECT]==2&&m->refs[EHK_PATBLT]==2);
  EnableEditHooksForModule(nullptr,kind,false);EnableEditHooksForModule(nullptr,kind,false);for(auto* m:{&Mock::rich,&Mock::msft,&Mock::gdi})CHECK(m->refs[EHK_FILLRECT]==0&&m->refs[EHK_PATBLT]==0);
  CHECK(Mock::msft.refs[EHK_EXTTEXTOUT]==0&&Mock::msft.refs[EHK_EXTTEXTOUTA]==0); // accepted text routing is not broadened
 }
 {Fixture f;Mock::rich={};Mock::msft={};Mock::gdi={};EnableEditHooksForModule(nullptr,Kind::Edit,true);for(auto* m:{&Mock::rich,&Mock::msft,&Mock::gdi})CHECK(!m->refs[EHK_FILLRECT]&&!m->refs[EHK_PATBLT]);}
 {Fixture f;Mock::rich={};Mock::msft={};Mock::gdi={};Mock::msftLoaded=false;Mock::rich.missing[EHK_PATBLT]=true;EnableEditHooksForModule(nullptr,Kind::RichEdit50W,true);CHECK(Mock::msft.refs[EHK_FILLRECT]==0&&Mock::rich.refs[EHK_PATBLT]==0&&Mock::gdi.refs[EHK_PATBLT]==1);EnableEditHooksForModule(nullptr,Kind::RichEdit50W,false);CHECK(Mock::gdi.refs[EHK_PATBLT]==0);}
}
int main(int argc,char** argv){try{CHECK(argc==2);std::string c=argv[1];if(c=="routes")Routes();else if(c=="isolation")Isolation();else if(c=="brushes")Brushes();else if(c=="failure")FailureSemantics();else if(c=="hotkey")HotKeyCompatibility();else if(c=="clip")ClipAndReentry();else if(c=="hooks")HookLifetimes();else return 2;CHECK(Mock::live==0);std::cout<<"PASS "<<c<<'\n';return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
'''


class RichSelectionBrushTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='rich-selection-brush-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        source = (ROOT / 'GlassCore/src/Hooks/EditHooks.cpp').read_text(encoding='utf-8')
        session = '    struct EditPaintSession' + source.split('    struct EditPaintSession', 1)[1].split('    [[nodiscard]] static bool IsDelphiMemo', 1)[0]
        decl = '    BOOL WINAPI Edit_MyExtTextOutW(HDC,' + source.split('    BOOL WINAPI Edit_MyExtTextOutW(HDC,', 1)[1].split('    using EditDispatcher', 1)[0]
        kind = '    static bool IsRichEditKind(' + source.rsplit('    static bool IsRichEditKind(', 1)[1].split('    // ── Канал 1:', 1)[0]
        get_session = '    static HWND GetSessionHwnd() noexcept' + source.split('    static HWND GetSessionHwnd() noexcept', 1)[1].split('    // Резолв HWND', 1)[0]
        handlers = '    [[nodiscard]] static HWND ResolveRichEditSelectionDc(' + source.split('    [[nodiscard]] static HWND ResolveRichEditSelectionDc(', 1)[1].split('    BOOL WINAPI Edit_MyExtTextOutA(\n', 1)[0]
        enable = '    static void EnableEditHooksForModule(' + source.split('    static void EnableEditHooksForModule(', 1)[1].split('    // =========================================================================\n    //  PaintDispatcher provider callbacks', 1)[0]
        cpp = tmp / 'test.cpp'
        cpp.write_text(CPP.replace('PRODUCTION_DECL', session + decl).replace('PRODUCTION_IMPL', kind + get_session + handlers + enable), encoding='utf-8')
        cls.binaries = []
        for debug in (False, True):
            binary = tmp / ('debug' if debug else 'release')
            command = compiler + ['-std=c++20', '-Wall', '-Wextra', '-Werror',
                                  '-Wno-sign-compare', '-Wno-unused-variable', '-Wno-unused-parameter',
                                  '-Wno-misleading-indentation', str(cpp), '-o', str(binary)]
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

    def test_native_vcl_and_winforms_paint_getdc_and_both_rich_dlls(self):
        self.run_case('routes')

    def test_plain_memo_foreign_stale_unregistered_and_monochrome_dcs_are_native(self):
        self.run_case('isolation')

    def test_highlight_system_dc_brushes_and_other_colours_rops_patterns(self):
        self.run_case('brushes')

    def test_allocation_selection_and_native_failures_preserve_state_result_error(self):
        self.run_case('failure')

    def test_hotkey_background_and_legacy_corner_keep_their_original_module_scope(self):
        self.run_case('hotkey')

    def test_native_geometry_clip_and_nested_fill_keep_brush_lifetime(self):
        self.run_case('clip')

    def test_nested_refs_missing_slots_and_plain_edits_do_not_arm_selection_hooks(self):
        self.run_case('hooks')


if __name__ == '__main__':
    unittest.main()
