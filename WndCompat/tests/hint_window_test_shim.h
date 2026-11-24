// Environment doubles for the production GlassCore THintWindowDraw unit and the
// WndCompat bridge that calls it (Popup/Tooltip/DelphiHintPaint.h). They model
// what the two can see and do: the import table of the EXE (DrawText and
// SystemParametersInfo, wide and ANSI), a window with a class, an instance, a
// thread, a frame and a client, DCs that tell their kind, their window, their
// mapping and what was excluded from their clip, and everything that is drawn,
// in order. There are no threads: a test says which one is calling.
// This is not Windows GDI, DWM, the VCL, AnimateWindow, the real PE import
// table or the real subclass chain.
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
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
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
using ULONGLONG = std::uint64_t;
using WCHAR = wchar_t;
using LPCSTR = const char*;
using LPCWSTR = const wchar_t*;
using LPWSTR = wchar_t*;
using PVOID = void*;
using HRESULT = std::int32_t;
using LRESULT = std::intptr_t;
using WPARAM = std::uintptr_t;
using LPARAM = std::intptr_t;
using LONG_PTR = std::intptr_t;
using ULONG_PTR = std::uintptr_t;
using FARPROC = void (*)();
struct RECT { LONG left{}, top{}, right{}, bottom{}; };
using LPRECT = RECT*;
struct POINT { LONG x{}, y{}; };
struct CHARSETINFO { UINT ciCharset{}; UINT ciACP{}; };
struct TestWindow;
struct TestDC;
struct TestModule;
using HWND = TestWindow*;
using HDC = TestDC*;
using HMODULE = TestModule*;
inline constexpr BOOL TRUE = 1, FALSE = 0;
inline constexpr HRESULT S_OK = 0, E_FAIL = -1;
inline bool SUCCEEDED(HRESULT hr) { return hr >= 0; }
inline constexpr UINT WM_PAINT = 0x000f, WM_ERASEBKGND = 0x0014, WM_NCPAINT = 0x0085;
inline constexpr UINT WM_PRINT = 0x0317, WM_PRINTCLIENT = 0x0318, WM_SETTEXT = 0x000c;
inline constexpr UINT DT_WORDBREAK = 0x10, DT_NOCLIP = 0x100, DT_CALCRECT = 0x400, DT_NOPREFIX = 0x800;
inline constexpr UINT DT_MODIFYSTRING = 0x10000, DT_RTLREADING = 0x20000;
inline constexpr DWORD OBJ_DC = 3, OBJ_MEMDC = 10;
inline constexpr int MM_TEXT = 1, MM_ANISOTROPIC = 8;
inline constexpr int ERROR = 0, NULLREGION = 1, SIMPLEREGION = 2, COMPLEXREGION = 3;
inline constexpr int TRANSPARENT = 1, OPAQUE = 2;
inline constexpr DWORD BLACKNESS = 0x42;
inline constexpr int GWLP_HINSTANCE = -6, GWL_EXSTYLE = -20;
inline constexpr LONG_PTR WS_EX_LAYOUTRTL = 0x00400000, WS_EX_TOOLWINDOW = 0x00000080, WS_EX_RTLREADING = 0x00002000;
inline constexpr UINT SPI_GETTOOLTIPANIMATION = 0x1016, SPI_GETTOOLTIPFADE = 0x1018, SPI_GETMENUANIMATION = 0x1002;
inline constexpr UINT CP_ACP = 0;
inline constexpr int ANSI_CHARSET = 0, DEFAULT_CHARSET = 1, SYMBOL_CHARSET = 2, RUSSIAN_CHARSET = 204;
inline constexpr DWORD TCI_SRCCHARSET = 1;
inline constexpr int DBG_NORMAL = 0;

