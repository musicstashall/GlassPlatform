// Portable Win32/IAT test doubles for the real TCoolBarDraw/Subclass sources.
// This tests routing and state, not Windows rasterization or visual correctness.
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
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#define WINAPI
#define CALLBACK
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#define _wcsicmp wcscasecmp
using BOOL = int;
using UINT = unsigned int;
using LONG = std::int32_t;
using DWORD = std::uint32_t;
using COLORREF = DWORD;
using HRESULT = std::int32_t;
using ULONGLONG = std::uint64_t;
using INT = int;
using PVOID = void*;
using LPCSTR = const char*;
using LPCWSTR = const wchar_t*;
using LPWSTR = wchar_t*;
using LRESULT = std::intptr_t;
using WPARAM = std::uintptr_t;
using LPARAM = std::intptr_t;
using UINT_PTR = std::uintptr_t;
using DWORD_PTR = std::uintptr_t;
using HGDIOBJ = void*;
struct RECT { LONG left{}, top{}, right{}, bottom{}; };
using LPRECT = RECT*;
using LPCRECT = const RECT*;
struct LOGBRUSH { UINT lbStyle{}; COLORREF lbColor{}; std::uintptr_t lbHatch{}; };
struct LOGFONTW { int marker{}; };
struct DRAWTEXTPARAMS { UINT cbSize{}; };
using LPDRAWTEXTPARAMS = DRAWTEXTPARAMS*;
struct TestWindow;
struct TestDC;
struct TestModule;
struct TestTheme {};
struct TestFont { int marker{}; };
using HWND = TestWindow*;
using HDC = TestDC*;
using HMODULE = TestModule*;
using HTHEME = TestTheme*;
using HFONT = TestFont*;
using HBRUSH = LOGBRUSH*;
using SUBCLASSPROC = LRESULT (CALLBACK*)(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
inline constexpr BOOL TRUE = 1, FALSE = 0;
inline constexpr HRESULT S_OK = 0, S_FALSE = 1, E_FAIL = -1;
inline bool SUCCEEDED(HRESULT hr) { return hr >= 0; }
inline constexpr int TRANSPARENT = 1, OPAQUE = 2;
inline constexpr UINT ETO_OPAQUE = 2, ETO_CLIPPED = 4, ETO_GLYPH_INDEX = 16;
inline constexpr UINT DT_CALCRECT = 0x400, DT_MODIFYSTRING = 0x10000;
inline constexpr DWORD BLACKNESS = 0x42, WHITENESS = 0xff0062, PATCOPY = 0xf00021;
inline constexpr UINT BS_SOLID = 0, OBJ_BRUSH = 2, OBJ_MEMDC = 10;
inline constexpr int COLOR_WINDOW = 5, COLOR_BTNFACE = 15, COLOR_3DFACE = 15;
inline constexpr int SM_CXEDGE = 45, SM_CYEDGE = 46, TMT_FONT = 210, TMT_TEXTCOLOR = 3803;
inline constexpr UINT WM_PAINT = 0xf, WM_ERASEBKGND = 0x14, WM_PRINTCLIENT = 0x318, WM_NCDESTROY = 0x82;
inline constexpr UINT SMTO_ABORTIFHUNG = 2;
inline constexpr int DBG_NORMAL = 1, DBG_WARNING = 2;
inline constexpr COLORREF RGB(unsigned r, unsigned g, unsigned b) { return r | (g << 8) | (b << 16); }

namespace GlassShared { enum class GlassControlKind { None, CoolBar, Root, Toolbar, ListView, WinForms_ListView, ListBox, ComboBox }; }
struct TestWindow
{
    bool valid = true, backdrop = true;
    // Raw class from the Windows probe, initially unmarked: tests must attach
    // through the real class gate, not assume the rendering path is active.
    GlassShared::GlassControlKind kind = GlassShared::GlassControlKind::None;
    std::wstring cls = L"TCoolBar";
    SUBCLASSPROC subclass{};
    HTHEME theme{};
    std::vector<HWND> children;
};
struct DcState
{
    int bkMode = OPAQUE;
    COLORREF text = 0, background = 0xffffff;
    HBRUSH brush{};
    HFONT font{};
};
struct TestDC : DcState
{
    HWND owner{};
    std::vector<DcState> saved;
};
struct TestModule
{
    std::unordered_map<std::string, void*> originals, hooks;
    std::unordered_map<std::string, int> refs;
};

namespace Test
{
    inline HMODULE ux{};
    inline HWND host{}, topWindow{};
    inline std::unordered_map<HDC, HWND> stackOwners;
    inline bool full = true, shuttingDown = false;
    inline int alphaText = 0, alphaGlyph = 0, nativeText = 0, nativeTheme = 0;
    inline int nativeExt = 0, nativeFill = 0, nativePaint = 0, black = 0;
    inline DWORD lastRop{};
    inline int alphaBkMode{};
    inline COLORREF alphaTextColor{};
    inline HFONT alphaFont{};
    inline UINT glyphOptions{}, glyphCount{};
    inline const INT* glyphDx{};
    inline void* glyphOriginal{};
    inline HRESULT alphaResult = S_OK;
    inline std::function<void()> duringAlpha;
    inline std::function<void(HWND, UINT, WPARAM, LPARAM)> duringNativePaint;
}

inline BOOL IsWindow(HWND w) { return w && w->valid; }
inline HWND WindowFromDC(HDC dc) { return dc ? dc->owner : nullptr; }
inline DWORD GetObjectType(HDC dc) { return dc ? (dc->owner ? 3 : OBJ_MEMDC) : 0; }
inline COLORREF GetSysColor(int index) { return index == COLOR_WINDOW ? 0xffffff : 0xf0f0f0; }
inline int GetObjectW(HBRUSH brush, int bytes, void* out)
{
    if (!brush || bytes < static_cast<int>(sizeof(*brush))) return 0;
    std::memcpy(out, brush, sizeof(*brush)); return sizeof(*brush);
}
inline HGDIOBJ GetCurrentObject(HDC dc, UINT) { return dc->brush; }
inline COLORREF GetTextColor(HDC dc) { return dc ? dc->text : 0; }
inline COLORREF GetBkColor(HDC dc) { return dc ? dc->background : 0; }
inline int GetBkMode(HDC dc) { return dc ? dc->bkMode : 0; }
inline int SetBkMode(HDC dc, int mode) { return std::exchange(dc->bkMode, mode); }
inline COLORREF SetTextColor(HDC dc, COLORREF color) { return std::exchange(dc->text, color); }
inline int SaveDC(HDC dc) { dc->saved.push_back(*dc); return dc->saved.size(); }
inline BOOL RestoreDC(HDC dc, int saved)
{
    assert(saved > 0 && saved <= static_cast<int>(dc->saved.size()));
    static_cast<DcState&>(*dc) = dc->saved[saved - 1]; dc->saved.resize(saved - 1); return TRUE;
}
inline HFONT CreateFontIndirectW(const LOGFONTW* font) { return new TestFont{font->marker}; }
inline HGDIOBJ SelectObject(HDC dc, HFONT font) { return std::exchange(dc->font, font); }
inline BOOL IsRectEmpty(const RECT* rc) { return rc->right <= rc->left || rc->bottom <= rc->top; }
inline UINT GetDpiForWindow(HWND) { return 96; }
inline int GetSystemMetricsForDpi(int, UINT dpi) { return 2 * dpi / 96; }
inline ULONGLONG GetTickCount64() { return 100; }
inline HMODULE GetModuleHandleW(LPCWSTR name) { return std::wstring_view(name) == L"uxtheme.dll" ? Test::ux : nullptr; }
inline HTHEME GetWindowTheme(HWND w) { return w->theme; }
inline HRESULT GetThemeFont(HTHEME, HDC, int, int, int, LOGFONTW* font) { font->marker = 99; return S_OK; }
inline HRESULT GetThemeColor(HTHEME, int, int, int, COLORREF* color) { *color = RGB(12, 34, 56); return S_OK; }
template <typename... Args> inline void DebugLog(int, const wchar_t*, Args...) {}

inline BOOL ExtTextOutW(HDC dc, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*)
{ assert(dc); ++Test::nativeExt; return TRUE; }
inline int FillRect(HDC, const RECT*, HBRUSH) { return ++Test::nativeFill; }
inline BOOL PatBlt(HDC, int, int, int, int, DWORD rop)
{ Test::lastRop = rop; if (rop == BLACKNESS) ++Test::black; return TRUE; }
inline int DrawTextW(HDC, LPCWSTR, int, LPRECT, UINT) { ++Test::nativeText; return 17; }
inline int DrawTextExW(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS) { ++Test::nativeText; return 18; }
inline HRESULT DrawThemeText(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, DWORD, LPCRECT)
{ ++Test::nativeTheme; return S_FALSE; }

inline int GetClassNameW(HWND w, LPWSTR text, int capacity)
{
    if (!IsWindow(w) || capacity <= static_cast<int>(w->cls.size())) return 0;
    std::wcscpy(text, w->cls.c_str()); return w->cls.size();
}
inline BOOL GetClientRect(HWND, LPRECT rc) { *rc = RECT{0, 0, 500, 40}; return TRUE; }
inline BOOL SetWindowSubclass(HWND w, SUBCLASSPROC proc, UINT_PTR, DWORD_PTR)
{ w->subclass = proc; return TRUE; }
inline BOOL RemoveWindowSubclass(HWND w, SUBCLASSPROC proc, UINT_PTR)
{ if (w->subclass != proc) return FALSE; w->subclass = nullptr; return TRUE; }
inline LRESULT DefSubclassProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    ++Test::nativePaint;
    if (Test::duringNativePaint) Test::duringNativePaint(w, msg, wp, lp);
    return 777;
}
inline LRESULT SendMessageTimeoutW(HWND w, UINT msg, WPARAM wp, LPARAM lp, UINT, UINT, DWORD_PTR*)
{ return w->subclass ? w->subclass(w, msg, wp, lp, 1, 0) : 0; }
inline BOOL EnumChildWindows(HWND root, BOOL(CALLBACK* visit)(HWND, LPARAM), LPARAM data)
{
    for (HWND child : root->children)
    {
        if (!visit(child, data) || !EnumChildWindows(child, visit, data)) return FALSE;
    }
    return TRUE;
}

