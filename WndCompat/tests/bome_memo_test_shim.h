// Environment doubles for the complete production TBomeMemoDraw.cpp and the
// actual root gate, IAT rows and wrappers. Not Windows/DDB rasterization.
#pragma once
#include "coolbar_test_shim.h"
#include <Hooks/RootGdiHooks.h>
#include <GlassBomeMemo.h>
#include <stdexcept>

using ULONG_PTR = std::uintptr_t;
struct POINT { LONG x{}, y{}; };
struct BITMAP
{
    LONG bmType{}, bmWidth = 981, bmHeight = 576, bmWidthBytes = 3924;
    unsigned short bmPlanes = 1, bmBitsPixel = 32;
    void* bmBits{};
};
struct CHARSETINFO { UINT ciCharset{}, ciACP{}; };
inline constexpr UINT OBJ_BITMAP = 7, BS_HATCHED = 2, TCI_SRCCHARSET = 1, CP_ACP = 0;
inline constexpr int MM_TEXT = 1, GM_COMPATIBLE = 1, DEFAULT_CHARSET = 1, SYMBOL_CHARSET = 2;
inline constexpr int COLOR_MENUBAR = 30;
inline constexpr UINT DT_NOPREFIX = 0x800, DT_SINGLELINE = 0x20, DT_VCENTER = 4, DT_NOCLIP = 0x100;
namespace BomeTest
{
    struct DcExtra
    {
        int mapMode = MM_TEXT, graphics = GM_COMPATIBLE, charset = 0;
        DWORD layout{};
        POINT viewport{}, window{};
        BITMAP bitmap;
        bool originOk = true, bitmapPresent = true, bitmapReadable = true;
    };
    inline std::unordered_map<HDC, DcExtra> dcs;
    inline std::unordered_map<HWND, HDC> paints;
    inline COLORREF windowColor = 0xffffff, faceColor = 0xf0f0f0;
    inline int chainTexts{}, chainFills{}, rawTexts{}, rawFills{}, conversions{};
    inline int blackAttempts{};
    inline bool failBlack = false, failCharset = false, failSave = false, failMode = false;
    inline int nativeBkMode{};
    inline int failConversionCall{};
    inline HMODULE originalModule{};
    inline UINT conversionPage{}, conversionFlags{};
    inline int conversionCount{};
    inline std::wstring wideText;
    inline int wideCount{};
    inline UINT textFlags{};
    inline RECT textRect{}, blackRect{};
    inline HDC textDc{}, nativeDc{};
    inline LPCSTR nativeString{};
    inline int nativeCount{};
    inline LPRECT nativeRect{};
    inline UINT nativeFlags{};
    inline HBRUSH nativeBrush{};
    inline std::function<void()> duringBlack;
}
inline int GetMapMode(HDC dc) { return BomeTest::dcs[dc].mapMode; }
inline int GetGraphicsMode(HDC dc) { return BomeTest::dcs[dc].graphics; }
inline DWORD GetLayout(HDC dc) { return BomeTest::dcs[dc].layout; }
inline BOOL GetViewportOrgEx(HDC dc, POINT* pt)
{ *pt = BomeTest::dcs[dc].viewport; return BomeTest::dcs[dc].originOk; }
inline BOOL GetWindowOrgEx(HDC dc, POINT* pt)
{ *pt = BomeTest::dcs[dc].window; return BomeTest::dcs[dc].originOk; }
inline int GetTextCharset(HDC dc) { return BomeTest::dcs[dc].charset; }
inline BOOL TranslateCharsetInfo(DWORD* source, CHARSETINFO* info, DWORD flags)
{
    assert(flags == TCI_SRCCHARSET);
    if (BomeTest::failCharset) return FALSE;
    info->ciCharset = static_cast<UINT>(reinterpret_cast<ULONG_PTR>(source));
    info->ciACP = info->ciCharset == 204 ? 1251 : info->ciCharset == 128 ? 932 : 1252;
    return TRUE;
}
inline int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR text, int count, LPWSTR out, int size)
{
    ++BomeTest::conversions; BomeTest::conversionPage = cp;
    BomeTest::conversionFlags = flags; BomeTest::conversionCount = count;
    if (BomeTest::failConversionCall == BomeTest::conversions) return 0;
    const int bytes = count == -1 ? int(std::strlen(text)) + 1 : count;
    std::wstring converted;
    for (int i = 0; i < bytes; ++i)
    {
        const auto ch = static_cast<unsigned char>(text[i]);
        if (cp == 932 && ch == 0x82 && i+1 < bytes && static_cast<unsigned char>(text[i+1]) == 0xa0)
        { converted += L'\u3042'; ++i; }
        else converted += cp == 1251 && ch == 0xc0 ? L'\u0410' : wchar_t(ch);
    }
    if (!out) return converted.size();
    assert(size >= static_cast<int>(converted.size()));
    std::copy(converted.begin(), converted.end(), out); return converted.size();
}
inline COLORREF BomeGetSysColor(int index)
{ return index == COLOR_WINDOW ? BomeTest::windowColor : BomeTest::faceColor; }
inline HGDIOBJ BomeGetCurrentObject(HDC dc, UINT kind)
{
    if (kind != OBJ_BITMAP) return GetCurrentObject(dc, kind);
    auto& info = BomeTest::dcs[dc]; return info.bitmapPresent ? &info.bitmap : nullptr;
}
inline int BomeGetObjectW(HGDIOBJ object, int bytes, void* out)
{
    if (bytes != sizeof(BITMAP)) return GetObjectW(static_cast<HBRUSH>(object), bytes, out);
    for (const auto& [dc, info] : BomeTest::dcs)
    {
        (void)dc;
        if (object == &info.bitmap && info.bitmapReadable)
        { *static_cast<BITMAP*>(out) = info.bitmap; return sizeof(BITMAP); }
    }
    return 0;
}
inline BOOL BomePatBlt(HDC dc, int x, int y, int w, int h, DWORD rop)
{
    ++BomeTest::blackAttempts; BomeTest::blackRect = RECT{x,y,x+w,y+h};
    if (BomeTest::duringBlack) BomeTest::duringBlack();
    return BomeTest::failBlack ? FALSE : PatBlt(dc,x,y,w,h,rop);
}
inline int BomeSaveDC(HDC dc)
{ return BomeTest::failSave ? 0 : SaveDC(dc); }
inline int BomeSetBkMode(HDC dc, int mode)
{ return BomeTest::failMode ? 0 : SetBkMode(dc, mode); }
inline int DrawTextA(HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
{
    ++BomeTest::rawTexts; BomeTest::nativeDc = dc; BomeTest::nativeString = text;
    BomeTest::nativeBkMode = GetBkMode(dc);
    BomeTest::nativeCount = count; BomeTest::nativeRect = rect; BomeTest::nativeFlags = flags;
    if (rect && (flags & DT_CALCRECT)) rect->right += 13;
    if (text && (flags & DT_MODIFYSTRING)) *const_cast<char*>(text) = '#';
    return 41;
}
inline int BomeNativeFill(HDC dc, const RECT*, HBRUSH brush)
{
    ++BomeTest::rawFills; BomeTest::nativeDc = dc; BomeTest::nativeBrush = brush;
    return 37;
}
namespace GlassCore::PaintDispatcher
{
    inline bool HasOpenPaintSessionForHwnd(HWND w) { return BomeTest::paints.contains(w); }
    inline bool IsCurrentPaintDcForHwnd(HWND w, HDC dc)
    { const auto i = BomeTest::paints.find(w); return i != BomeTest::paints.end() && i->second == dc; }
}
namespace GlassCore::Text
{
    inline HRESULT BomeDrawTextWithAlpha(HDC dc, LPCWSTR text, int count, LPRECT rect,
        UINT flags, int& result, bool transparent)
    {
        BomeTest::textDc = dc; BomeTest::wideText.assign(text, count);
        BomeTest::wideCount = count; BomeTest::textFlags = flags; BomeTest::textRect = *rect;
        const auto hr = DrawTextWithAlpha(dc, text, count, rect, flags, result, transparent);
        rect->left = -999; // renderer gets its own RECT, never the caller's.
        return hr;
    }
}
namespace GlassCore::Root
{
    inline std::atomic<bool> g_shutdown{false};
    struct RootHookCache { std::vector<std::pair<HMODULE, size_t>> paintHooks, dcHooks; };
    inline void PushPaint(HMODULE module, size_t index, RootHookCache& cache)
    { cache.paintHooks.emplace_back(module, index); }
    template <size_t Slot, typename Fn> Fn GetRootOrg(HMODULE module)
    {
        static_assert(Slot == OHK_BOME_MEMO_TEXT || Slot == OHK_BOME_MEMO_FILL);
        const char* name = Slot == OHK_BOME_MEMO_TEXT ? "DrawTextA" : "FillRect";
        BomeTest::originalModule = module;
        if (!module || !module->refs[name]) return nullptr;
        const auto i = module->originals.find(name);
        return i == module->originals.end() ? nullptr : reinterpret_cast<Fn>(i->second);
    }
}
#define GetSysColor BomeGetSysColor
#define GetCurrentObject BomeGetCurrentObject
#define GetObjectW BomeGetObjectW
#define DrawTextWithAlpha BomeDrawTextWithAlpha
#define PatBlt BomePatBlt
#define SaveDC BomeSaveDC
#define SetBkMode BomeSetBkMode