struct TestWindow
{
    std::wstring cls = L"THintWindow";
    bool valid = true;
    DWORD thread = 1;
    HMODULE instance{};                 // who registered the window class
    LONG_PTR ex = WS_EX_TOOLWINDOW;
    RECT screen{300, 200, 415, 221};    // the window rectangle: 115 x 21
    LONG frame = 1;                     // WS_BORDER
    // What VCL keeps for it.
    std::wstring caption = L"Open a file";
    std::string ansiCaption = "Open a file";
    COLORREF color = 0x00e1ffff;        // clInfoBk
    COLORREF fontColor = 0x00000000;    // Screen.HintFont.Color
    int charset = DEFAULT_CHARSET;
    bool themed = true;                 // THintWindow.Paint draws the tooltip face first
    bool erasePending = false;          // the update region carries an erase: BeginPaint sends it
    UINT textFlags = DT_NOPREFIX | DT_WORDBREAK;
    RECT Client() const { return RECT{0, 0, screen.right - screen.left - 2 * frame, screen.bottom - screen.top - 2 * frame}; }
};
struct DcState
{
    int bkMode = OPAQUE;
    COLORREF text = 0x00123456;
    std::vector<RECT> excluded;         // what was taken out of the clip
};
struct TestDC : DcState
{
    HWND owner{};                       // none: a memory DC
    bool whole = false;                 // a window DC: the frame too
    int map = MM_TEXT;
    POINT viewport{}, window{};
    int charset = DEFAULT_CHARSET;
    std::vector<DcState> saved;
};
struct TestModule
{
    std::unordered_map<std::string, void*> originals, hooks;
    std::unordered_map<std::string, int> refs;
};

namespace HintTest
{
    inline thread_local DWORD error{};
    inline DWORD thread = 1;                            // the calling thread
    inline ULONGLONG tick = 1000;
    inline bool disabled = false;                       // tooltip customization is off
    inline COLORREF tooltipColor = 0x001a1a1a;          // the text colour of Glass tooltips
    // Failures.
    inline bool failBlack = false, failSave = false, failExclude = false, failWindowDc = false;
    inline HWND failClient{}, failRect{}, failOrigin{};
    inline bool failViewport = false, failOrg = false;
    inline bool failConversion = false;                 // the buffer of a converted caption cannot be filled
    inline std::unordered_map<HMODULE, std::unordered_set<std::string>> registrationFailures;
    // The compositor.
    inline HRESULT alphaResult = S_OK;
    inline int alphaHeight = 15;
    inline bool alphaThrows = false, alphaMovesRect = true;
    inline std::function<void()> duringAlpha;
    // What the system says about tooltips.
    inline BOOL animation = TRUE, fade = TRUE, parametersResult = TRUE;
    inline int parameterCalls = 0, directParametersW = 0, directParametersA = 0;
    // The window DCs the unit took and gave back.
    inline std::vector<HDC> liveDcs;
    inline int dcsTaken = 0, dcsReleased = 0;
    // The locks: neither is taken while it is held.
    inline bool exclusive = false;
    inline int shared = 0, exclusiveLocks = 0;
    // What another thread does before this one gets the exclusive lock it asked for. Once.
    inline std::function<void()> beforeExclusive;

    // Everything that is drawn, in order.
    enum class Kind { Black, Fill, Face, Edge, TextW, TextA, DirectTextW, DirectTextA, Alpha };
    struct Event
    {
        Event(Kind k, HDC d, const RECT& r) : kind(k), dc(d), rect(r) {}
        Kind kind{};
        HDC dc{};
        RECT rect{};
        std::wstring text;              // of a wide or composed caption
        std::string bytes;              // of an ANSI one
        int count{};
        UINT flags{};
        COLORREF color{};               // of a fill, or the text colour of the DC
        int bkMode{};
        std::vector<RECT> excluded;     // the clip of the DC at that moment
        RECT* rectPointer{};
        DWORD entryError{};
    };
    inline std::vector<Event> events;
    inline int Count(Kind kind)
    { return static_cast<int>(std::count_if(events.begin(), events.end(), [=](const Event& e) { return e.kind == kind; })); }
    struct Log { std::wstring format; std::vector<long long> numbers; };
    inline std::vector<Log> logs;

