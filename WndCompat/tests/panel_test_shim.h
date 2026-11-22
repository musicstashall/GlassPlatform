// Panel-only extensions to the existing portable Win32/IAT doubles. No real
// Windows rasterization is claimed; the production DIB bytes are inspected.
#pragma once
#include "coolbar_test_shim.h"
using BYTE = std::uint8_t;
using ULONG_PTR = std::uintptr_t;
using LONG_PTR = std::intptr_t;
struct BITMAP
{
    LONG bmType{}, bmWidth{1}, bmHeight{1}, bmWidthBytes{4};
    std::uint16_t bmPlanes{1}, bmBitsPixel{32};
    void* bmBits{};
};
struct TestBitmap
{
    std::array<BYTE, 4> pixel{};
    BITMAP info{};
    std::vector<BYTE> pixels;
    BYTE* Data() { return pixels.empty() ? pixel.data() : pixels.data(); }
};
using HBITMAP = TestBitmap*;
inline constexpr int MM_TEXT = 1, OBJ_BITMAP = 7, GWLP_HINSTANCE = -6;
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
    inline std::unordered_map<HWND, HWND> parents;
    inline std::unordered_map<HWND, HMODULE> instances;
    inline std::unordered_map<HDC, int> maps;
    inline DWORD error{};
}
inline HWND GetParent(HWND w) { return PanelTest::parents.count(w) ? PanelTest::parents.at(w) : nullptr; }
inline LONG_PTR GetWindowLongPtrW(HWND w, int index)
{
    assert(index == GWLP_HINSTANCE);
    return reinterpret_cast<LONG_PTR>(PanelTest::instances.count(w) ? PanelTest::instances.at(w)
        : GlassCore::ModuleHandler::g_entryPointModule);
}
inline int GetMapMode(HDC dc) { return PanelTest::maps.count(dc) ? PanelTest::maps.at(dc) : MM_TEXT; }
inline DWORD GetLastError() { return PanelTest::error; }
inline void SetLastError(DWORD error) { PanelTest::error = error; }
inline HGDIOBJ PanelCurrentObject(HDC dc, UINT type)
{
    if (type == OBJ_BITMAP)
        return PanelTest::bitmaps.count(dc) ? PanelTest::bitmaps.at(dc) : PanelTest::stockBitmap;
    return GetCurrentObject(dc, type);
}
inline int GetObjectW(HGDIOBJ object, int bytes, void* out)
{
    if (bytes != sizeof(BITMAP)) return GetObjectW(static_cast<HBRUSH>(object), bytes, out);
    if (reinterpret_cast<ULONG_PTR>(object) <= 5) return 0;
    *static_cast<BITMAP*>(out) = static_cast<HBITMAP>(object)->info; return bytes;
}
inline BOOL StretchBlt(HDC, int, int, int, int, HDC, int, int, int, int, DWORD) { return TRUE; }
#if !defined(GLASS_TEST_REAL_OPAQUE_BITMAP)
// Background/caption suite never prepares images. The dedicated VCL image
// suite defines this macro and compiles the real source/selection helper.
namespace GlassCore::GdiUtils
{
    struct OpaqueBitmapSource
    {
        explicit OpaqueBitmapSource(HDC) {}
        explicit operator bool() const { return false; }
    };
}
#endif
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
// DrawThemeTextEx structures keep the x64 SDK layout/fields used by VCL.
struct POINT { LONG x{}, y{}; };
struct DTTOPTS
{
    DWORD dwSize{}, dwFlags{};
    COLORREF crText{}, crBorder{}, crShadow{};
    int iTextShadowType{};
    POINT ptShadowOffset{};
    int iBorderSize{}, iFontPropId{}, iColorPropId{}, iStateId{};
    BOOL fApplyOverlay{};
    int iGlowSize{};
    void* pfnDrawTextCallback{};
    LPARAM lParam{};
};
static_assert(sizeof(DTTOPTS) == 72);
inline constexpr DWORD DTT_TEXTCOLOR = 1, DTT_CALCRECT = 0x200, DTT_GLOWSIZE = 0x800;
inline constexpr DWORD DTT_CALLBACK = 0x1000, DTT_COMPOSITED = 0x2000;
using HPAINTBUFFER = void*;
struct RGBQUAD { BYTE rgbBlue{}, rgbGreen{}, rgbRed{}, rgbReserved{}; };
namespace PanelTest
{
    inline int themeBuffers = 0, nativeThemeCalls = 0, chainedThemeCalls = 0;
    inline HMODULE rootOriginalModule{};
    inline HRESULT bufferResult = S_OK;
    inline bool failThemeBufferDraw = false;
    inline HDC themeDc{};
    inline HTHEME theme{};
    inline int themePart{}, themeState{}, themeCount{};
    inline DWORD themeFlags{};
    inline LPCWSTR themeText{};
    inline const DTTOPTS* optionsPointer{};
    inline DTTOPTS themeOptions{};
    inline RECT themeRect{}, bufferedRect{};
    inline bool bufferErased = true;
    inline std::function<void()> duringThemeBuffer;
}
inline HRESULT DrawThemeTextEx(HTHEME theme, HDC dc, int part, int state,
    LPCWSTR text, int count, DWORD flags, LPRECT rect, const DTTOPTS* options)
{
    ++PanelTest::nativeThemeCalls;
    PanelTest::theme = theme; PanelTest::themeDc = dc;
    PanelTest::themePart = part; PanelTest::themeState = state;
    PanelTest::themeText = text; PanelTest::themeCount = count; PanelTest::themeFlags = flags;
    PanelTest::optionsPointer = options;
    PanelTest::themeOptions = options ? *options : DTTOPTS{};
    PanelTest::themeRect = rect ? *rect : RECT{};
    if (rect && ((flags & DT_CALCRECT) || (options && (options->dwFlags & DTT_CALCRECT)))) rect->right += 5;
    return PanelTest::failThemeBufferDraw && dc && !dc->owner ? E_FAIL : S_OK;
}
namespace GlassCore::Root
{
    // Only environment doubles for the production snippets included by the
    // runner: cache storage, root dispatcher lookup and shutdown state.
    inline constexpr size_t OHK_DELPHI_DRAWTHEMETEXTEX = 0;
    inline std::atomic<bool> g_shutdown{false};
    struct RootHookCache { std::vector<HMODULE> paintHooks, dcHooks; };
    inline bool StrEqI(LPCWSTR a, LPCWSTR b) { return a && b && _wcsicmp(a, b) == 0; }
    inline void PushPaint(HMODULE module, size_t slot, RootHookCache& cache)
    { assert(slot == OHK_DELPHI_DRAWTHEMETEXTEX); cache.paintHooks.push_back(module); }
    inline void PushDc(HMODULE module, size_t slot, RootHookCache& cache)
    { assert(slot == OHK_DELPHI_DRAWTHEMETEXTEX); cache.dcHooks.push_back(module); }
    template <size_t Slot, typename Fn> Fn GetRootOrg(HMODULE module)
    {
        static_assert(Slot == OHK_DELPHI_DRAWTHEMETEXTEX);
        PanelTest::rootOriginalModule = module;
        if (!module || !module->refs["DrawThemeTextEx"]) return nullptr;
        const auto it = module->originals.find("DrawThemeTextEx");
        return it == module->originals.end() ? nullptr : reinterpret_cast<Fn>(it->second);
    }
}
namespace GlassCore::Text
{
    inline HRESULT DrawThemeContent(HDC dc, const RECT& rect, const RECT*, const RECT*, DWORD,
        const std::function<void(HDC, HPAINTBUFFER, RGBQUAD*, int)>& callback,
        BYTE alpha, bool useBlt, bool update, bool erase)
    {
        assert(alpha == 255 && !useBlt && update);
        ++PanelTest::themeBuffers; PanelTest::bufferedRect = rect; PanelTest::bufferErased = erase;
        if (!SUCCEEDED(PanelTest::bufferResult)) return PanelTest::bufferResult;
        TestDC memory;
        static_cast<DcState&>(memory) = *dc;
        if (PanelTest::duringThemeBuffer) PanelTest::duringThemeBuffer();
        callback(&memory, nullptr, nullptr, 0);
        return PanelTest::bufferResult;
    }
}
#define GetCurrentObject PanelCurrentObject
#define GetSysColor PanelGetSysColor
#define GetClientRect PanelGetClientRect
#define GetModuleHandleW PanelGetModuleHandleW
#define GetProcAddress PanelGetProcAddress
