// Portable environment for the complete production EditHooks.cpp. These are
// Win32/IAT/ink-renderer doubles, not a replacement implementation of its policy.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#define WINAPI
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#define _wcsicmp wcscasecmp
using BYTE = std::uint8_t;
using BOOL = int;
using UINT = unsigned;
using INT = int;
using LONG = std::int32_t;
using DWORD = std::uint32_t;
using COLORREF = DWORD;
using HRESULT = std::int32_t;
using ULONGLONG = std::uint64_t;
using ULONG_PTR = std::uintptr_t;
using WPARAM = std::uintptr_t;
using LPARAM = std::intptr_t;
using LRESULT = std::intptr_t;
using USHORT = std::uint16_t;
using WCHAR = wchar_t;
using PVOID = void*;
using LPCSTR = const char*;
using LPCWSTR = const wchar_t*;
using LPWSTR = wchar_t*;
using FARPROC = void(*)();
struct RECT { LONG left{}, top{}, right{}, bottom{}; };
struct CHARRANGE { LONG cpMin{}, cpMax{}; };
using LPRECT = RECT*;
struct PAINTSTRUCT { RECT rcPaint{}; BOOL fErase = 1; };
using LPPAINTSTRUCT = PAINTSTRUCT*;
struct LOGBRUSH { UINT lbStyle{}; COLORREF lbColor{}; };
struct RGBQUAD { BYTE rgbBlue{}, rgbGreen{}, rgbRed{}, rgbReserved{}; };
struct BITMAPINFOHEADER
{
    DWORD biSize{}; LONG biWidth{}, biHeight{};
    unsigned short biPlanes{}, biBitCount{}; DWORD biCompression{};
};
struct BITMAPINFO { BITMAPINFOHEADER bmiHeader{}; };
struct BLENDFUNCTION { BYTE BlendOp{}, BlendFlags{}, SourceConstantAlpha{}, AlphaFormat{}; };
inline constexpr BOOL TRUE = 1, FALSE = 0;
inline constexpr HRESULT S_OK = 0, E_FAIL = -1;
inline bool SUCCEEDED(HRESULT hr) { return hr >= 0; }
inline bool FAILED(HRESULT hr) { return hr < 0; }
inline constexpr DWORD CLR_INVALID = 0xffffffffu;
inline constexpr DWORD BLACKNESS = 0x42, PATCOPY = 0xf00021, SRCCOPY = 0xcc0020, DSTINVERT = 0x550009;
inline constexpr UINT ETO_OPAQUE = 2, ETO_CLIPPED = 4, ETO_GLYPH_INDEX = 16, ETO_IGNORELANGUAGE = 0x1000;
inline constexpr int TRANSPARENT = 1, OPAQUE = 2;
inline constexpr int COLOR_WINDOW = 5, COLOR_BTNFACE = 15, COLOR_SCROLLBAR = 0, COLOR_HIGHLIGHT = 13, COLOR_MENUBAR = 30;
inline constexpr int GWL_STYLE = -16, ES_MULTILINE = 4;
inline constexpr DWORD WS_HSCROLL = 0x100000;
inline constexpr UINT BS_SOLID = 0, BS_HATCHED = 2, OBJ_BRUSH = 2, OBJ_FONT = 6, OBJ_MEMDC = 10, DC_BRUSH = 18;
inline constexpr int SM_CXVSCROLL = 2, SM_CYHSCROLL = 3, DBG_NORMAL = 1, DBG_IMPORTANT = 2;
inline constexpr DWORD GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS = 4, GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT = 2;
inline constexpr UINT CP_ACP = 0, BI_RGB = 0, DIB_RGB_COLORS = 0, EM_EXGETSEL = 0x400+52;
inline constexpr BYTE AC_SRC_OVER = 0, AC_SRC_ALPHA = 1;
inline constexpr COLORREF RGB(unsigned r, unsigned g, unsigned b) { return r | (g << 8) | (b << 16); }
inline BYTE GetRValue(COLORREF c) { return c & 255; }
inline BYTE GetGValue(COLORREF c) { return (c >> 8) & 255; }
inline BYTE GetBValue(COLORREF c) { return (c >> 16) & 255; }
namespace GlassShared
{
    enum class GlassControlKind { None, Edit, WinForms_Edit, RichEdit20W, WinForms_RichEdit20W, RichEdit50W, WinForms_RichEdit50W, Root };
}
struct TestWindow
{
    GlassShared::GlassControlKind kind = GlassShared::GlassControlKind::RichEdit50W;
    std::wstring cls = L"RICHEDIT50W";
    RECT client{0, 0, 80, 60};
    bool backdrop = true;
    DWORD style = 0;
    COLORREF memoColor = CLR_INVALID;
    CHARRANGE selected{};
};
using HWND = TestWindow*;
inline LRESULT SendMessageW(HWND w, UINT message, WPARAM, LPARAM lparam)
{
    assert(w && message == EM_EXGETSEL); // no locally invented background getter
    *reinterpret_cast<CHARRANGE*>(lparam) = w->selected; return 0;
}
struct TestObject
{
    enum Type { Brush, Bitmap, Font } type;
    explicit TestObject(Type t) : type(t) {}
};
using HGDIOBJ = TestObject*;
struct TestBrush : TestObject
{
    LOGBRUSH lb;
    BYTE alpha;
    TestBrush(COLORREF c = 0, UINT style = BS_SOLID, BYTE a = 255)
        : TestObject(Brush), lb{style, c}, alpha(a) {}
};
using HBRUSH = TestBrush*;
using Pixel = std::array<BYTE, 4>;
struct TestBitmap : TestObject
{
    int width, height;
    std::vector<Pixel> pixels;
    TestBitmap(int w = 1, int h = 1) : TestObject(Bitmap), width(w), height(h), pixels(w*h) {}
};
using HBITMAP = TestBitmap*;
struct TestFont : TestObject { TestFont() : TestObject(Font) {} };
using HFONT = TestFont*;
inline TestBitmap stockBitmap;
inline TestFont stockFont;
inline TestBrush stockDcBrush;
inline const HGDIOBJ HGDI_ERROR = reinterpret_cast<HGDIOBJ>(static_cast<std::intptr_t>(-1));
struct TestDC
{
    HWND owner{};
    TestBitmap surface{80, 60};
    HBITMAP bitmap = &surface;
    HFONT font = &stockFont;
    HBRUSH brush = &stockDcBrush;
    COLORREF background = RGB(255, 255, 255), text = 0, dcBrush = 0;
    int bkMode = OPAQUE;
    RECT clip{0, 0, 80, 60};
};
using HDC = TestDC*;
struct TestModule
{
    std::unordered_map<std::string, void*> originals, hooks;
    std::unordered_map<std::string, int> refs;
    std::unordered_set<std::string> failAcquire;
};
using HMODULE = TestModule*;
namespace RichTest
{
    inline std::unordered_map<std::wstring, HMODULE> modules;
    inline std::unordered_map<HDC, HWND> stackOwners;
    inline HWND host{}, topWindow{}, focus{};
    inline HDC currentGetDc{};
    inline HMODULE caller{};
    inline bool full = true;
    inline COLORREF windowColor = 0xffffff, faceColor = 0xf0f0f0, highlightColor = 0xd77800;
    inline bool failDc = false, failBitmap = false, failSelect = false, failBlend = false, failBlacken = false;
    inline bool failInk = false;
    inline int liveDcs = 0, liveBitmaps = 0, liveBrushes = 0;
    inline int fills = 0, pats = 0, blacks = 0, blends = 0, nativeTexts = 0, inkTexts = 0;
    inline int originalTag = 0;
    inline RECT lastRect{};
    inline Pixel blendPixel{};
    inline BLENDFUNCTION blend{};
    inline UINT nativeOptions{}, inkOptions{}, inkCount{};
    inline LPCWSTR inkText{};
    inline const INT* inkDx{};
    inline HDC inkDc{};
    inline void* inkOriginal{};
    inline BYTE inkBlend = 217;
    inline std::function<void()> duringBlend, duringInk;
}
inline BOOL IsRectEmpty(const RECT* r) { return r->right <= r->left || r->bottom <= r->top; }
inline BOOL IntersectRect(RECT* out, const RECT* a, const RECT* b)
{
    const RECT r{std::max(a->left, b->left), std::max(a->top, b->top), std::min(a->right, b->right), std::min(a->bottom, b->bottom)};
    *out = r; return !IsRectEmpty(out);
}
inline BOOL InflateRect(RECT* r, int x, int y)
{ r->left -= x; r->right += x; r->top -= y; r->bottom += y; return TRUE; }
inline BOOL IsWindow(HWND w) { return w != nullptr; }
inline BOOL GetClientRect(HWND w, LPRECT r) { if (!w) return FALSE; *r = w->client; return TRUE; }
inline LONG GetWindowLongW(HWND w, int) { return w->style; }
inline HWND WindowFromDC(HDC dc) { return dc ? dc->owner : nullptr; }
inline DWORD GetObjectType(HDC dc) { return dc ? (dc->owner ? 3 : OBJ_MEMDC) : 0; }
inline COLORREF GetSysColor(int index)
{
    if (index == COLOR_WINDOW) return RichTest::windowColor;
    if (index == COLOR_HIGHLIGHT) return RichTest::highlightColor;
    return RichTest::faceColor;
}
inline COLORREF GetBkColor(HDC dc) { return dc->background; }
inline COLORREF GetTextColor(HDC dc) { return dc->text; }
inline COLORREF SetBkColor(HDC dc, COLORREF c) { return std::exchange(dc->background, c); }
inline COLORREF SetTextColor(HDC dc, COLORREF c) { return std::exchange(dc->text, c); }
inline int GetBkMode(HDC dc) { return dc->bkMode; }
inline int SetBkMode(HDC dc, int m) { return std::exchange(dc->bkMode, m); }
inline COLORREF GetDCBrushColor(HDC dc) { return dc->dcBrush; }
inline HGDIOBJ GetStockObject(int) { return &stockDcBrush; }
inline int GetClassNameW(HWND w, LPWSTR name, int size)
{
    if (!w || size <= static_cast<int>(w->cls.size())) return 0;
    std::wcscpy(name, w->cls.c_str()); return w->cls.size();
}
inline HGDIOBJ GetCurrentObject(HDC dc, UINT type)
{ return type == OBJ_FONT ? static_cast<HGDIOBJ>(dc->font) : dc->brush; }
inline int GetObjectW(HBRUSH brush, int size, void* out)
{
    if (!brush || size < static_cast<int>(sizeof(LOGBRUSH))) return 0;
    std::memcpy(out, &brush->lb, sizeof(LOGBRUSH)); return sizeof(LOGBRUSH);
}
inline int GetObject(HBRUSH brush, int size, void* out) { return GetObjectW(brush, size, out); }
inline HGDIOBJ SelectObject(HDC dc, HGDIOBJ object)
{
    if (RichTest::failSelect) return HGDI_ERROR;
    assert(object && object != HGDI_ERROR);
    if (object->type == TestObject::Bitmap) return std::exchange(dc->bitmap, static_cast<HBITMAP>(object));
    if (object->type == TestObject::Font) return std::exchange(dc->font, static_cast<HFONT>(object));
    return std::exchange(dc->brush, static_cast<HBRUSH>(object));
}
inline HDC CreateCompatibleDC(HDC)
{
    if (RichTest::failDc) return nullptr;
    auto* dc = new TestDC; dc->bitmap = &stockBitmap; ++RichTest::liveDcs; return dc;
}
inline HBITMAP CreateDIBSection(HDC, const BITMAPINFO* bi, UINT, void** bits, void*, DWORD)
{
    assert(bi->bmiHeader.biPlanes == 1 && bi->bmiHeader.biBitCount == 32);
    if (RichTest::failBitmap) return nullptr;
    auto* bitmap = new TestBitmap(bi->bmiHeader.biWidth, std::abs(bi->bmiHeader.biHeight));
    *bits = bitmap->pixels.data(); ++RichTest::liveBitmaps; return bitmap;
}
inline HBITMAP CreateCompatibleBitmap(HDC, int w, int h)
{ auto* b = new TestBitmap(w, h); ++RichTest::liveBitmaps; return b; }
inline BOOL DeleteObject(HGDIOBJ object)
{
    assert(object);
    if (object->type == TestObject::Bitmap) { delete static_cast<HBITMAP>(object); --RichTest::liveBitmaps; }
    else if (object->type == TestObject::Brush) { delete static_cast<HBRUSH>(object); --RichTest::liveBrushes; }
    else assert(false);
    return TRUE;
}
inline BOOL DeleteDC(HDC dc) { delete dc; --RichTest::liveDcs; return TRUE; }
inline BOOL GdiFlush() { return TRUE; }
template <typename F> inline void Raster(HDC dc, const RECT& rect, F fn)
{
    RECT r{}, bounds{0, 0, dc->bitmap->width, dc->bitmap->height};
    if (!IntersectRect(&r, &rect, &bounds) || !IntersectRect(&r, &r, &dc->clip)) return;
    for (int y = r.top; y < r.bottom; ++y)
        for (int x = r.left; x < r.right; ++x) fn(dc->bitmap->pixels[y*dc->bitmap->width+x]);
}
inline BOOL PatBlt(HDC dc, int x, int y, int w, int h, DWORD rop)
{
    if (rop == BLACKNESS)
    {
        ++RichTest::blacks;
        if (RichTest::failBlacken) return FALSE;
        Raster(dc, RECT{x, y, x+w, y+h}, [](Pixel& p) { p = {}; });
    }
    else ++RichTest::pats;
    return TRUE;
}
inline BOOL GdiAlphaBlend(HDC dest, int x, int y, int w, int h, HDC src, int, int, int, int, BLENDFUNCTION bf)
{
    ++RichTest::blends; RichTest::lastRect = RECT{x, y, x+w, y+h};
    RichTest::blend = bf; RichTest::blendPixel = src->bitmap->pixels[0];
    if (RichTest::duringBlend) RichTest::duringBlend();
    if (RichTest::failBlend) return FALSE;
    const Pixel p = src->bitmap->pixels[0];
    Raster(dest, RichTest::lastRect, [&](Pixel& d) {
        for (size_t i = 0; i < 4; ++i) d[i] = std::min(255u, unsigned(p[i]) + (unsigned(d[i]) * (255u-p[3]) + 127u)/255u);
    });
    return TRUE;
}
inline BOOL BitBlt(HDC, int, int, int, int, HDC, int, int, DWORD) { return TRUE; }
inline int FillRect(HDC dc, const RECT* r, HBRUSH brush)
{
    ++RichTest::fills;
    if (r && brush)
    {
        COLORREF c{}; BYTE a = 255;
        const auto value = reinterpret_cast<ULONG_PTR>(brush);
        if (value <= COLOR_MENUBAR + 1u) c = GetSysColor(value-1);
        else { c = brush == &stockDcBrush ? dc->dcBrush : brush->lb.lbColor; a = brush->alpha; }
        Raster(dc, *r, [&](Pixel& p) { p = Pixel{GetBValue(c), GetGValue(c), GetRValue(c), a}; });
    }
    return 7;
}
inline BOOL ExtTextOutW(HDC dc, int, int, UINT options, const RECT* r, LPCWSTR, UINT, const INT*)
{
    ++RichTest::nativeTexts; RichTest::nativeOptions = options;
    if ((options & ETO_OPAQUE) && r) { TestBrush b{GetBkColor(dc)}; FillRect(dc, r, &b); }
    return TRUE;
}
inline BOOL ExtTextOutA(HDC dc, int x, int y, UINT options, const RECT* r, LPCSTR, UINT count, const INT* dx)
{ return ExtTextOutW(dc, x, y, options, r, nullptr, count, dx); }
inline int MultiByteToWideChar(UINT, DWORD, LPCSTR text, int count, LPWSTR out, int)
{ if (out) for (int i = 0; i < count; ++i) out[i] = static_cast<unsigned char>(text[i]); return count; }
inline UINT GetDpiForWindow(HWND) { return 96; }
inline int GetSystemMetricsForDpi(int, UINT) { return 17; }
inline int GetSystemMetrics(int) { return 17; }
inline HMODULE GetModuleHandleW(LPCWSTR name)
{ auto it = RichTest::modules.find(name); return it == RichTest::modules.end() ? nullptr : it->second; }
inline void* _ReturnAddress() { return RichTest::caller; }
inline BOOL GetModuleHandleExW(DWORD, LPCWSTR address, HMODULE* out)
{ *out = reinterpret_cast<HMODULE>(const_cast<wchar_t*>(address)); return *out != nullptr; }
inline FARPROC GetProcAddress(HMODULE, LPCSTR name)
{ return std::string_view(name) == "ExtTextOutW" ? reinterpret_cast<FARPROC>(&ExtTextOutW) : nullptr; }
inline HWND GetFocus() { return RichTest::focus; }
inline HWND GetParent(HWND) { return nullptr; }
inline ULONGLONG GetTickCount64() { return 1; }
inline int GetClipBox(HDC dc, RECT* r) { *r = dc->clip; return 2; }
inline UINT GetTextAlign(HDC) { return 0; }
inline USHORT CaptureStackBackTrace(DWORD, DWORD, void**, DWORD*) { return 0; }
template <typename... Args> inline void DebugLog(int, const wchar_t*, Args...) {}
namespace GlassShared
{
    inline GlassControlKind GlassGetWindowKind(HWND w) { return w ? w->kind : GlassControlKind::None; }
    inline bool GlassIsBackdropAllowedForHwnd(HWND w) { return w && w->backdrop; }
    inline COLORREF GetDelphiMemoBackground(HWND w) { return w->memoColor; }
}
namespace wil
{
    struct srwlock
    {
        struct Guard { ~Guard() {} };
        Guard lock_exclusive() { return {}; }
        Guard lock_shared() { return {}; }
    };
    template <typename F> struct ScopeExit { F fn; ~ScopeExit() { fn(); } };
    template <typename F> auto scope_exit(F fn) { return ScopeExit<F>{std::move(fn)}; }
    struct BrushDelete { void operator()(HBRUSH b) const { DeleteObject(b); } };
    using unique_hbrush = std::unique_ptr<TestBrush, BrushDelete>;
}
namespace Shared::WinApiUtils
{
    inline HBRUSH CreateSolidColorBrushWithAlpha(COLORREF c, BYTE a)
    { ++RichTest::liveBrushes; return new TestBrush(c, BS_SOLID, a); }
}
namespace GlassCore::Root { inline bool IsFullInjectionMode() { return RichTest::full; } }
namespace GlassCore::ModuleHandler
{
    inline HMODULE g_entryPointModule{};
    inline void RegisterModulePrewarmHandler(void(*)(HMODULE, const wchar_t*) noexcept) {}
}
namespace GlassCore::PaintDispatcher
{
    struct PaintProvider { void(*onBegin)(HWND, HMODULE); void(*onEnd)(HWND, HMODULE); };
    struct PostPaintProvider { bool(*onAfterBeginPaint)(HWND, HDC, LPPAINTSTRUCT, HMODULE) noexcept; };
    inline void RegisterPaintProvider(PaintProvider) {}
    inline void RegisterGetDCProvider(PaintProvider) {}
    inline void RegisterPostPaintProvider(PostPaintProvider) {}
    inline HWND GetPaintSessionHostHwnd() { return RichTest::host; }
    inline HDC GetCurrentGetDcSessionHdc() { return RichTest::currentGetDc; }
    inline HWND GetStackOwnerForDc(HDC dc)
    { auto it = RichTest::stackOwners.find(dc); return it == RichTest::stackOwners.end() ? nullptr : it->second; }
    inline bool IsPaintStackDcForHwnd(HWND w, HDC dc) { return GetStackOwnerForDc(dc) == w; }
    inline bool IsCurrentPaintDcForHwnd(HWND w, HDC dc) { return IsPaintStackDcForHwnd(w, dc); }
    inline bool IsTopWindowDrawingSession(HWND w) { return w == RichTest::topWindow; }
    inline HDC GetPaintSessionBaseWindowDc(HWND) { return nullptr; }
}
namespace GlassCore::DelphiInput
{
    inline bool IsSpinEdit(HWND w) { return w && w->cls == L"TSpinEdit"; }
    inline bool IsNumberBox(HWND w) { return w && w->cls == L"TNumberBox"; }
    inline bool IsHotKey(HWND w) { return w && w->cls == L"THotKey"; }
    inline bool ClearNumberBoxGlyphBackground(HWND, HDC, const RECT*, UINT) { return false; }
    inline bool ClearHotKeyBackground(HWND, HDC, const RECT*, HBRUSH) { return false; }
}
namespace GlassCore::TextInk { inline void EnsureTextInkClientRegistered(HWND) {} }
namespace GlassCore::Text
{
    inline BYTE g_ExtTextOutRenderMode = 0;
    struct Profile {};
    struct TextSessionBuffer { RGBQUAD* bits; int width, height; bool dual, overrideDual; };
    enum class ExtTextOutAlphaMode { FullQuality, EditSingleLinePlain };
    inline bool PlainEditUseDualPass(HWND, BYTE) { return true; }
    inline bool PlainEditUseDualPassSingleLinePlain(HWND, BYTE) { return true; }
    inline BYTE ResolveEffectiveTargetBlendAlpha(bool) { return RichTest::inkBlend; }
    inline const Profile& GetRuntimeInkCapsuleProfile(bool) { static Profile p; return p; }
    inline HRESULT ExtTextOutWithAlpha(HDC dc, int, int, UINT options, const RECT*, LPCWSTR text,
        UINT count, const INT* dx, decltype(&::ExtTextOutW) orig, ExtTextOutAlphaMode, HWND, const Profile*)
    {
        ++RichTest::inkTexts; RichTest::inkOptions = options; RichTest::inkCount = count;
        RichTest::inkText = text; RichTest::inkDc = dc; RichTest::inkDx = dx;
        RichTest::inkOriginal = reinterpret_cast<void*>(orig);
        if (RichTest::duringInk) RichTest::duringInk();
        return RichTest::failInk ? E_FAIL : S_OK;
    }
    inline HRESULT ExtTextOutWithAlphaToSession(HDC dc, int x, int y, UINT o, const RECT* r,
        LPCWSTR t, UINT c, const INT* dx, decltype(&::ExtTextOutW) orig, const TextSessionBuffer&, const Profile* p)
    { return ExtTextOutWithAlpha(dc, x, y, o, r, t, c, dx, orig, ExtTextOutAlphaMode::FullQuality, nullptr, p); }
    inline HRESULT ExtTextOutWithAlphaMultilinePlainGetDc(HDC dc, int x, int y, UINT o, const RECT* r,
        LPCWSTR t, UINT c, const INT* dx, decltype(&::ExtTextOutW) orig, bool, const Profile* p)
    { return ExtTextOutWithAlpha(dc, x, y, o, r, t, c, dx, orig, ExtTextOutAlphaMode::FullQuality, nullptr, p); }
}
namespace GlassCore::Hooks
{
    template <size_t Count, size_t Imports> using HookDispatcherDependency = std::array<
        std::tuple<std::array<std::string_view, Imports>, LPCSTR, void*>, Count>;
    template <size_t Count, size_t Imports> struct HookDispatcher
    {
        const HookDispatcherDependency<Count, Imports>& dep;
        void* moduleAddress{};
        std::array<int, Count> refs{};
        HookDispatcher(const HookDispatcherDependency<Count, Imports>& d, void* m) : dep(d), moduleAddress(m) {}
        void CacheHookData() {}
        int GetRef(size_t index) { return refs[index]; }
        bool EnableHook(size_t index, bool enable)
        {
            auto* m = static_cast<HMODULE>(moduleAddress);
            const auto name = std::get<1>(dep[index]);
            if (!m->originals.count(name) || (enable && m->failAcquire.count(name))) return false;
            if (!enable && !refs[index]) return false;
            const bool changed = enable ? refs[index] == 0 : refs[index] == 1;
            refs[index] += enable ? 1 : -1; m->refs[name] += enable ? 1 : -1;
            assert(refs[index] >= 0 && m->refs[name] >= 0);
            if (refs[index]) m->hooks[name] = std::get<2>(dep[index]);
            else m->hooks.erase(name);
            return changed; // Real dispatcher returns false for nested refs!
        }
        template <size_t I, typename Fn> Fn GetOrg()
        {
            auto* m = static_cast<HMODULE>(moduleAddress);
            return refs[I] ? reinterpret_cast<Fn>(m->originals.at(std::get<1>(dep[I]))) : nullptr;
        }
        void DisableAllHooks()
        { for (size_t i = 0; i < Count; ++i) while (refs[i]) EnableHook(i, false); }
    };
}