    // The bridge.
    inline bool shuttingDown = false;
    inline HMODULE core{};
    inline FARPROC beginExport{}, endExport{};
    inline std::function<LRESULT(HWND, UINT, WPARAM, LPARAM)> windowProcedure;
}

inline DWORD GetLastError() { return HintTest::error; }
inline void SetLastError(DWORD error) { HintTest::error = error; }
inline BOOL IsWindow(HWND w) { return w && w->valid; }
inline int GetClassNameW(HWND w, LPWSTR text, int capacity)
{
    SetLastError(0xc106);               // any API may leave a last error of its own
    if (!IsWindow(w) || capacity <= static_cast<int>(w->cls.size())) return 0;
    std::wcscpy(text, w->cls.c_str()); return static_cast<int>(w->cls.size());
}
inline DWORD GetCurrentThreadId() { return HintTest::thread; }
inline DWORD GetWindowThreadProcessId(HWND w, DWORD* process) { assert(!process); return IsWindow(w) ? w->thread : 0; }
inline LONG_PTR GetWindowLongPtrW(HWND w, int index)
{
    assert(index == GWLP_HINSTANCE || index == GWL_EXSTYLE);
    if (!IsWindow(w)) return 0;
    return index == GWL_EXSTYLE ? w->ex : reinterpret_cast<LONG_PTR>(w->instance);
}
inline BOOL GetClientRect(HWND w, RECT* rc)
{ if (!IsWindow(w) || w == HintTest::failClient) return FALSE; *rc = w->Client(); return TRUE; }
inline BOOL GetWindowRect(HWND w, RECT* rc)
{ if (!IsWindow(w) || w == HintTest::failRect) return FALSE; *rc = w->screen; return TRUE; }
inline BOOL ClientToScreen(HWND w, POINT* point)
{
    if (!IsWindow(w) || w == HintTest::failOrigin) return FALSE;
    point->x += w->screen.left + w->frame; point->y += w->screen.top + w->frame;
    return TRUE;
}
inline BOOL IsRectEmpty(const RECT* rc) { return rc->right <= rc->left || rc->bottom <= rc->top; }
inline DWORD GetObjectType(HDC dc) { return !dc ? 0 : dc->owner ? OBJ_DC : OBJ_MEMDC; }
inline HWND WindowFromDC(HDC dc) { return dc ? dc->owner : nullptr; }
inline int GetMapMode(HDC dc) { return dc->map; }
inline BOOL GetViewportOrgEx(HDC dc, POINT* out) { if (HintTest::failViewport) return FALSE; *out = dc->viewport; return TRUE; }
inline BOOL GetWindowOrgEx(HDC dc, POINT* out) { if (HintTest::failOrg) return FALSE; *out = dc->window; return TRUE; }
inline int SetBkMode(HDC dc, int mode) { return std::exchange(dc->bkMode, mode); }
inline COLORREF SetTextColor(HDC dc, COLORREF color) { return std::exchange(dc->text, color); }
inline int SaveDC(HDC dc)
{
    if (HintTest::failSave) return 0;
    dc->saved.push_back(*dc); return static_cast<int>(dc->saved.size());
}
inline BOOL RestoreDC(HDC dc, int saved)
{
    assert(saved > 0 && saved == static_cast<int>(dc->saved.size()));
    static_cast<DcState&>(*dc) = dc->saved.back(); dc->saved.pop_back(); return TRUE;
}
inline int ExcludeClipRect(HDC dc, int l, int t, int r, int b)
{
    if (HintTest::failExclude) return ERROR;
    dc->excluded.push_back(RECT{l, t, r, b}); return COMPLEXREGION;
}
inline BOOL PatBlt(HDC dc, int x, int y, int w, int h, DWORD rop)
{
    assert(dc && rop == BLACKNESS);
    SetLastError(0xc101);
    if (HintTest::failBlack) return FALSE;
    HintTest::Event e(HintTest::Kind::Black, dc, RECT{x, y, x + w, y + h});
    e.excluded = dc->excluded;
    HintTest::events.push_back(e);
    return TRUE;
}
inline HDC GetWindowDC(HWND w)
{
    if (HintTest::failWindowDc || !IsWindow(w)) return nullptr;
    auto* dc = new TestDC; dc->owner = w; dc->whole = true;
    HintTest::liveDcs.push_back(dc); ++HintTest::dcsTaken;
    return dc;
}
inline int ReleaseDC(HWND w, HDC dc)
{
    const auto it = std::find(HintTest::liveDcs.begin(), HintTest::liveDcs.end(), dc);
    assert(it != HintTest::liveDcs.end() && dc->owner == w);
    assert(dc->saved.empty() && dc->excluded.empty());     // given back as it was taken
    HintTest::liveDcs.erase(it); ++HintTest::dcsReleased;
    delete dc;
    return 1;
}
inline ULONGLONG GetTickCount64() { return HintTest::tick; }