namespace Shared { enum class InjectionMode { Full, PopupOnly }; }
namespace WndCompat
{
    inline constexpr UINT WM_GLASS_DETACH_COOLBAR = 0x8225;
    inline bool IsShuttingDown() { return Test::shuttingDown; }
    inline Shared::InjectionMode WinEventHook_GetMode()
    { return Test::full ? Shared::InjectionMode::Full : Shared::InjectionMode::PopupOnly; }
    inline void WinEventHook_SignalDetachComplete() {}
}
namespace GlassShared
{
    inline bool GlassIsBackdropAllowedForHwnd(HWND w) { return w && w->backdrop; }
    inline GlassControlKind GlassGetWindowKind(HWND w) { return w->kind; }
    inline void GlassMarkWindow(HWND w, GlassControlKind kind) { w->kind = kind; }
    inline void GlassUnmarkWindow(HWND w) { w->kind = GlassControlKind::None; }
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
    // Match WIL lambda_call's forwarding deduction and constructor checks:
    // a by-value double silently accepted lvalue callbacks that real WIL rejects.
    template <typename F> auto scope_exit(F&& fn)
    {
        static_assert(std::is_same_v<decltype(fn()), void>, "scope_exit lambdas must not have a return value");
        static_assert(!std::is_reference_v<F>, "scope_exit should only be directly used with a lambda");
        return ScopeExit<F>{std::forward<F>(fn)};
    }
    using unique_hfont = std::unique_ptr<TestFont>;
}
namespace GlassCore::ModuleHandler
{
    inline HMODULE g_comctl32New{}, g_comctl32Legacy{}, g_entryPointModule{};
}
namespace GlassCore::Root { inline bool IsFullInjectionMode() { return Test::full; } }
namespace GlassCore::PaintDispatcher
{
    inline bool IsTopWindowDrawingSession(HWND w) { return Test::topWindow == w; }
    inline bool IsPaintStackDcForHwnd(HWND w, HDC dc)
    { auto it = Test::stackOwners.find(dc); return it != Test::stackOwners.end() && it->second == w; }
    inline HWND GetPaintSessionHostHwnd() { return Test::host; }
    inline HWND GetStackOwnerForDc(HDC dc)
    { auto it = Test::stackOwners.find(dc); return it != Test::stackOwners.end() ? it->second : nullptr; }
}
namespace GlassCore::Text
{
    inline HRESULT DrawTextWithAlpha(HDC dc, LPCWSTR, int, LPRECT, UINT, int& result, bool transparent)
    {
        assert(transparent);
        ++Test::alphaText; Test::alphaBkMode = dc->bkMode;
        Test::alphaTextColor = dc->text; Test::alphaFont = dc->font;
        if (Test::duringAlpha) Test::duringAlpha();
        result = 15; return Test::alphaResult;
    }
    inline HRESULT ExtTextOutWithAlpha(HDC dc, int, int, UINT options, const RECT*, LPCWSTR,
        UINT count, const INT* dx, decltype(&::ExtTextOutW) orig)
    {
        ++Test::alphaGlyph; Test::alphaBkMode = dc->bkMode;
        Test::glyphOptions = options; Test::glyphDx = dx; Test::glyphCount = count;
        Test::glyphOriginal = reinterpret_cast<void*>(orig); return Test::alphaResult;
    }
}
namespace GlassCore::Hooks
{
    template <size_t Count, size_t Imports> using HookDispatcherDependency = std::array<
        std::tuple<std::array<std::string_view, Imports>, LPCSTR, void*>, Count>;
    template <size_t Count, size_t Imports> struct HookDispatcher
    {
        const HookDispatcherDependency<Count, Imports>& dependency;
        void* moduleAddress{};
        std::array<int, Count> refs{};
        HookDispatcher(const HookDispatcherDependency<Count, Imports>& dep, void* base)
            : dependency(dep), moduleAddress(base) {}
        void CacheHookData() {}
        int GetRef(size_t i) { return refs[i]; }
        bool EnableHook(size_t i, bool enable)
        {
            auto* mod = static_cast<HMODULE>(moduleAddress);
            const auto name = std::get<1>(dependency[i]);
            if (!mod->originals.count(name)) return false;
            if (enable)
            {
                ++refs[i]; ++mod->refs[name];
                mod->hooks[name] = std::get<2>(dependency[i]);
            }
            else
            {
                assert(refs[i] > 0 && mod->refs[name] > 0);
                --refs[i]; --mod->refs[name];
                if (!refs[i]) mod->hooks.erase(name);
            }
            return true;
        }
        template <size_t I, typename Fn> Fn GetOrg()
        {
            auto* mod = static_cast<HMODULE>(moduleAddress);
            const auto name = std::get<1>(dependency[I]);
            return refs[I] ? reinterpret_cast<Fn>(mod->originals.at(name)) : nullptr;
        }
        void DisableAllHooks()
        { for (size_t i = 0; i < Count; ++i) while (refs[i]) EnableHook(i, false); }
    };
}
