#!/usr/bin/env python3
"""Compile complete TInputIcons.cpp against Win32/IAT environment doubles.

Check bounded button backgrounds, native arguments/alpha pixels, probe payloads
and lifetimes in Release, Debug and Debug/probe. Not a Windows renderer, PE/IAT or MSVC test.
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
#include <cassert>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#define WINAPI
#define _countof(a) (sizeof(a)/sizeof((a)[0]))
#define _wcsicmp wcscasecmp
using BOOL=int; using UINT=unsigned; using DWORD=std::uint32_t; using LONG=std::int32_t;
using COLORREF=DWORD; using HRESULT=std::int32_t; using BYTE=unsigned char; using ULONGLONG=std::uint64_t;
using LPCWSTR=const wchar_t*; using HMODULE=void*; using FARPROC=void(*)(); using HGDIOBJ=void*;using ULONG_PTR=std::uintptr_t;
constexpr BOOL TRUE=1,FALSE=0; constexpr HRESULT S_OK=0,E_FAIL=-1;
constexpr int COLOR_WINDOW=5,COLOR_BTNFACE=15,COLOR_MENUBAR=30;
bool SUCCEEDED(HRESULT h){return h>=0;} bool FAILED(HRESULT h){return h<0;}
constexpr UINT ILD_TRANSPARENT=1,ILD_MASK=0x10,ILD_ROP=0x40,ILD_PRESERVEALPHA=0x1000,ILD_DPISCALE=0x4000,ILD_OVERLAYMASK=0xf00;
constexpr DWORD ILS_SATURATE=2,ILS_ALPHA=4;
constexpr int MM_TEXT=1,OBJ_MEMDC=10,OBJ_BITMAP=7,BPBF_TOPDOWNDIB=2,DBG_NORMAL=1;
constexpr UINT BS_SOLID=0,BS_PATTERN=3;
constexpr int GM_COMPATIBLE=1,GM_ADVANCED=2,GW_CHILD=5,GW_HWNDNEXT=2;
constexpr DWORD BLACKNESS=0x42;
constexpr DWORD GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS=4,GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT=2;
constexpr BYTE AC_SRC_OVER=0,AC_SRC_ALPHA=1;
struct RECT{LONG left{},top{},right{},bottom{};};
struct POINT{LONG x{},y{};};
struct LOGBRUSH{UINT lbStyle{};COLORREF lbColor{};std::uintptr_t lbHatch{};};using HBRUSH=LOGBRUSH*;
struct BLENDFUNCTION{BYTE BlendOp,BlendFlags,SourceConstantAlpha,AlphaFormat;};
struct BP_PAINTPARAMS{DWORD cbSize,dwFlags;const RECT* prcExclude;const BLENDFUNCTION* pBlendFunction;};
namespace GlassShared { enum class GlassControlKind{None,Edit}; }
struct Window{
 const wchar_t* cls;Window* parent=nullptr;GlassShared::GlassControlKind kind=GlassShared::GlassControlKind::Edit;bool backdrop=true;
 RECT client{0,0,102,19},outer{100,200,230,223};POINT clientOrigin{102,202};
 Window* firstChild=nullptr;Window* nextSibling=nullptr;bool visible=true;
};using HWND=Window*;
struct DC{
 HWND window=nullptr,registered=nullptr;bool top=true;int bpp=32,map=MM_TEXT;
 RECT clip{104,2,128,21};POINT origin{100,200},viewport{3,4},windowOrg{1,2};int clipKind=2;
 int graphicsMode=GM_COMPATIBLE;DWORD layout=0;
 int surfaceWidth=0,surfaceHeight=0;std::vector<DWORD> surface{};bool useHole=false;RECT hole{};
};using HDC=DC*;
struct BITMAP{int bmBitsPixel=32;};
struct RGBQUAD{BYTE rgbBlue=0,rgbGreen=0,rgbRed=0,rgbReserved=0;bool operator==(const RGBQUAD&)const=default;};
struct MaskIcon{std::vector<RGBQUAD> rgb{};std::vector<bool> solid{};};
struct Images{int width=16,height=18;bool legacy=false;std::map<int,MaskIcon> icons{};std::map<UINT,int> overlays{};};using HIMAGELIST=Images*;using HPAINTBUFFER=void*;
struct IMAGELISTDRAWPARAMS{DWORD cbSize;HIMAGELIST himl;int i;HDC hdcDst;int x,y,cx,cy,xBitmap,yBitmap;COLORREF rgbBk,rgbFg;UINT fStyle;DWORD dwRop,fState,Frame,crEffect;bool operator==(const IMAGELISTDRAWPARAMS&)const=default;};
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(__func__)+": "+#x);}while(false)
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
constexpr bool ProbeBuild=true;
#else
constexpr bool ProbeBuild=false;
#endif
namespace Mock {
 bool full=true,failSize=false,failModule=false,failBegin=false,failMemory=false,failClear=false,failEnd=false,failNative=false,failBits=false,failFlush=false,noAlpha=false;
 bool failGeometry=false,failBrushInfo=false,failProbeRegistration=false,failPat=false;
 DWORD error=9;ULONGLONG tick=1;int calls=0,begins=0,ends=0,live=0,missing=-1;
 BOOL update=0;UINT flags=0;HDC target=nullptr;RECT bounds{};BLENDFUNCTION blend{};
 IMAGELISTDRAWPARAMS indirect{};int index=-1,x=0,y=0,width=0,height=0,tag=0;
 COLORREF background=0,foreground=0;std::function<void()> duringNative,duringFill;bool reentered=false,fillReentered=false;
 DC memory,whiteMemory;int padding=0,stride=0,reportedStride=-1,whitePadding=0,whiteStride=0,whiteReportedStride=-1;
 bool failWhiteBegin=false,failWhiteMemory=false,failWhiteClear=false,failWhiteBits=false,failWhiteNative=false,poisonPadding=false,nullWhiteBits=false;
 int flushCalls=0,failFlushAt=-1,badWhite=0;DWORD whiteError=124;
 std::vector<RGBQUAD> pixels,whitePixels,drawPixels,committedPixels;
 struct NativeCall{int tag,index;HDC dc;int x,y,width,height;COLORREF background,foreground;UINT flags;IMAGELISTDRAWPARAMS indirect;};std::vector<NativeCall> nativeCalls;
 using Arg=std::variant<std::uint64_t,std::wstring>;
 struct Log{std::wstring format;std::vector<Arg> args;};std::vector<Log> logs;
 struct Fill{HDC dc;const RECT* pointer;RECT rect;HBRUSH brush;DWORD entryError;};std::vector<Fill> fills;
 struct Pat{HDC dc;RECT rect;DWORD rop;};std::vector<Pat> pats;
 int fillResult=7;DWORD fillError=55;std::map<std::string,void*> hooks;
}
DWORD GetLastError(){return Mock::error;}void SetLastError(DWORD e){Mock::error=e;}
HWND WindowFromDC(HDC dc){SetLastError(77);return dc?dc->window:nullptr;}
int GetMapMode(HDC dc){SetLastError(77);return dc->map;}
int GetObjectType(HDC dc){SetLastError(77);return dc&&!dc->window?OBJ_MEMDC:3;}
int GetClassNameW(HWND w,wchar_t* out,int size){SetLastError(77);if(!w||std::wcslen(w->cls)>=unsigned(size))return 0;std::wcscpy(out,w->cls);return std::wcslen(out);}
HWND GetParent(HWND w){SetLastError(77);return w?w->parent:nullptr;}
HWND GetWindow(HWND w,UINT cmd){SetLastError(77);return !w?nullptr:cmd==GW_CHILD?w->firstChild:w->nextSibling;}
BOOL IsWindowVisible(HWND w){return w&&w->visible;}
int GetGraphicsMode(HDC dc){SetLastError(77);return dc->graphicsMode;}
DWORD GetLayout(HDC dc){SetLastError(77);return dc->layout;}
BOOL EqualRect(const RECT* a,const RECT* b){return a->left==b->left&&a->top==b->top&&a->right==b->right&&a->bottom==b->bottom;}
BOOL IsRectEmpty(const RECT* r){return r->right<=r->left||r->bottom<=r->top;}
HGDIOBJ GetCurrentObject(HDC dc,int){return dc;}
int GetObjectW(HGDIOBJ object,int size,void* out){
 SetLastError(77);if(!object)return 0;
 if(size==sizeof(BITMAP)){static_cast<BITMAP*>(out)->bmBitsPixel=static_cast<HDC>(object)->bpp;return size;}
 CHECK(size==sizeof(LOGBRUSH));if(Mock::failBrushInfo)return 0;*static_cast<LOGBRUSH*>(out)=*static_cast<HBRUSH>(object);return size;
}
int GetClipBox(HDC dc,RECT* out){SetLastError(77);*out=dc->clip;return dc->clipKind;}
BOOL GetClientRect(HWND w,RECT* out){SetLastError(77);if(Mock::failGeometry)return FALSE;*out=w->client;return TRUE;}
BOOL GetWindowRect(HWND w,RECT* out){SetLastError(77);if(Mock::failGeometry)return FALSE;*out=w->outer;return TRUE;}
BOOL GetDCOrgEx(HDC dc,POINT* out){SetLastError(77);if(Mock::failGeometry)return FALSE;*out=dc->origin;return TRUE;}
BOOL ClientToScreen(HWND w,POINT* out){SetLastError(77);if(Mock::failGeometry)return FALSE;*out=w->clientOrigin;return TRUE;}
BOOL GetViewportOrgEx(HDC dc,POINT* out){SetLastError(77);if(Mock::failGeometry)return FALSE;*out=dc->viewport;return TRUE;}
BOOL GetWindowOrgEx(HDC dc,POINT* out){SetLastError(77);if(Mock::failGeometry)return FALSE;*out=dc->windowOrg;return TRUE;}
COLORREF GetSysColor(int index){SetLastError(77);return index==COLOR_WINDOW?0xffffff:0xc0c0c0;}
COLORREF GetBkColor(HDC){SetLastError(77);return 0xffffff;}
int GetBkMode(HDC){SetLastError(77);return 2;}
BOOL ImageList_GetIconSize(HIMAGELIST images,int* w,int* h){if(Mock::failSize)return FALSE;*w=images->width;*h=images->height;return TRUE;}
void NativeMask(HIMAGELIST images,int index,HDC dc,UINT flags,const IMAGELISTDRAWPARAMS* params){
 const bool white=dc==&Mock::whiteMemory;auto& bits=white?Mock::whitePixels:Mock::pixels;const int stride=white?Mock::whiteStride:Mock::stride;
 const int width=Mock::bounds.right-Mock::bounds.left,height=Mock::bounds.bottom-Mock::bounds.top;
 const int xb=params?params->xBitmap:0,yb=params?params->yBitmap:0;const DWORD state=params?params->fState:0;
 auto layer=[&](int i){const auto& icon=images->icons.at(i);CHECK(icon.rgb.size()==icon.solid.size());
  for(int y=0;y<height;++y)for(int x=0;x<width;++x){
   const int sx=x+xb,sy=y+yb;if(sx<0||sy<0||sx>=images->width||sy>=images->height)continue;
   const int source=sy*images->width+sx;if(!icon.solid.at(source))continue; // real binary native mask, independent of RGB
   auto color=icon.rgb.at(source);auto& dst=bits[y*stride+x];
   if(state&ILS_SATURATE)color.rgbBlue=color.rgbGreen=color.rgbRed=BYTE((unsigned(color.rgbBlue)+color.rgbGreen+color.rgbRed)/3);
   const unsigned a=(state&ILS_ALPHA)?params->Frame:255;CHECK(a<=255);
   dst.rgbBlue=BYTE((color.rgbBlue*a+dst.rgbBlue*(255-a))/255);
   dst.rgbGreen=BYTE((color.rgbGreen*a+dst.rgbGreen*(255-a))/255);
   dst.rgbRed=BYTE((color.rgbRed*a+dst.rgbRed*(255-a))/255);dst.rgbReserved=0; // RGB-only legacy renderer
  }};
 layer(index);if(const UINT overlay=flags&ILD_OVERLAYMASK)layer(images->overlays.at(overlay));
 if(white&&Mock::badWhite){if(Mock::badWhite==1)bits[1].rgbBlue=0;else bits[0].rgbGreen=1;}
}
BOOL Record(int tag,HIMAGELIST images,int index,HDC dc,int x,int y,int w,int h,COLORREF bg,COLORREF fg,UINT flags,const IMAGELISTDRAWPARAMS* params=nullptr){
 Mock::nativeCalls.push_back({tag,index,dc,x,y,w,h,bg,fg,flags,params?*params:IMAGELISTDRAWPARAMS{}});
 ++Mock::calls;Mock::tag=tag;Mock::index=index;Mock::target=dc;Mock::flags=flags;Mock::x=x;Mock::y=y;Mock::width=w;Mock::height=h;Mock::background=bg;Mock::foreground=fg;
 if(Mock::duringNative&&!Mock::reentered){Mock::reentered=true;Mock::duringNative();Mock::reentered=false;}
 if(images&&images->legacy&&(dc==&Mock::memory||dc==&Mock::whiteMemory))NativeMask(images,index,dc,flags,params);
 else if(dc==&Mock::memory&&!Mock::pixels.empty()&&!Mock::noAlpha){
  if(Mock::drawPixels.empty())Mock::pixels[0].rgbReserved=255; // native opaque BLACK glyph
  else{
   const int width=Mock::bounds.right-Mock::bounds.left,height=Mock::bounds.bottom-Mock::bounds.top;
   CHECK(Mock::drawPixels.size()==size_t(width*height));
   for(int row=0;row<height;++row)for(int col=0;col<width;++col)Mock::pixels[row*Mock::stride+col]=Mock::drawPixels[row*width+col];
  }
 }
 SetLastError(dc==&Mock::whiteMemory?Mock::whiteError:123);return !Mock::failNative&&!(dc==&Mock::whiteMemory&&Mock::failWhiteNative);
}
BOOL ImageList_Draw(HIMAGELIST images,int index,HDC dc,int x,int y,UINT flags){return Record(0,images,index,dc,x,y,0,0,0,0,flags);}
BOOL ImageList_DrawEx(HIMAGELIST images,int index,HDC dc,int x,int y,int w,int h,COLORREF bg,COLORREF fg,UINT flags){return Record(1,images,index,dc,x,y,w,h,bg,fg,flags);}
BOOL ImageList_DrawIndirect(IMAGELISTDRAWPARAMS* p){if(!p)return FALSE;Mock::indirect=*p;return Record(2,p->himl,p->i,p->hdcDst,p->x,p->y,p->cx,p->cy,p->rgbBk,p->rgbFg,p->fStyle,p);}
bool Contains(const RECT& r,int x,int y){return x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom;}
void PaintSurface(HDC dc,const RECT* rect,DWORD pixel){
 if(!dc||!rect)return;
 for(int y=0;y<dc->surfaceHeight;++y)for(int x=0;x<dc->surfaceWidth;++x)
  if(Contains(*rect,x,y)&&Contains(dc->clip,x,y)&&(!dc->useHole||!Contains(dc->hole,x,y)))dc->surface[y*dc->surfaceWidth+x]=pixel;
}
BOOL PatBlt(HDC dc,int x,int y,int w,int h,DWORD rop){
 Mock::pats.push_back({dc,{x,y,x+w,y+h},rop});CHECK(rop==BLACKNESS);
 if(!Mock::failPat)PaintSurface(dc,&Mock::pats.back().rect,0);SetLastError(66);return !Mock::failPat;
}
int FillRect(HDC dc,const RECT* rect,HBRUSH brush){
 Mock::fills.push_back({dc,rect,rect?*rect:RECT{},brush,GetLastError()});
 if(Mock::fillResult&&brush){const auto value=reinterpret_cast<ULONG_PTR>(brush);PaintSurface(dc,rect,value<=COLOR_MENUBAR+1u?0xffffff:brush->lbColor);}
 if(Mock::duringFill&&!Mock::fillReentered){Mock::fillReentered=true;Mock::duringFill();Mock::fillReentered=false;}
 SetLastError(Mock::fillError);return Mock::fillResult;
}
int FillImport(HDC dc,const RECT* rect,HBRUSH brush){
 const auto it=Mock::hooks.find("FillRect");
 return it==Mock::hooks.end()?FillRect(dc,rect,brush):reinterpret_cast<decltype(&::FillRect)>(it->second)(dc,rect,brush);
}
BOOL GetModuleHandleExW(DWORD,LPCWSTR,HMODULE* out){*out=reinterpret_cast<void*>(1);return !Mock::failModule;}
FARPROC GetProcAddress(HMODULE,const char* name){return std::strcmp(name,"ImageList_GetIconSize")==0?reinterpret_cast<FARPROC>(&ImageList_GetIconSize):nullptr;}
HPAINTBUFFER BeginBufferedPaint(HDC,const RECT* r,int format,BP_PAINTPARAMS* p,HDC* memory){
 const bool white=Mock::live==1;CHECK(Mock::live<=1);++Mock::begins;CHECK(format==BPBF_TOPDOWNDIB);CHECK(p->dwFlags==0);Mock::bounds=*r;Mock::blend=*p->pBlendFunction;
 const int stride=r->right-r->left+(white?Mock::whitePadding:Mock::padding);auto& pixels=white?Mock::whitePixels:Mock::pixels;
 pixels.assign(stride*(r->bottom-r->top),{});if(white)Mock::whiteStride=stride;else Mock::stride=stride;
 if(Mock::poisonPadding)for(int y=0;y<r->bottom-r->top;++y)for(int x=r->right-r->left;x<stride;++x)pixels[y*stride+x]={9,8,7,201};
 if(Mock::failBegin||(white&&Mock::failWhiteBegin))return nullptr;++Mock::live;
 *memory=(Mock::failMemory||(white&&Mock::failWhiteMemory))?nullptr:white?&Mock::whiteMemory:&Mock::memory;return reinterpret_cast<void*>(white?2:1);
}
HRESULT BufferedPaintClear(HPAINTBUFFER buffer,const RECT*){return Mock::failClear||(buffer==reinterpret_cast<void*>(2)&&Mock::failWhiteClear)?E_FAIL:S_OK;}
HRESULT GetBufferedPaintBits(HPAINTBUFFER buffer,RGBQUAD** pixels,int* stride){
 const bool white=buffer==reinterpret_cast<void*>(2);*pixels=white?Mock::nullWhiteBits?nullptr:Mock::whitePixels.data():Mock::pixels.data();
 const int reported=white?Mock::whiteReportedStride:Mock::reportedStride;*stride=reported<0?(white?Mock::whiteStride:Mock::stride):reported;
 return Mock::failBits||(white&&Mock::failWhiteBits)?E_FAIL:S_OK;
}
BOOL GdiFlush(){++Mock::flushCalls;SetLastError(77);return !Mock::failFlush&&Mock::flushCalls!=Mock::failFlushAt;}
HRESULT EndBufferedPaint(HPAINTBUFFER buffer,BOOL update){
 CHECK(buffer==reinterpret_cast<void*>(1)||buffer==reinterpret_cast<void*>(2));CHECK(buffer!=reinterpret_cast<void*>(2)||!update);
 ++Mock::ends;--Mock::live;CHECK(Mock::live>=0);Mock::update=update;if(update)Mock::committedPixels=Mock::pixels;SetLastError(88);return Mock::failEnd?E_FAIL:S_OK;
}
ULONGLONG GetTickCount64(){SetLastError(77);return Mock::tick;}
template<class T>Mock::Arg LogArg(T value){
 if constexpr(std::is_same_v<T,const wchar_t*>||std::is_same_v<T,wchar_t*>)return std::wstring(value?value:L"");
 else if constexpr(std::is_pointer_v<T>)return std::uint64_t(reinterpret_cast<std::uintptr_t>(value));
 else return std::uint64_t(value);
}
template<class...T>void DebugLog(int,const wchar_t* format,T...args){
 size_t placeholders=0;for(auto ch:std::wstring_view(format))if(ch==L'%')++placeholders;CHECK(placeholders==sizeof...(args));
 if(std::wstring_view(format).find(L"stage=pixels")!=std::wstring_view::npos)CHECK(Mock::live==1); // before EndBufferedPaint
 Mock::logs.push_back({format,{LogArg(args)...}});SetLastError(99);
}
namespace wil {
 struct srwlock{struct Guard{~Guard(){}};Guard lock_shared(){return{};}Guard lock_exclusive(){return{};}};
 template<class F>struct Exit{F f;~Exit(){f();}};
 template<class F>auto scope_exit(F&& f){static_assert(!std::is_reference_v<F>);static_assert(std::is_same_v<decltype(f()),void>);return Exit<F>{std::forward<F>(f)};}
}
namespace GlassShared {
 GlassControlKind GlassGetWindowKind(HWND w){SetLastError(77);return w?w->kind:GlassControlKind::None;}
 bool GlassIsBackdropAllowedForHwnd(HWND w){SetLastError(77);return w&&w->backdrop;}
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
  std::array<void*,N> previous{},originals{}; // supplied effective originals; not a PE/trampoline emulator
  HookDispatcher(const HookDispatcherDependency<N,M>& d,void* m):dep(d),moduleAddress(m){}
  void CacheHookData(){}
  int GetRef(size_t i){return refs[i];}
  void Restore(size_t i){
   const auto name=std::get<1>(dep[i]);
   if(previous[i])Mock::hooks[name]=previous[i];else Mock::hooks.erase(name);
   previous[i]=originals[i]=nullptr;refs[i]=0;
  }
  bool EnableHook(size_t i,bool enable){
   const auto name=std::get<1>(dep[i]);
   if(!moduleAddress||(std::strcmp(name,"FillRect")==0?Mock::failProbeRegistration:int(i)==Mock::missing))return false;
   if(enable){
    if(refs[i]++)return false;
    const auto it=Mock::hooks.find(name);previous[i]=it==Mock::hooks.end()?nullptr:it->second;
    const std::string_view api=name;
    void* native=api=="ImageList_Draw"?reinterpret_cast<void*>(&ImageList_Draw):api=="ImageList_DrawEx"?reinterpret_cast<void*>(&ImageList_DrawEx):api=="ImageList_DrawIndirect"?reinterpret_cast<void*>(&ImageList_DrawIndirect):reinterpret_cast<void*>(&FillRect);
    originals[i]=previous[i]?previous[i]:native;Mock::hooks[name]=std::get<2>(dep[i]);
   }else{if(!refs[i]||--refs[i])return false;Restore(i);}
   return true;
  }
  template<size_t I,class F>F GetOrg(){return reinterpret_cast<F>(refs[I]?originals[I]:nullptr);}
  void DisableAllHooks(){for(size_t i=0;i<N;++i)if(refs[i])Restore(i);}
 };
}
#include "PRODUCTION"
using namespace GlassCore::DelphiInputIcons;
struct Fixture {
 Window edit{L"TButtonedEdit"},search{L"TSearchBox"},glyph{L"TEditButton.TGlyph",&edit},other{L"TEdit"};DC dc{&search};Images images;
 Fixture(){
  Shutdown();CHECK(Mock::hooks.empty());Mock::full=true;glyph.kind=GlassShared::GlassControlKind::None;
  Mock::failSize=Mock::failModule=Mock::failBegin=Mock::failMemory=Mock::failClear=Mock::failEnd=Mock::failNative=Mock::failBits=Mock::failFlush=Mock::noAlpha=false;
  Mock::failGeometry=Mock::failBrushInfo=Mock::failProbeRegistration=Mock::failPat=false;Mock::pats.clear();
  Mock::calls=Mock::begins=Mock::ends=Mock::live=0;Mock::duringNative={};Mock::duringFill={};Mock::memory={};Mock::whiteMemory={};Mock::missing=-1;
  Mock::failWhiteBegin=Mock::failWhiteMemory=Mock::failWhiteClear=Mock::failWhiteBits=Mock::failWhiteNative=Mock::poisonPadding=Mock::nullWhiteBits=false;
  Mock::whitePadding=0;Mock::whiteReportedStride=-1;Mock::flushCalls=0;Mock::failFlushAt=-1;Mock::badWhite=0;Mock::whiteError=124;
  Mock::nativeCalls.clear();Mock::whitePixels.clear();
  Mock::padding=0;Mock::reportedStride=-1;Mock::drawPixels.clear();Mock::committedPixels.clear();Mock::logs.clear();Mock::fills.clear();
  Mock::fillResult=7;Mock::fillError=55;Mock::tick+=2000;Startup();
 }
 ~Fixture(){Shutdown();assert(Mock::hooks.empty());}
 BOOL Ex(HDC dest=nullptr,UINT style=0){return DrawEx(&images,7,dest?dest:&dc,4,5,0,0,0x112233,0x445566,style);}
 void Alpha(){CHECK(Mock::begins==1&&Mock::ends==1&&Mock::live==0&&Mock::update);CHECK(Mock::target==&Mock::memory);CHECK((Mock::flags&(ILD_TRANSPARENT|ILD_PRESERVEALPHA))==(ILD_TRANSPARENT|ILD_PRESERVEALPHA));CHECK(Mock::blend.SourceConstantAlpha==255&&Mock::blend.AlphaFormat==AC_SRC_ALPHA);CHECK(Mock::bounds.left==4&&Mock::bounds.right==20&&Mock::bounds.bottom==23);CHECK(!g_inside);}
};
size_t LogCount(std::wstring_view stage){size_t count=0;for(const auto& log:Mock::logs)if(log.format.find(stage)!=std::wstring::npos)++count;return count;}
const Mock::Log& LastLog(std::wstring_view stage){for(auto it=Mock::logs.rbegin();it!=Mock::logs.rend();++it)if(it->format.find(stage)!=std::wstring::npos)return *it;throw std::runtime_error("missing log");}
size_t Field(const Mock::Log& log,const wchar_t* name){
 const auto at=log.format.find(std::wstring(name)+L"=");CHECK(at!=std::wstring::npos);size_t count=0;
 for(size_t i=0;i<at;++i)if(log.format[i]==L'%')++count;return count;
}
std::uint64_t Number(const Mock::Log& log,const wchar_t* name,size_t component=0){return std::get<std::uint64_t>(log.args.at(Field(log,name)+component));}
std::wstring Text(const Mock::Log& log,const wchar_t* name){return std::get<std::wstring>(log.args.at(Field(log,name)));}
void Targets(){for(int target=0;target<4;++target){Fixture f;Window legacy{L"TGlyph",&f.edit};f.dc.window=target==0?&f.search:target==1?&f.glyph:target==2?&f.edit:&legacy;CHECK(f.Ex());f.Alpha();CHECK(Mock::index==7&&Mock::background==0x112233&&Mock::foreground==0x445566);CHECK(GetLastError()==123);}}
void Qualified(){
 for(LPCWSTR cls:{L"TEditButton.TGlyph",L"teditbutton.tglyph",L"TGlyph"}){
  Fixture f;f.glyph.cls=cls;f.dc.window=&f.glyph;
  for(int i:{0,3,17,28})CHECK(Draw(&f.images,i,&f.dc,0,0,0));
  CHECK(Mock::begins==4&&Mock::index==28&&Mock::committedPixels[0].rgbReserved==255);
 }
 for(LPCWSTR cls:{L"MyTEditButton.TGlyph",L"TEditButton.TGlyphExtra",L"TEditButton.TGlyph.Child",L"TGlyphExtra"}){
  Fixture f;f.glyph.cls=cls;f.dc.window=&f.glyph;CHECK(f.Ex());CHECK(Mock::begins==0);
 }
 for(int kind=0;kind<4;++kind){
  Fixture f;Window middle{L"TPanel",&f.edit};f.dc.window=&f.glyph;
  if(kind==0)f.glyph.parent=&f.other;else if(kind==1)f.glyph.parent=&f.search;else if(kind==2)f.glyph.parent=&middle;else f.glyph.parent=nullptr;
  CHECK(f.Ex());CHECK(Mock::begins==0);
 }
}
void Isolation(){
 for(int which=0;which<11;++which){Fixture f;Window fake{L"TButtonedEditExtra"},foreign{L"TEditButton.TGlyph",&f.other};
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
void Lifecycle(){Fixture f;Startup();for(auto r:g_dispatcher.refs)CHECK(r==1);CHECK(Mock::hooks.count("FillRect")==1);Shutdown();for(auto r:g_dispatcher.refs)CHECK(r==0);CHECK(f.Ex());CHECK(Mock::begins==0);Mock::missing=0;Startup();CHECK(g_dispatcher.refs[0]==0&&g_dispatcher.refs[1]==1);CHECK(f.Ex());f.Alpha();}
void SetMixed(Fixture& f){
 f.images.width=3;f.images.height=2;Mock::padding=2;
 Mock::drawPixels={{0,0,0,0},{255,255,255,255},{0,0,0,255},{10,20,30,100},{250,251,252,200},{255,255,255,0}};
}
void Mixed(){
 Fixture f;SetMixed(f);const RECT background{104,2,128,21};LOGBRUSH white{BS_SOLID,0xffffff};
 CHECK(FillImport(&f.dc,&background,&white)==7);CHECK(f.Ex());
 CHECK(Mock::fills.size()==1&&Mock::fills[0].brush==&white&&white.lbColor==0xffffff);
 CHECK(Mock::committedPixels.size()==10);
 for(int row=0;row<2;++row)for(int col=0;col<3;++col)CHECK(Mock::committedPixels[row*5+col]==Mock::drawPixels[row*3+col]);
 CHECK(Mock::committedPixels[3]==RGBQUAD{}&&Mock::committedPixels[8]==RGBQUAD{});
 CHECK(GetLastError()==123&&Mock::live==0); // diagnostics must not normalize any bytes
}
void ProbeFill(){
 for(int target=0;target<3;++target){
  Fixture f;f.dc.window=target==0?&f.search:target==1?&f.glyph:&f.edit;
  const RECT rect{104,2,128,21};LOGBRUSH brush{BS_SOLID,0xffffff};SetLastError(19);
  CHECK(FillImport(&f.dc,&rect,&brush)==7);CHECK(GetLastError()==55);
  CHECK(Mock::fills.size()==1&&Mock::fills[0].pointer==&rect&&Mock::fills[0].brush==&brush&&Mock::fills[0].entryError==19);
  CHECK(Mock::begins==0&&brush.lbColor==0xffffff);
  CHECK(LogCount(L"stage=fill")==size_t(ProbeBuild));
  if(ProbeBuild){
   const auto& log=LastLog(L"stage=fill");CHECK(Text(log,L"class")==f.dc.window->cls);
   CHECK(Number(log,L"hwnd")==reinterpret_cast<std::uintptr_t>(target==1?&f.edit:f.dc.window));
   CHECK(Number(log,L"target")==reinterpret_cast<std::uintptr_t>(f.dc.window));
   CHECK(Number(log,L"rect")==104&&Number(log,L"rect",2)==128);
   CHECK(Number(log,L"clip")==104&&Number(log,L"clip",3)==21);
   CHECK(Number(log,L"brushRGB")==0xffffff&&Number(log,L"brushValid")==1&&Number(log,L"result")==7);
   CHECK(Number(log,L"client",2)==102&&Number(log,L"dcOrg")==100&&Number(log,L"clientOrg")==102);
   CHECK(Number(log,L"window")==100&&Number(log,L"window",3)==223&&Number(log,L"viewport")==3&&Number(log,L"windowOrg",1)==2);
   CHECK(Number(log,L"brushRGBValid")==1);
  }
 }
 for(int which=0;which<8;++which){
  Fixture f;const RECT rect{0,0,16,18};LOGBRUSH brush{BS_PATTERN,0x123456};
  if(which==0)f.dc.window=&f.other;else if(which==1)Mock::full=false;else if(which==2)f.search.backdrop=false;
  else if(which==3)f.search.kind=GlassShared::GlassControlKind::None;else if(which==4)f.dc.map=2;
  else if(which==5){f.dc.window=nullptr;f.dc.registered=&f.glyph;f.dc.bpp=1;}
  else if(which==6)g_inside=true;else f.dc.window=nullptr;
  SetLastError(19);CHECK(FillImport(&f.dc,&rect,&brush)==7);g_inside=false;
  CHECK(LogCount(L"stage=fill")==0&&Mock::fills.size()==1&&Mock::fills[0].entryError==19&&GetLastError()==55);
 }
 {Fixture f;f.dc.window=nullptr;f.dc.registered=&f.glyph;const RECT r{0,0,16,18};LOGBRUSH b{BS_PATTERN,0x123456};
  CHECK(FillImport(&f.dc,&r,&b)==7);CHECK(LogCount(L"stage=fill")==size_t(ProbeBuild));
  if(ProbeBuild)CHECK(Number(LastLog(L"stage=fill"),L"brushValid")==1&&Number(LastLog(L"stage=fill"),L"brushRGBValid")==0);}
 {Fixture f;const RECT r{0,0,16,18};HBRUSH b=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1u);
  CHECK(FillImport(&f.dc,&r,b)==7&&Mock::fills[0].brush==b);
  if(ProbeBuild){const auto& log=LastLog(L"stage=fill");CHECK(Number(log,L"systemBrush")==1&&Number(log,L"brushRGBValid")==1&&Number(log,L"brushRGB")==0xffffff);}}
 {Fixture f;const RECT r{0,0,16,18};LOGBRUSH b{BS_PATTERN,0x123456};Mock::failGeometry=Mock::failBrushInfo=true;Mock::fillResult=0;
  CHECK(FillImport(&f.dc,&r,&b)==0&&GetLastError()==55);
  if(ProbeBuild){const auto& log=LastLog(L"stage=fill");CHECK(Number(log,L"clientOk")==0&&Number(log,L"brushValid")==0&&Number(log,L"result")==0);}}
}
void ProbePixels(){
 {Fixture f;SetMixed(f);CHECK(f.Ex());CHECK(LogCount(L"stage=pixels")==size_t(ProbeBuild));
  if(ProbeBuild){const auto& log=LastLog(L"stage=pixels");
   CHECK(Text(log,L"api")==L"ImageList_DrawEx"&&Text(log,L"class")==L"TSearchBox");
   CHECK(Number(log,L"index")==7&&Number(log,L"stride")==5&&Number(log,L"available")==1);
   CHECK(Number(log,L"alpha0")==2&&Number(log,L"alpha255")==2&&Number(log,L"alphaPartial")==2);
   CHECK(Number(log,L"whiteNonzeroAlpha")==2&&Number(log,L"nonPremul")==2);
  }}
 for(int which=0;which<4;++which){Fixture f;
  if(which==0)Mock::failBits=true;else if(which==1)Mock::failFlush=true;else if(which==2)Mock::reportedStride=0;else Mock::noAlpha=true;
  CHECK(f.Ex());CHECK(Mock::target==&f.dc&&!Mock::update&&Mock::live==0&&GetLastError()==123);
  CHECK(LogCount(L"stage=pixels")==size_t(ProbeBuild));
  if(ProbeBuild){const auto& log=LastLog(L"stage=pixels");CHECK(Number(log,L"available")==unsigned(which==3));CHECK(Number(log,L"alpha255")==0);CHECK(Number(log,L"alpha0")==unsigned(which==3?16*18:0));}
 }
 {Fixture f;Mock::failNative=true;CHECK(!f.Ex());
  if(ProbeBuild){const auto& log=LastLog(L"stage=pixels");CHECK(Number(log,L"draw")==0&&Number(log,L"available")==0&&Number(log,L"alpha0")==0);}}
 {Fixture f;CHECK(Draw(&f.images,3,&f.dc,4,5,0x100));if(ProbeBuild)CHECK(Text(LastLog(L"stage=pixels"),L"api")==L"ImageList_Draw");}
 {Fixture f;IMAGELISTDRAWPARAMS p{sizeof(p),&f.images,11,&f.dc,4,5,0,0,2,3,44,55,0x100,66,77,88,99};
  CHECK(DrawIndirect(&p));if(ProbeBuild)CHECK(Text(LastLog(L"stage=pixels"),L"api")==L"ImageList_DrawIndirect");}
}
void ProbeBudget(){
 Fixture f;const RECT r{104,2,128,21};LOGBRUSH b{BS_SOLID,0xffffff};
 for(int i=0;i<50;++i)CHECK(FillImport(&f.dc,&r,&b)==7);
 for(int i=0;i<20;++i)CHECK(f.Ex());
 CHECK(LogCount(L"stage=fill")==size_t(ProbeBuild?24:0)&&LogCount(L"stage=pixels")==size_t(ProbeBuild?12:0));
 Mock::tick+=1000;CHECK(FillImport(&f.dc,&r,&b)==7);CHECK(f.Ex());
 CHECK(LogCount(L"stage=fill")==size_t(ProbeBuild?25:0)&&LogCount(L"stage=pixels")==size_t(ProbeBuild?13:0));
 std::thread worker([&]{CHECK(FillImport(&f.dc,&r,&b)==7);CHECK(f.Ex());});worker.join();
 CHECK(LogCount(L"stage=fill")==size_t(ProbeBuild?26:0)&&LogCount(L"stage=pixels")==size_t(ProbeBuild?14:0));
 CHECK(Mock::fills.size()==52&&Mock::calls==22);
}
void ProbeLifecycle(){
 {Fixture f;const RECT r{0,0,16,18};LOGBRUSH b{BS_SOLID,0xffffff};
  Mock::duringFill=[&]{CHECK(FillImport(&f.dc,&r,&b)==7);};CHECK(FillImport(&f.dc,&r,&b)==7);Mock::duringFill={};
  CHECK(Mock::fills.size()==2&&LogCount(L"stage=fill")==size_t(ProbeBuild)&&GetLastError()==55);
  Mock::duringNative=[&]{CHECK(FillImport(&f.dc,&r,&b)==7);};CHECK(f.Ex());
  CHECK(LogCount(L"stage=fill")==size_t(ProbeBuild));
  Shutdown();SetLastError(19);CHECK(FillImport(&f.dc,&r,&b)==7);
  CHECK(LogCount(L"stage=fill")==size_t(ProbeBuild)&&GetLastError()==55);
 }
 {Fixture f;const RECT r{0,0,16,18};LOGBRUSH b{BS_SOLID,0xffffff};
  Mock::duringFill=[&]{CHECK(f.Ex());};CHECK(FillImport(&f.dc,&r,&b)==7);f.Alpha();
  CHECK(Mock::fills.size()==1&&GetLastError()==55&&LogCount(L"stage=fill")==size_t(ProbeBuild));}
 {Fixture f;Shutdown();Mock::failProbeRegistration=true;Startup();CHECK(!Mock::hooks.count("FillRect"));
  if(ProbeBuild){const auto& log=LastLog(L"stage=arm");CHECK(Number(log,L"fillRef")==0&&Number(log,L"imageRefs")==1);}
  CHECK(f.Ex());f.Alpha();}
 {Fixture f;const RECT r{0,0,16,18};LOGBRUSH b{BS_SOLID,0xffffff};void* captured=Mock::hooks.count("FillRect")?Mock::hooks.at("FillRect"):reinterpret_cast<void*>(&FillRect);
  Shutdown();SetLastError(19);CHECK(reinterpret_cast<decltype(&::FillRect)>(captured)(&f.dc,&r,&b)==7);
  CHECK(Mock::fills.size()==1&&LogCount(L"stage=fill")==0&&GetLastError()==55);
 }
}
void ButtonGeometry(Fixture& f,bool child){
 f.edit.client={0,0,136,19};f.edit.outer={397,500,537,523};f.edit.clientOrigin={399,502};f.edit.firstChild=&f.glyph;
 f.glyph.client={0,0,16,16};f.glyph.outer={519,503,535,519};f.glyph.clientOrigin={519,503};
 f.dc.window=child?&f.glyph:&f.edit;f.dc.origin=child?f.glyph.clientOrigin:f.edit.clientOrigin;
 f.dc.viewport=f.dc.windowOrg={0,0};f.dc.clip=child?f.glyph.client:f.edit.client;
 f.dc.surfaceWidth=child?16:136;f.dc.surfaceHeight=child?16:19;f.dc.surface.assign(f.dc.surfaceWidth*f.dc.surfaceHeight,0x7f123456);
}
DWORD At(const DC& dc,int x,int y){return dc.surface[y*dc.surfaceWidth+x];}
void BackgroundChild(){
 for(LPCWSTR name:{L"TEditButton.TGlyph",L"TGlyph"}){
  Fixture f;ButtonGeometry(f,true);f.glyph.cls=name;LOGBRUSH b{BS_SOLID,0xffffff};const auto r=f.glyph.client;SetLastError(19);
  CHECK(FillImport(&f.dc,&r,&b)==7&&GetLastError()==55);CHECK(Mock::fills.size()==1&&Mock::fills[0].pointer==&r&&Mock::fills[0].entryError==19);
  CHECK(Mock::pats.size()==1&&Mock::pats[0].dc==&f.dc&&EqualRect(&Mock::pats[0].rect,&r));
  for(auto pixel:f.dc.surface)CHECK(pixel==0);CHECK(b.lbColor==0xffffff&&Mock::begins==0);
 }
}
void BackgroundParent(){
 for(bool visible:{false,true})for(int clipCase=0;clipCase<4;++clipCase){
  Fixture f;ButtonGeometry(f,false);f.glyph.visible=visible;
  const RECT button{120,1,136,17};if(clipCase==1)f.dc.clip=button;else if(clipCase==2)f.dc.clip={0,1,16,17};else if(clipCase==3)f.dc.clip={30,2,90,18};
  Window left{L"TGlyph",&f.edit};left.kind=GlassShared::GlassControlKind::None;
  left.client={0,0,16,16};left.outer={399,503,415,519};left.clientOrigin={399,503};f.glyph.nextSibling=&left;
  const RECT r=f.edit.client;LOGBRUSH b{BS_SOLID,0xffffff};SetLastError(19);const auto clip=f.dc.clip;
  CHECK(FillImport(&f.dc,&r,&b)==7&&GetLastError()==55);CHECK(Mock::fills.size()==1&&Mock::fills[0].pointer==&r);
  CHECK(Mock::pats.size()==2&&EqualRect(&f.dc.clip,&clip));
  for(int y=0;y<19;++y)for(int x=0;x<136;++x){
   const bool inClip=Contains(clip,x,y),inButton=Contains(button,x,y)||(x<16&&y>=1&&y<17);
   CHECK(At(f.dc,x,y)==(inClip?(inButton?0u:0xffffffu):0x7f123456u));
  }
 }
}
void BackgroundClip(){
 Fixture f;ButtonGeometry(f,false);const RECT r=f.edit.client;LOGBRUSH b{BS_SOLID,0xffffff};
 f.dc.clip={123,4,133,13};f.dc.clipKind=3;f.dc.useHole=true;f.dc.hole={126,6,129,11};
 CHECK(FillImport(&f.dc,&r,&b)==7);CHECK(Mock::pats.size()==1);
 for(int y=0;y<19;++y)for(int x=0;x<136;++x)CHECK(At(f.dc,x,y)==(Contains(f.dc.clip,x,y)&&!Contains(f.dc.hole,x,y)?0u:0x7f123456u));
 CHECK(f.dc.useHole&&f.dc.clipKind==3&&f.dc.hole.left==126);
}
void BackgroundIsolation(){
 for(int which=0;which<29;++which){
  Fixture f;ButtonGeometry(f,false);Window middle{L"TPanel",&f.edit};LOGBRUSH b{BS_SOLID,0xffffff};RECT r=f.edit.client;
  switch(which){
   case 0:f.dc.window=&f.search;break;case 1:f.dc.window=&f.other;break;case 2:f.edit.firstChild=nullptr;break;
   case 3:f.glyph.cls=L"TEditButton.TGlyphExtra";break;case 4:f.glyph.parent=&middle;break;
   case 5:Mock::full=false;break;case 6:f.edit.backdrop=false;break;case 7:f.glyph.backdrop=false;break;
   case 8:f.edit.kind=GlassShared::GlassControlKind::None;break;case 9:f.dc.map=2;break;
   case 10:f.dc.viewport={1,0};break;case 11:f.dc.windowOrg={0,1};break;case 12:f.dc.origin={397,500};break;
   case 13:f.dc.graphicsMode=GM_ADVANCED;break;case 14:f.dc.layout=1;break;
   case 15:b.lbColor=0x123456;break;case 16:b.lbStyle=BS_PATTERN;break;case 17:r={120,1,136,17};break;
   case 18:f.dc.window=nullptr;f.dc.registered=&f.edit;break;case 19:Mock::failGeometry=true;break;case 20:g_inside=true;break;
   case 21:Mock::failBrushInfo=true;break;case 22:f.glyph.outer.right++;break;
   case 23:f.glyph.outer={539,503,555,519};f.glyph.clientOrigin={539,503};break;
   case 24:f.glyph.outer={399,502,535,521};f.glyph.client={0,0,136,19};f.glyph.clientOrigin=f.edit.clientOrigin;break;
   case 25:f.glyph.client.right=0;break;case 26:f.glyph.client.right=1025;break;
   case 27:f.glyph.clientOrigin.x++;break;case 28:f.glyph.client.left=1;break;
  }
  SetLastError(19);CHECK(FillImport(&f.dc,&r,&b)==7&&GetLastError()==55);g_inside=false;
  CHECK(Mock::pats.empty()&&Mock::fills.size()==1&&Mock::fills[0].pointer==&r&&Mock::fills[0].entryError==19);
 }
}
void BackgroundFailures(){
 for(int which=0;which<4;++which){
  Fixture f;ButtonGeometry(f,true);const RECT r=f.glyph.client;LOGBRUSH b{BS_SOLID,0xffffff};
  if(which==0)Mock::fillResult=0;else if(which==1)Mock::failPat=true;else if(which==2)Shutdown();else{Shutdown();Mock::failProbeRegistration=true;Startup();}
  SetLastError(19);CHECK(FillImport(&f.dc,&r,&b)==Mock::fillResult&&GetLastError()==55);
  CHECK(Mock::fills.size()==1&&Mock::pats.size()==size_t(which==1));
  for(auto pixel:f.dc.surface)CHECK(pixel==(which==0?0x7f123456u:0xffffffu));
 }
 {Fixture f;ButtonGeometry(f,true);const RECT r=f.glyph.client;LOGBRUSH b{BS_SOLID,0xffffff};
  Mock::duringFill=[&]{CHECK(FillImport(&f.dc,&r,&b)==7);};CHECK(FillImport(&f.dc,&r,&b)==7);
  CHECK(Mock::fills.size()==2&&Mock::pats.size()==1&&GetLastError()==55);}
}
void LegacyFixture(Fixture& f){
 f.dc.window=&f.glyph;f.images.width=3;f.images.height=3;f.images.legacy=true;
 const MaskIcon icon{{{0,0,0,0},{255,255,255,0},{0,0,0,0},{255,255,255,0},{31,79,151,0},{0,0,0,0},{17,99,201,0},{0,0,0,0},{255,255,255,0}},
                     {false,true,true,false,true,false,true,true,false}};
 f.images.icons[7]=icon;f.images.icons[2]=icon;f.images.icons[2].solid={true,false,false,false,false,false,false,false,false};
 f.images.icons[2].rgb[0]={200,40,20,0};f.images.overlays[0x300]=2;
 Mock::padding=2;Mock::whitePadding=4;Mock::poisonPadding=true;
}
void CheckLegacy(Fixture& f,bool overlay=false,unsigned opacity=255,bool saturate=false,int xb=0,int yb=0,int width=3,int height=3){
 CHECK(Mock::begins==2&&Mock::ends==2&&Mock::live==0&&Mock::update&&GetLastError()==123&&!g_inside);
 CHECK(Mock::committedPixels.size()==size_t(Mock::stride*height));
 for(int y=0;y<height;++y)for(int x=0;x<width;++x){
  RGBQUAD expected{};unsigned alpha=0;const int source=(y+yb)*3+x+xb;
  auto layer=[&](int index){const auto& icon=f.images.icons.at(index);if(!icon.solid.at(source))return;
   auto rgb=icon.rgb.at(source);if(saturate)rgb.rgbBlue=rgb.rgbGreen=rgb.rgbRed=BYTE((unsigned(rgb.rgbBlue)+rgb.rgbGreen+rgb.rgbRed)/3);
   expected.rgbBlue=BYTE((rgb.rgbBlue*opacity+expected.rgbBlue*(255-opacity))/255);
   expected.rgbGreen=BYTE((rgb.rgbGreen*opacity+expected.rgbGreen*(255-opacity))/255);
   expected.rgbRed=BYTE((rgb.rgbRed*opacity+expected.rgbRed*(255-opacity))/255);alpha=opacity+alpha*(255-opacity)/255;
  };
  layer(7);if(overlay)layer(2);expected.rgbReserved=BYTE(alpha);
  CHECK(Mock::committedPixels[y*Mock::stride+x]==expected);
 }
 for(int y=0;y<height;++y){
  for(int x=width;x<Mock::stride;++x)CHECK((Mock::committedPixels[y*Mock::stride+x]==RGBQUAD{9,8,7,201}));
  for(int x=width;x<Mock::whiteStride;++x)CHECK((Mock::whitePixels[y*Mock::whiteStride+x]==RGBQUAD{9,8,7,201}));
 }
#ifdef _DEBUG
 const auto& log=LastLog(L"stage=result");CHECK(Number(log,L"legacyMask")==1);
#endif
 if(ProbeBuild){const auto& log=LastLog(L"stage=pixels");CHECK(Number(log,L"alpha0")==size_t(width*height)&&Number(log,L"alpha255")==0);}
}
void LegacyMask(){
 for(LPCWSTR name:{L"TEditButton.TGlyph",L"TGlyph",L"TButtonedEdit"}){
  Fixture f;LegacyFixture(f);if(std::wcscmp(name,L"TButtonedEdit")==0)f.dc.window=&f.edit;else f.glyph.cls=name;
  CHECK(f.Ex());CheckLegacy(f);CHECK(Mock::calls==2&&Mock::nativeCalls.size()==2);
  for(const auto& call:Mock::nativeCalls)CHECK(call.index==7&&call.x==4&&call.y==5&&call.background==0x112233&&call.foreground==0x445566&&call.flags==(ILD_TRANSPARENT|ILD_PRESERVEALPHA));
 }
 for(int variant=0;variant<4;++variant){
  Fixture f;LegacyFixture(f);auto& icon=f.images.icons[7];
  if(variant<2)for(auto& rgb:icon.rgb)rgb=variant==0?RGBQUAD{}:RGBQUAD{255,255,255,0};
  else for(size_t i=0;i<icon.solid.size();++i)icon.solid[i]=variant==3;
  CHECK(f.Ex());CheckLegacy(f);CHECK(Mock::calls==2);
 }
}
void LegacyParams(){
 {Fixture f;LegacyFixture(f);CHECK(Draw(&f.images,7,&f.dc,4,5,0x300));CheckLegacy(f,true);CHECK(Mock::calls==2);for(const auto& c:Mock::nativeCalls)CHECK(c.tag==0&&c.index==7&&c.flags==(0x300|ILD_TRANSPARENT|ILD_PRESERVEALPHA));}
 for(bool saturate:{false,true}){
  Fixture f;LegacyFixture(f);IMAGELISTDRAWPARAMS p{sizeof(p),&f.images,7,&f.dc,4,5,2,2,1,1,44,55,0x300,66,ILS_ALPHA|(saturate?ILS_SATURATE:0),128,99};const auto before=p;
  CHECK(DrawIndirect(&p));CheckLegacy(f,true,128,saturate,1,1,2,2);CHECK(p==before);
  CHECK(Mock::nativeCalls.size()==2);for(const auto& c:Mock::nativeCalls){auto expected=before;expected.hdcDst=c.dc;expected.fStyle|=ILD_TRANSPARENT|ILD_PRESERVEALPHA;
   CHECK(c.tag==2&&c.dc!=&f.dc&&c.indirect==expected);}
 }
}
void LegacyIsolation(){
 {Fixture f;LegacyFixture(f);f.dc.window=&f.search;CHECK(f.Ex());CHECK(Mock::begins==1&&Mock::ends==1&&!Mock::update&&Mock::calls==2&&Mock::target==&f.dc&&Mock::whitePixels.empty());}
 {Fixture f;LegacyFixture(f);f.images.legacy=false;SetMixed(f);CHECK(f.Ex());CHECK(Mock::begins==1&&Mock::calls==1);for(int y=0;y<2;++y)for(int x=0;x<3;++x)CHECK(Mock::committedPixels[y*Mock::stride+x]==Mock::drawPixels[y*3+x]);}
 for(UINT flags:{ILD_MASK,ILD_ROP,ILD_DPISCALE}){Fixture f;LegacyFixture(f);CHECK(f.Ex(nullptr,flags));CHECK(Mock::begins==0&&Mock::calls==1&&Mock::flags==flags);}
 {Fixture f;LegacyFixture(f);Mock::duringNative=[&]{CHECK(Draw(&f.images,7,&f.dc,0,0,0));};CHECK(f.Ex());CHECK(Mock::begins==2&&Mock::ends==2&&Mock::calls==4&&Mock::live==0&&!g_inside);}
}
void LegacyFailures(){
 for(int which=0;which<11;++which){
  Fixture f;LegacyFixture(f);
  switch(which){case 0:Mock::failWhiteBegin=true;break;case 1:Mock::failWhiteMemory=true;break;case 2:Mock::failWhiteClear=true;break;case 3:Mock::failWhiteBits=true;break;
   case 4:Mock::nullWhiteBits=true;break;case 5:Mock::whiteReportedStride=2;break;case 6:Mock::failFlushAt=2;break;case 7:Mock::failWhiteNative=true;break;case 8:Mock::failFlushAt=3;break;case 9:Mock::badWhite=2;break;case 10:Mock::badWhite=1;break;}
  CHECK(f.Ex());CHECK(Mock::begins==2&&Mock::live==0&&!Mock::update&&Mock::committedPixels.empty()&&Mock::target==&f.dc&&!g_inside&&GetLastError()==123);
  CHECK(Mock::calls==((which>=7)?3:2));const auto& call=Mock::nativeCalls.back();CHECK(call.dc==&f.dc&&call.flags==0&&call.index==7&&call.background==0x112233&&call.foreground==0x445566);
 }
}
int PriorFill(HDC dc,const RECT* r,HBRUSH b){
 CHECK(GetLastError()==19);CHECK(FillRect(dc,r,b)==7);SetLastError(67);return 13;
}
void ProbeOriginal(){
 Fixture f;auto cleanup=wil::scope_exit([]{Shutdown();Mock::hooks.clear();});
 Shutdown();Mock::hooks["FillRect"]=reinterpret_cast<void*>(&PriorFill);Startup();
 const RECT r{104,2,128,21};LOGBRUSH b{BS_SOLID,0xffffff};
 for(int foreign=0;foreign<2;++foreign){
  f.dc.window=foreign?&f.other:&f.search;SetLastError(19);
  CHECK(FillImport(&f.dc,&r,&b)==13&&GetLastError()==67);
  CHECK(Mock::fills.size()==size_t(foreign+1)&&Mock::fills.back().brush==&b&&Mock::fills.back().pointer==&r);
 }
 ButtonGeometry(f,true);const auto client=f.glyph.client;SetLastError(19);
 CHECK(FillImport(&f.dc,&client,&b)==13&&GetLastError()==67);
 CHECK(Mock::fills.size()==3&&Mock::pats.size()==1&&Mock::fills.back().pointer==&client);
 CHECK(LogCount(L"stage=fill")==size_t(ProbeBuild?2:0));
 if(ProbeBuild)CHECK(Number(LastLog(L"stage=fill"),L"result")==13&&Number(LastLog(L"stage=fill"),L"patched")==1);
 Shutdown();CHECK(Mock::hooks.at("FillRect")==reinterpret_cast<void*>(&PriorFill));Mock::hooks.clear();
}
int main(int argc,char** argv){try{
 CHECK(argc==2);std::string c=argv[1];
 if(c=="targets")Targets();else if(c=="qualified")Qualified();else if(c=="isolation")Isolation();else if(c=="params")Params();
 else if(c=="failures")Failures();else if(c=="reentry")Reentry();else if(c=="lifecycle")Lifecycle();else if(c=="mixed")Mixed();
 else if(c=="probe-fill")ProbeFill();else if(c=="probe-pixels")ProbePixels();else if(c=="probe-budget")ProbeBudget();else if(c=="probe-lifecycle")ProbeLifecycle();else if(c=="probe-original")ProbeOriginal();else if(c=="bg-child")BackgroundChild();else if(c=="bg-parent")BackgroundParent();else if(c=="bg-clip")BackgroundClip();else if(c=="bg-isolation")BackgroundIsolation();else if(c=="bg-failures")BackgroundFailures();else if(c=="legacy-mask")LegacyMask();else if(c=="legacy-params")LegacyParams();else if(c=="legacy-isolation")LegacyIsolation();else if(c=="legacy-failures")LegacyFailures();else return 2;
 std::cout<<"PASS "<<c<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
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
            for probe in ((False, True) if debug else (False,)):
                binary = tmp / ('debug-probe' if probe else 'debug' if debug else 'release')
                command = compiler + ['-std=c++20', '-Wall', '-Wextra', '-Werror',
                                      '-Wno-unknown-pragmas', '-Wno-sign-compare',
                                      '-Wno-misleading-indentation', '-pthread',
                                      '-I', str(inc), str(cpp), '-o', str(binary)]
                command += ['-O0', '-g', '-D_DEBUG'] if debug else ['-O2']
                if probe:
                    command.append('-DGLASS_DELPHI_DRAW_PROBE')
                result = subprocess.run(command, capture_output=True, text=True, timeout=120)
                if result.returncode:
                    raise AssertionError(f'{binary.name} build failed:\n{result.stdout}{result.stderr}')
                cls.binaries.append(binary)

    def run_case(self, name):
        for binary in self.binaries:
            with self.subTest(build=binary.name):
                result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_exact_edit_glyph_and_search_window_dc(self): self.run_case('targets')
    def test_qualified_class_all_indices_legacy_alias_and_no_substring_or_ancestor_match(self): self.run_case('qualified')
    def test_foreign_unregistered_mono_and_disabled_glass_are_native(self): self.run_case('isolation')
    def test_all_three_apis_preserve_native_image_parameters(self): self.run_case('params')
    def test_buffer_and_native_failures_do_not_commit_partial_icons(self): self.run_case('failures')
    def test_nested_image_calls_do_not_reenter_composition(self): self.run_case('reentry')
    def test_missing_slots_repeated_startup_and_shutdown_are_balanced(self): self.run_case('lifecycle')
    def test_separate_white_fill_and_mixed_black_white_partial_alpha_pixels_are_unchanged(self): self.run_case('mixed')
    def test_fill_probe_records_original_arguments_geometry_results_and_error_without_drawing(self): self.run_case('probe-fill')
    def test_pixel_probe_counts_full_buffer_without_padding_and_distinguishes_unavailable_bits(self): self.run_case('probe-pixels')
    def test_probe_budgets_are_separate_bounded_and_thread_local(self): self.run_case('probe-budget')
    def test_probe_reentry_missing_import_shutdown_and_captured_original_are_native(self): self.run_case('probe-lifecycle')
    def test_fill_probe_calls_and_restores_supplied_effective_original_even_for_foreign_windows(self): self.run_case('probe-original')
    def test_button_background_blackens_only_the_full_direct_glyph_client(self): self.run_case('bg-child')
    def test_parent_full_and_partial_fill_restore_both_visible_and_hidden_button_regions(self): self.run_case('bg-parent')
    def test_button_background_keeps_partial_complex_clip_and_untouched_pixels(self): self.run_case('bg-clip')
    def test_button_background_rejects_foreign_colored_unproven_or_transformed_calls(self): self.run_case('bg-isolation')
    def test_button_background_native_failures_shutdown_missing_slot_and_reentry(self): self.run_case('bg-failures')
    def test_rgb_only_native_mask_retains_opaque_black_white_colors_and_transparent_holes(self): self.run_case('legacy-mask')
    def test_rgb_only_mask_branch_preserves_index_overlay_crop_state_colors_and_caller_params(self): self.run_case('legacy-params')
    def test_rgb_only_branch_leaves_searchbox_alpha_icons_unsupported_styles_and_reentry_alone(self): self.run_case('legacy-isolation')
    def test_rgb_only_secondary_buffer_failures_discard_and_repeat_the_original_native_call(self): self.run_case('legacy-failures')


if __name__ == '__main__':
    unittest.main()
