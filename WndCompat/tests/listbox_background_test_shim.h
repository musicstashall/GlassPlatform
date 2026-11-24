// Environment/argument doubles for the complete TListBoxDraw.cpp. No copy of
// its colour policy, Windows rasterizer or real PE/IAT chain is implemented.
#pragma once
#include "vcl_images_test_shim.h"
#include <thread>
#include <climits>
#undef LONG_MIN
#define LONG_MIN (-2147483647L - 1)

using WCHAR = wchar_t;
inline constexpr UINT ODT_LISTBOX = 2, ODT_COMBOBOX = 3, ODA_DRAWENTIRE = 1, ODA_SELECT = 2, ODA_FOCUS = 4;
inline constexpr UINT ODS_SELECTED = 1, ODS_FOCUS = 0x10, ODS_DISABLED = 4, ODS_COMBOBOXEDIT = 0x1000;
inline constexpr UINT TA_UPDATECP = 1, ETO_PDY = 0x2000;
inline constexpr UINT BS_NULL = 1, BS_PATTERN = 3, OBJ_PEN = 1, PS_SOLID = 0;
inline constexpr int BLACK_PEN = 7, BP_CHECKBOX = 3, BPBF_TOPDOWNDIB = 2, COLOR_HIGHLIGHT = 13;
inline constexpr int ERROR = 0, NULLREGION = 1, SIMPLEREGION = 2;
inline constexpr DWORD SRCINVERT = 0x00660046;
struct DRAWITEMSTRUCT
{
    UINT CtlType{}, CtlID{}, itemID{}, itemAction{}, itemState{};
    HWND hwndItem{}; HDC hDC{}; RECT rcItem{}; std::uintptr_t itemData{};
};
struct LOGPEN { UINT lopnStyle{}; POINT lopnWidth{}; COLORREF lopnColor{}; };
struct BP_PAINTPARAMS { DWORD cbSize{}, dwFlags{}; const RECT* prcExclude{}; const BLENDFUNCTION* pBlendFunction{}; };
namespace ListBackgroundTest
{
    inline COLORREF highlight = RGB(0, 120, 215);
    inline std::unordered_map<HBRUSH, BYTE> alphas;
    inline bool failBrush = false, failFill = false, failBlack = false, failSave = false, failClip = false;
    inline int brushCalls = 0, fills = 0, blackens = 0, liveBrushes = 0, themes = 0, extCalls = 0;
    inline BYTE lastAlpha{};
    inline HBRUSH lastBrush{};
    inline HDC lastDc{};
    inline RECT lastRect{};
    inline COLORREF lastColor{};
    inline int fillAlpha = -1;
    inline UINT extOptions{}, extCount{}, align{};
    inline bool drawTransparent{};
    inline HDC drawDc{}, nativeDrawDc{};
    inline LPCWSTR drawText{}, nativeDrawText{};
    inline int drawCount{}, nativeDrawCount{};
    inline LPRECT drawRect{}, nativeDrawRect{};
    inline UINT drawFormat{}, nativeDrawFormat{};
    inline const INT* extDx{};
    inline LPCWSTR extText{};
    inline BOOL extResult = TRUE;
    inline std::function<void()> duringFill;
    inline bool reentered = false;
    inline std::unordered_map<HDC, RECT> clips;
    inline std::unordered_map<HDC, std::vector<RECT>> savedClips;
    inline HDC blackDc{};
    inline RECT blackRect{};
    inline DWORD nativeError = 0x51, fillEntryError = 0;
    inline int clipChanges = 0;
    // Optional deterministic surface: replay only the supplied native rectangle,
    // brush alpha and actual clip, including holes. Not a Windows rasterizer.
    struct Surface
    {
        int width{}, height{};
        std::vector<DWORD> pixels;
        std::vector<RECT> holes;
    };
    inline std::unordered_map<HDC, Surface> surfaces;
}
inline BOOL EqualRect(const RECT* a, const RECT* b)
{ return a && b && a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom; }
inline BOOL IntersectRect(RECT* out, const RECT* a, const RECT* b)
{
    const RECT r{std::max(a->left,b->left), std::max(a->top,b->top), std::min(a->right,b->right), std::min(a->bottom,b->bottom)};
    *out = r; return !IsRectEmpty(&r);
}
inline BOOL PtInRect(const RECT* r, POINT p)
{ return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom; }
inline RECT ListClip(HDC dc)
{
    const auto it = ListBackgroundTest::clips.find(dc);
    return it == ListBackgroundTest::clips.end() ? RECT{-10000,-10000,10000,10000} : it->second;
}
inline int ListSaveDC(HDC dc)
{
    if (ListBackgroundTest::failSave) return 0;
    ListBackgroundTest::savedClips[dc].push_back(ListClip(dc)); return SaveDC(dc);
}
inline BOOL ListRestoreDC(HDC dc, int saved)
{
    auto& clips = ListBackgroundTest::savedClips.at(dc);
    ListBackgroundTest::clips[dc] = clips.at(saved-1); clips.resize(saved-1); return RestoreDC(dc,saved);
}
inline int IntersectClipRect(HDC dc, int l, int t, int r, int b)
{
    ++ListBackgroundTest::clipChanges;
    if (ListBackgroundTest::failClip) return ERROR;
    const RECT input{l,t,r,b}, clip = ListClip(dc);
    return IntersectRect(&ListBackgroundTest::clips[dc], &clip, &input) ? SIMPLEREGION : NULLREGION;
}
inline int GetClipBox(HDC dc, LPRECT out) { *out = ListClip(dc); return SIMPLEREGION; }
inline UINT GetTextAlign(HDC) { return ListBackgroundTest::align; }
inline COLORREF GetPixel(HDC, int, int) { return CLR_INVALID; }
inline COLORREF ListGetSysColor(int index)
{ return index == COLOR_HIGHLIGHT ? ListBackgroundTest::highlight : PanelGetSysColor(index); }
inline void ListPaintPixels(HDC dc, const RECT* r, COLORREF color, BYTE alpha)
{
    using namespace ListBackgroundTest;
    const auto it=surfaces.find(dc);
    if (it==surfaces.end() || !r) return;
    auto& s=it->second;
    const RECT clip=ListClip(dc);
    for (int y=0;y<s.height;++y)
        for (int x=0;x<s.width;++x)
        {
            const POINT p{x,y};
            if (!PtInRect(r,p) || !PtInRect(&clip,p)) continue;
            bool excluded=false;
            for (const RECT& hole:s.holes) if (PtInRect(&hole,p)) excluded=true;
            if (!excluded) s.pixels[y*s.width+x]=(static_cast<DWORD>(alpha)<<24)|(color&0xFFFFFFu);
        }
}
inline int ListFillRect(HDC dc, const RECT* r, HBRUSH brush)
{
    using namespace ListBackgroundTest;
    fillEntryError=GetLastError();
    ++fills; lastDc=dc; lastRect = r ? *r : RECT{}; lastBrush=brush;
    lastColor=brush ? brush->lbColor : CLR_INVALID;
    const auto it=alphas.find(brush); fillAlpha=it == alphas.end() ? -1 : int(it->second);
    if (!failFill) ListPaintPixels(dc,r,lastColor,fillAlpha<0 ? 0 : BYTE(fillAlpha));
    if (duringFill && !reentered) { reentered=true; duringFill(); reentered=false; }
    SetLastError(nativeError);
    return failFill ? 0 : 7;
}
inline BOOL ListPatBlt(HDC dc, int x, int y, int w, int h, DWORD rop)
{
    using namespace ListBackgroundTest;
    assert(rop == BLACKNESS); ++blackens; blackDc=dc; blackRect={x,y,x+w,y+h};
    if (!failBlack) ListPaintPixels(dc,&blackRect,0,0);
    SetLastError(0xdead);
    return !failBlack;
}
inline BOOL ListExtTextOutW(HDC, int, int, UINT options, const RECT*, LPCWSTR text, UINT count, const INT* dx)
{
    using namespace ListBackgroundTest;
    ++extCalls; extOptions=options; extText=text; extCount=count; extDx=dx; return extResult;
}
inline int ListDrawTextW(HDC dc, LPCWSTR text, int count, LPRECT rect, UINT format)
{
    using namespace ListBackgroundTest;
    nativeDrawDc=dc; nativeDrawText=text; nativeDrawCount=count;
    nativeDrawRect=rect; nativeDrawFormat=format; ++Test::nativeText; return 17;
}
namespace GlassCore::Text
{
    // Unlike the image shim's convenience overload, use the real API default
    // (false). Otherwise a six-argument call would falsely pass as glyph-only.
    inline HRESULT ListDrawTextWithAlpha(HDC dc, LPCWSTR text, int count, LPRECT rect,
        UINT format, int& result, bool transparent=false)
    {
        using namespace ListBackgroundTest;
        drawDc=dc; drawText=text; drawCount=count; drawRect=rect;
        drawFormat=format; drawTransparent=transparent;
        ++Test::alphaText; Test::alphaBkMode=dc->bkMode;
        Test::alphaTextColor=dc->text; Test::alphaFont=dc->font;
        if (Test::duringAlpha) Test::duringAlpha();
        result=15; return Test::alphaResult;
    }
}
inline BOOL Rectangle(HDC, int, int, int, int) { return TRUE; }
inline HGDIOBJ GetStockObject(int) { static LOGPEN pen; return &pen; }
inline HRESULT DrawThemeBackground(HTHEME, HDC, int, int, LPCRECT, LPCRECT) { return S_OK; }
inline HPAINTBUFFER BeginBufferedPaint(HDC, LPCRECT, int, BP_PAINTPARAMS*, HDC*) { return nullptr; }
inline HRESULT BufferedPaintClear(HPAINTBUFFER, LPCRECT) { return S_OK; }
inline HRESULT GetBufferedPaintBits(HPAINTBUFFER, RGBQUAD** bits, int*) { *bits=nullptr; return E_FAIL; }
inline HRESULT EndBufferedPaint(HPAINTBUFFER, BOOL) { return S_OK; }
namespace Shared::WinApiUtils
{
    inline HBRUSH ListCreateAlphaBrush(COLORREF color, BYTE alpha)
    {
        using namespace ListBackgroundTest;
        ++brushCalls; lastAlpha=alpha; SetLastError(0xbeef);
        if (failBrush) return nullptr;
        auto* brush = new LOGBRUSH{BS_PATTERN,color}; alphas[brush]=alpha; ++liveBrushes; return brush;
    }
}
namespace wil
{
    struct ListBrushDeleter
    {
        void operator()(HBRUSH brush) const
        { assert(ListBackgroundTest::alphas.erase(brush)==1); --ListBackgroundTest::liveBrushes; delete brush; SetLastError(0xfee); }
    };
    using unique_hbrush = std::unique_ptr<LOGBRUSH,ListBrushDeleter>;
}
namespace Shared::ThemeHelper
{ inline HRESULT GetThemeClass(HTHEME, LPWSTR out, size_t) { std::wcscpy(out,L"Button"); return S_OK; } }
namespace Shared::ColorUtils
{ inline COLORREF ARGBToCOLORREF(DWORD color) { return RGB((color>>16)&255,(color>>8)&255,color&255); } }
namespace Shared::BitmapUtils
{
    // Image conversion is deliberately unavailable here; the image tests own
    // that independent policy. Background tests must not try to normalize it.
    inline bool IsOemBitmap(HBITMAP) { return true; }
    inline HBITMAP Promise32BPP(HBITMAP) { throw std::runtime_error("unexpected image conversion"); }
    struct SpriteEffect { SpriteEffect(COLORREF,BYTE) {} void Apply(HBITMAP) const {} };
}
namespace GlassCore::GdiUtils { inline void ApplyBlackKeyAlpha(HBITMAP) { assert(false); } }
namespace GlassCore::Hooks::UxThemeHooks
{
    inline bool EnableThemeHooks(bool enable)
    { ListBackgroundTest::themes += enable ? 1 : -1; return true; }
}
namespace GlassCore::DelphiBackground { inline void ClearObservedListTail(HWND,const DRAWITEMSTRUCT*) {} }

#undef GetSysColor
#define GetSysColor ListGetSysColor
#define CreateSolidColorBrushWithAlpha ListCreateAlphaBrush
#define FillRect ListFillRect
#define PatBlt ListPatBlt
#define ExtTextOutW ListExtTextOutW
#define DrawTextW ListDrawTextW
#define DrawTextWithAlpha ListDrawTextWithAlpha
#define SaveDC ListSaveDC
#define RestoreDC ListRestoreDC
