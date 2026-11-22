// Environment extensions for the complete production ListView translation unit.
// Reuse the strict bitmap/ref doubles; do not emulate a Windows rasterizer,
// executable IAT chains, theme renderer or the new VCL image policy here.
#pragma once
#include "vcl_images_test_shim.h"

using WCHAR = wchar_t;
using USHORT = std::uint16_t;
inline constexpr int DBG_IMPORTANT = 3;
inline constexpr int COLOR_SCROLLBAR = 0, SM_CXVSCROLL = 2, SM_CYHSCROLL = 3;
inline constexpr UINT ETO_IGNORELANGUAGE = 0x1000;
inline constexpr UINT DT_LEFT = 0, DT_TOP = 0, DT_SINGLELINE = 0x20, DT_NOCLIP = 0x100;
inline constexpr UINT OBJ_PEN = 1, PS_NULL = 5;
inline constexpr COLORREF CLR_INVALID = 0xffffffff;
struct PAINTSTRUCT { HDC hdc{}; BOOL fErase{}; RECT rcPaint{}; };
using LPPAINTSTRUCT = PAINTSTRUCT*;
struct LOGPEN { UINT lopnStyle{}; POINT lopnWidth{}; COLORREF lopnColor{}; };
struct TestPen { LOGPEN info{}; };
using HPEN = TestPen*;

namespace ListViewImageTest
{
    inline bool themed = false;
    inline int scrollDepth = 0, scrollEnsures = 0, chainMasks = 0, maskResult = TRUE;
    inline HMODULE shell{};
    inline std::unordered_map<HWND, COLORREF> backgrounds, textBackgrounds;
    inline int backgroundSets = 0, textBackgroundSets = 0;
    inline std::vector<HMODULE> stack;
    inline std::wstring themeClass = L"ListView";
}
inline int GetSystemMetrics(int) { return 17; }
inline int GetClipBox(HDC, LPRECT rc) { *rc = {0, 0, 200, 60}; return 1; }
inline COLORREF SetBkColor(HDC dc, COLORREF color) { return std::exchange(dc->background, color); }
inline COLORREF ListView_GetBkColor(HWND w) { return ListViewImageTest::backgrounds[w]; }
inline COLORREF ListView_GetTextBkColor(HWND w) { return ListViewImageTest::textBackgrounds[w]; }
inline BOOL ListView_SetBkColor(HWND w, COLORREF c)
{ ++ListViewImageTest::backgroundSets; ListViewImageTest::backgrounds[w] = c; return TRUE; }
inline BOOL ListView_SetTextBkColor(HWND w, COLORREF c)
{ ++ListViewImageTest::textBackgroundSets; ListViewImageTest::textBackgrounds[w] = c; return TRUE; }
inline USHORT CaptureStackBackTrace(DWORD, DWORD capacity, void** out, DWORD*)
{
    const auto count = std::min<size_t>(capacity, ListViewImageTest::stack.size());
    for (size_t i = 0; i < count; ++i) out[i] = ListViewImageTest::stack[i];
    return static_cast<USHORT>(count);
}
inline HRESULT DrawThemeBackground(HTHEME, HDC, int, int, LPCRECT, LPCRECT) { return S_OK; }
inline BOOL LineTo(HDC, int, int) { return TRUE; }
inline BOOL Polyline(HDC, const POINT*, int) { return TRUE; }
inline BOOL GetCurrentPositionEx(HDC, POINT* p) { *p = {}; return TRUE; }
inline BOOL MoveToEx(HDC, int, int, POINT*) { return TRUE; }
inline HPEN CreatePen(int style, int width, COLORREF color)
{ return new TestPen{{static_cast<UINT>(style), {width, 0}, color}}; }
inline BOOL ImageDeleteObject(HBRUSH brush) { delete brush; return TRUE; }
template <typename... Args> inline void DebugLog(const wchar_t*, Args...) {}
namespace wil
{
    using unique_hbrush = std::unique_ptr<LOGBRUSH>;
    using unique_hpen = std::unique_ptr<TestPen>;
}
namespace Shared::ThemeHelper
{
    inline HRESULT GetThemeClass(HTHEME, LPWSTR out, size_t capacity)
    {
        if (ListViewImageTest::themeClass.size() >= capacity) return E_FAIL;
        std::wcscpy(out, ListViewImageTest::themeClass.c_str()); return S_OK;
    }
}
namespace Shared::WinApiUtils
{
    inline bool IsThemed(HWND, LPCWSTR) { return ListViewImageTest::themed; }
}
namespace GlassCore::GdiUtils
{
    // Existing sysmon pen support is an external dependency of this image test.
    inline wil::unique_hpen PenFromLogPen(const LOGPEN&) { return {}; }
    struct LayeredStrokePen
    {
        explicit operator bool() const { return false; }
        HPEN get() const { return nullptr; }
        static LayeredStrokePen TryCreateSolid(const LOGPEN&, BYTE) { return {}; }
    };
}
namespace GlassCore::ScrollBarTheme
{
    inline bool IsInsideScrollBarDrawThemeOrig() { return ListViewImageTest::scrollDepth != 0; }
    inline auto GetComctlScrollBarDrawThemeOrig(HMODULE) { return &DrawThemeBackground; }
    inline void EnsureComctlScrollBarIatIfComctlCaller(HWND, HMODULE) { ++ListViewImageTest::scrollEnsures; }
    struct DrawThemeOrigCallDepth
    {
        DrawThemeOrigCallDepth() { ++ListViewImageTest::scrollDepth; }
        ~DrawThemeOrigCallDepth() { --ListViewImageTest::scrollDepth; }
    };
}
namespace GlassCore::ScrollBarDrawTheme
{
    inline constexpr int kScrollBarPartSizeBox = 10;
    inline void LogScrollBarDrawThemeCall(LPCWSTR, int, int) {}
    inline bool TryGetScrollBarDrawRect(LPCRECT rc, LPCRECT, RECT& out)
    { if (!rc || IsRectEmpty(rc)) return false; out = *rc; return true; }
    inline bool IsScrollBarTrackBackgroundPart(int) { return false; }
    inline bool IsScrollBarInteractivePart(int) { return false; }
    inline bool TryDrawScrollBarInteractiveGlass(decltype(&DrawThemeBackground), HTHEME, HDC, int, int, const RECT&)
    { return false; }
}
namespace GlassCore::PaintDispatcher
{
    struct PostPaintProvider { bool (*onAfterBeginPaint)(HWND, HDC, LPPAINTSTRUCT, HMODULE) noexcept; };
    inline std::vector<PostPaintProvider> listViewPostProviders;
    inline void RegisterPostPaintProvider(PostPaintProvider p) { listViewPostProviders.push_back(p); }
}
namespace GlassCore::ModuleHandler
{
    inline void RegisterModulePrewarmHandler(void (*)(HMODULE, const wchar_t*) noexcept) {}
}
inline HMODULE ListViewGetModuleHandleW(LPCWSTR name)
{
    return std::wstring_view(name) == L"SHELL32.dll" ? ListViewImageTest::shell : ImageGetModuleHandleW(name);
}
#undef GetModuleHandleW
#define GetModuleHandleW ListViewGetModuleHandleW