// GDI's conversion of ANSI text: the charset of the font names the code page.
inline int GetTextCharset(HDC dc) { return dc->charset; }
inline BOOL TranslateCharsetInfo(DWORD* source, CHARSETINFO* out, DWORD flags)
{
    assert(flags == TCI_SRCCHARSET);
    const auto charset = static_cast<int>(reinterpret_cast<ULONG_PTR>(source));
    if (charset == RUSSIAN_CHARSET) { out->ciACP = 1251; return TRUE; }
    if (charset == ANSI_CHARSET) { out->ciACP = 1252; return TRUE; }
    if (charset == SYMBOL_CHARSET) { out->ciACP = 42; return TRUE; }    // CP_SYMBOL: not text
    return FALSE;
}
// 1251: 0xC0..0xFF are U+0410..U+044F. Everything else is widened as it is.
// Any negative count means a terminated string, and the terminator is counted.
// The symbol code page is never asked for: the unit leaves such a caption alone.
inline int MultiByteToWideChar(UINT codepage, DWORD flags, LPCSTR text, int count, LPWSTR out, int capacity)
{
    assert(!flags && (codepage == CP_ACP || codepage == 1251 || codepage == 1252));
    const int length = count < 0 ? static_cast<int>(std::strlen(text)) + 1 : count;
    if (!out) return length;
    if (HintTest::failConversion) throw std::bad_alloc();
    if (capacity < length) return 0;
    for (int i = 0; i < length; ++i)
    {
        const auto byte = static_cast<unsigned char>(text[i]);
        out[i] = codepage == 1251 && byte >= 0xc0 ? static_cast<wchar_t>(0x0410 + (byte - 0xc0)) : static_cast<wchar_t>(byte);
    }
    return length;
}

