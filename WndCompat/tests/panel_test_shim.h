// Panel-only extensions to the existing portable Win32/IAT doubles. No real
// Windows rasterization is claimed; the production DIB bytes are inspected.
#pragma once
#include "coolbar_test_shim.h"
using BYTE = std::uint8_t;
using ULONG_PTR = std::uintptr_t;
struct TestBitmap { std::array<BYTE, 4> pixel{}; };
using HBITMAP = TestBitmap*;
struct BITMAPINFOHEADER
{
    DWORD biSize{}; LONG biWidth{}, biHeight{}; unsigned short biPlanes{}, biBitCount{};
    DWORD biCompression{};
};
struct BITMAPINFO { BITMAPINFOHEADER bmiHeader{}; };
struct BLENDFUNCTION { BYTE BlendOp{}, BlendFlags{}, SourceConstantAlpha{}, AlphaFormat{}; };
inline constexpr DWORD BI_RGB = 0;
inline constexpr UINT DIB_RGB_COLORS = 0;
inline constexpr BYTE AC_SRC_OVER = 0, AC_SRC_ALPHA = 1;
inline constexpr int COLOR_MENUBAR = 30;
inline const HGDIOBJ HGDI_ERROR = reinterpret_cast<HGDIOBJ>(static_cast<std::intptr_t>(-1));
inline BYTE GetRValue(COLORREF c) { return c & 255; }
inline BYTE GetGValue(COLORREF c) { return (c >> 8) & 255; }
inline BYTE GetBValue(COLORREF c) { return (c >> 16) & 255; }
namespace PanelTest
{
    inline COLORREF windowColor = 0xffffff, faceColor = 0xf0f0f0;
    inline std::unordered_map<HWND, RECT> clients;
    inline std::unordered_map<HDC, HGDIOBJ> bitmaps;
    inline bool failDc = false, failBitmap = false, failSelect = false, failBlend = false;
    inline bool coreAvailable = true, endAvailable = true;
    inline int liveDcs = 0, liveBitmaps = 0, blends = 0;
    inline RECT blendRect{};
    inline BLENDFUNCTION blendMode{};
    inline std::array<BYTE, 4> blendPixel{};
    inline std::function<void()> duringBlend;
    inline HGDIOBJ stockBitmap = reinterpret_cast<HGDIOBJ>(std::uintptr_t(1));
}
inline COLORREF PanelGetSysColor(int i)
{ return i == COLOR_WINDOW ? PanelTest::windowColor : PanelTest::faceColor; }
inline BOOL PanelGetClientRect(HWND w, LPRECT r)
{
    if (auto it = PanelTest::clients.find(w); it != PanelTest::clients.end()) *r = it->second;
    else *r = RECT{0, 0, 500, 40};
    return TRUE;
}
inline HDC CreateCompatibleDC(HDC)
{
    if (PanelTest::failDc) return nullptr;
    auto* dc = new TestDC; ++PanelTest::liveDcs;
    PanelTest::bitmaps[dc] = PanelTest::stockBitmap; return dc;
}
inline HBITMAP CreateDIBSection(HDC, const BITMAPINFO* bi, UINT, void** bits, void*, DWORD)
{
    assert(bi->bmiHeader.biWidth == 1 && bi->bmiHeader.biHeight == -1);
    assert(bi->bmiHeader.biBitCount == 32 && bi->bmiHeader.biPlanes == 1);
    if (PanelTest::failBitmap) return nullptr;
    auto* bmp = new TestBitmap; ++PanelTest::liveBitmaps;
    *bits = bmp->pixel.data(); return bmp;
}
inline HGDIOBJ SelectObject(HDC dc, HGDIOBJ bitmap)
{
    if (PanelTest::failSelect) return HGDI_ERROR;
    return std::exchange(PanelTest::bitmaps.at(dc), bitmap);
}
inline HGDIOBJ SelectObject(HDC dc, HBITMAP bitmap)
{ return SelectObject(dc, static_cast<HGDIOBJ>(bitmap)); }
inline BOOL DeleteObject(HBITMAP bmp)
{ delete bmp; --PanelTest::liveBitmaps; return TRUE; }
inline BOOL DeleteDC(HDC dc)
{ PanelTest::bitmaps.erase(dc); delete dc; --PanelTest::liveDcs; return TRUE; }
inline BOOL GdiAlphaBlend(HDC, int x, int y, int w, int h, HDC src,
    int sx, int sy, int sw, int sh, BLENDFUNCTION blend)
{
    assert(sx == 0 && sy == 0 && sw == 1 && sh == 1);
    ++PanelTest::blends; PanelTest::blendRect = RECT{x, y, x+w, y+h};
    PanelTest::blendMode = blend;
    PanelTest::blendPixel = static_cast<HBITMAP>(PanelTest::bitmaps.at(src))->pixel;
    if (PanelTest::duringBlend) PanelTest::duringBlend();
    return !PanelTest::failBlend;
}
inline HMODULE PanelGetModuleHandleW(LPCWSTR name)
{
    return PanelTest::coreAvailable && std::wstring_view(name) == L"GlassCore.dll"
        ? GlassCore::ModuleHandler::g_entryPointModule : nullptr;
}
using FARPROC = void(*)();
FARPROC PanelGetProcAddress(HMODULE, LPCSTR);
#define GetSysColor PanelGetSysColor
#define GetClientRect PanelGetClientRect
#define GetModuleHandleW PanelGetModuleHandleW
#define GetProcAddress PanelGetProcAddress