// The true functions: what an import reaches through the original a hook was
// given (TextW / TextA), and what the unit reaches when it calls by name.
inline int RecordText(HintTest::Kind kind, HDC dc, LPCWSTR wide, LPCSTR ansi, int count, LPRECT rect, UINT flags, int result)
{
    HintTest::Event e(kind, dc, rect ? *rect : RECT{});
    if (wide) e.text = wide;
    if (ansi) e.bytes = ansi;
    e.count = count; e.flags = flags; e.rectPointer = rect; e.entryError = GetLastError();
    if (dc) { e.color = dc->text; e.bkMode = dc->bkMode; e.excluded = dc->excluded; }
    HintTest::events.push_back(e);
    SetLastError(0xc102);
    return result;
}
inline int NativeDrawTextW(HDC dc, LPCWSTR text, int count, LPRECT rect, UINT flags)
{ return RecordText(HintTest::Kind::TextW, dc, text, nullptr, count, rect, flags, 41); }
inline int NativeDrawTextA(HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
{ return RecordText(HintTest::Kind::TextA, dc, nullptr, text, count, rect, flags, 42); }
inline int DrawTextW(HDC dc, LPCWSTR text, int count, LPRECT rect, UINT flags)
{ return RecordText(HintTest::Kind::DirectTextW, dc, text, nullptr, count, rect, flags, 43); }
inline int DrawTextA(HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
{ return RecordText(HintTest::Kind::DirectTextA, dc, nullptr, text, count, rect, flags, 44); }
inline BOOL NativeParameters(UINT action, UINT, PVOID data, UINT)
{
    ++HintTest::parameterCalls;
    SetLastError(0xc103);
    if (!HintTest::parametersResult) return FALSE;
    if (data && action == SPI_GETTOOLTIPANIMATION) *static_cast<BOOL*>(data) = HintTest::animation;
    if (data && action == SPI_GETTOOLTIPFADE) *static_cast<BOOL*>(data) = HintTest::fade;
    return TRUE;
}
inline BOOL SystemParametersInfoW(UINT action, UINT param, PVOID data, UINT flags)
{ ++HintTest::directParametersW; return NativeParameters(action, param, data, flags); }
inline BOOL SystemParametersInfoA(UINT action, UINT param, PVOID data, UINT flags)
{ ++HintTest::directParametersA; return NativeParameters(action, param, data, flags); }

template <typename... Args> inline void DebugLog(int, const wchar_t* format, Args... args)
{
    HintTest::Log log{format, {}};
    const auto capture = [&](auto value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_pointer_v<T>) log.numbers.push_back(static_cast<long long>(reinterpret_cast<std::intptr_t>(value)));
        else log.numbers.push_back(static_cast<long long>(value));
    };
    (capture(args), ...);
    HintTest::logs.push_back(std::move(log));
    SetLastError(0xc104);
}

namespace wil
{
    struct srwlock
    {
        struct Exclusive { ~Exclusive() { HintTest::exclusive = false; } };
        struct Shared { ~Shared() { --HintTest::shared; } };
        Exclusive lock_exclusive()
        {
            if (auto first = std::exchange(HintTest::beforeExclusive, {})) first();
            assert(!HintTest::exclusive && !HintTest::shared);     // not recursive
            HintTest::exclusive = true; ++HintTest::exclusiveLocks;
            return {};
        }
        Shared lock_shared() { assert(!HintTest::exclusive); ++HintTest::shared; return {}; }
    };
    template <typename F> struct ScopeExit
    {
        F fn; bool active = true;
        explicit ScopeExit(F&& f) : fn(std::move(f)) {}
        ScopeExit(ScopeExit&& other) : fn(std::move(other.fn)), active(std::exchange(other.active, false)) {}
        ScopeExit(const ScopeExit&) = delete;
        ScopeExit& operator=(const ScopeExit&) = delete;
        ~ScopeExit() { if (active) fn(); }
    };
    template <typename F> auto scope_exit(F&& fn)
    {
        // As in WIL: the callable is taken by value, not referred to.
        static_assert(!std::is_lvalue_reference_v<F>, "scope_exit takes an rvalue callable");
        return ScopeExit<std::decay_t<F>>(std::forward<F>(fn));
    }
}
namespace GlassCore::ModuleHandler { inline HMODULE g_entryPointModule{}; }
namespace GlassCore::Settings { inline bool IsTooltipCustomizationDisabled() noexcept { return HintTest::disabled; } }
namespace GlassCore::Tooltip::Detail
{
    struct Rendering { COLORREF color{}; };
    struct RuntimeContext { Rendering rendering{}; };
    inline RuntimeContext& Context() noexcept
    {
        static RuntimeContext context;
        context.rendering.color = HintTest::tooltipColor;
        return context;
    }
}
namespace GlassCore::Text
{
    inline HRESULT DrawTextWithAlpha(HDC dc, LPCWSTR text, int count, LPRECT rect, UINT flags, int& result)
    {
        HintTest::Event e(HintTest::Kind::Alpha, dc, *rect);
        e.text = count < 0 ? std::wstring(text) : std::wstring(text, static_cast<size_t>(count));
        e.count = count; e.flags = flags; e.color = dc->text; e.bkMode = dc->bkMode;
        e.excluded = dc->excluded; e.rectPointer = rect; e.entryError = GetLastError();
        HintTest::events.push_back(e);
        if (HintTest::duringAlpha) HintTest::duringAlpha();
        // The compositor works on the rectangle it was given, and leaves its own last error.
        if (HintTest::alphaMovesRect) *rect = RECT{-1, -2, -3, -4};
        SetLastError(0xc105);
        if (HintTest::alphaThrows) throw std::runtime_error("injected renderer failure");
        result = HintTest::alphaHeight;
        return HintTest::alphaResult;
    }
}
namespace GlassCore::Hooks
{
    template <size_t Count, size_t Imports> using HookDispatcherDependency = std::array<
        std::tuple<std::array<std::string_view, Imports>, LPCSTR, void*>, Count>;
    // One import slot per name in the module; refs as the production dispatcher
    // counts them. EnableHook reports only a change of the slot, as it does
    // there; a slot that cannot be registered takes no reference.
    template <size_t Count, size_t Imports> struct HookDispatcher
    {
        const HookDispatcherDependency<Count, Imports>& dependency;
        void* moduleAddress{};
        std::array<int, Count> refs{};
        int cached = 0;
        HookDispatcher(const HookDispatcherDependency<Count, Imports>& dep, void* base)
            : dependency(dep), moduleAddress(base) {}
        void CacheHookData() { ++cached; }
        int GetRef(size_t i) { return refs[i]; }
        bool EnableHook(size_t i, bool enable)
        {
            auto* mod = static_cast<HMODULE>(moduleAddress);
            const auto name = std::get<1>(dependency[i]);
            if (!mod || !mod->originals.count(name)) return false;
            if (enable)
            {
                if (!refs[i] && HintTest::registrationFailures[mod].count(name)) return false;
                ++refs[i]; ++mod->refs[name];
                mod->hooks[name] = std::get<2>(dependency[i]);
                return refs[i] == 1;
            }
            if (!refs[i]) return false;
            --refs[i]; --mod->refs[name];
            if (!refs[i]) mod->hooks.erase(name);
            return !refs[i];
        }
        template <size_t I, typename Fn> Fn GetOrg()
        {
            auto* mod = static_cast<HMODULE>(moduleAddress);
            const auto name = std::get<1>(dependency[I]);
            return mod && refs[I] ? reinterpret_cast<Fn>(mod->originals.at(name)) : nullptr;
        }
        void DisableAllHooks()
        { for (size_t i = 0; i < Count; ++i) while (refs[i]) EnableHook(i, false); }
    };
}

// The bridge's side: WndCompat, the module of GlassCore and its two exports,
// and the window procedure the subclass chain ends in.
namespace WndCompat { inline bool IsShuttingDown() { return HintTest::shuttingDown; } }
inline HMODULE GetModuleHandleW(LPCWSTR name)
{ return std::wstring_view(name) == L"GlassCore.dll" ? HintTest::core : nullptr; }
inline FARPROC GetProcAddress(HMODULE module, LPCSTR name)
{
    assert(module && module == HintTest::core);
    if (std::strcmp(name, "GlassCore_BeginDelphiHintPaint") == 0) return HintTest::beginExport;
    if (std::strcmp(name, "GlassCore_EndDelphiHintPaint") == 0) return HintTest::endExport;
    return nullptr;
}
inline LRESULT DefSubclassProc(HWND w, UINT message, WPARAM wParam, LPARAM lParam)
{ return HintTest::windowProcedure(w, message, wParam, lParam); }
