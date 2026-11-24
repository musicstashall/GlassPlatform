// Environment doubles for the production WndCompat/Root/DelphiRestoreProbe.h.
// They model what the probe can see and do: a tree of windows with classes,
// styles and screen rectangles, the surface of the top-level window as a
// function of a point, the message queue as far as a posted message and a
// timer go, and the Debug log. This is not Windows: no window procedure runs,
// nothing is composed, and the order in which Windows takes posted messages,
// WM_PAINT and WM_TIMER is what the test says it is.
#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#define WINAPI
#define CALLBACK
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
using BOOL = int;
using UINT = unsigned int;
using LONG = std::int32_t;
using DWORD = std::uint32_t;
using COLORREF = DWORD;
using ULONGLONG = std::uint64_t;
using UINT_PTR = std::uintptr_t;
using WPARAM = std::uintptr_t;
using LPARAM = std::intptr_t;
using LRESULT = std::intptr_t;
struct RECT { LONG left{}, top{}, right{}, bottom{}; };
struct POINT { LONG x{}, y{}; };
struct TestWindow;
struct TestDC;
using HWND = TestWindow*;
using HDC = TestDC*;
struct WINDOWPOS { HWND hwnd{}; HWND hwndInsertAfter{}; int x{}, y{}, cx{}, cy{}; UINT flags{}; };
using WNDENUMPROC = BOOL (CALLBACK*)(HWND, LPARAM);
using TIMERPROC = void (CALLBACK*)(HWND, UINT, UINT_PTR, DWORD);
inline constexpr BOOL TRUE = 1, FALSE = 0;
inline constexpr UINT WM_NULL = 0x0000, WM_ERASEBKGND = 0x0014, WM_WINDOWPOSCHANGING = 0x0046;
inline constexpr UINT WM_NCDESTROY = 0x0082, WM_NCPAINT = 0x0085, WM_TIMER = 0x0113, WM_PAINT = 0x000f;
inline constexpr UINT SWP_SHOWWINDOW = 0x0040, SWP_NOSIZE = 0x0001;
inline constexpr UINT GA_ROOT = 2;
inline constexpr DWORD DCX_CACHE = 0x2;
inline constexpr int GWL_STYLE = -16;
inline constexpr HWND HWND_DESKTOP = nullptr;
inline constexpr COLORREF CLR_INVALID = 0xffffffffu;
inline constexpr int DBG_NORMAL = 1;

namespace GlassShared { enum class GlassControlKind : unsigned { None = 0, Button = 1, ComboBox = 5, TabControl = 12, Edit = 19 }; }
struct TestWindow
{
    std::wstring cls;
    HWND parent{};
    std::vector<HWND> children;         // direct children, in z-order
    RECT screen{};                      // the window rectangle, in screen coordinates
    POINT clientOrigin{};               // where the client of a top-level window starts on the screen
    bool visible = true, iconic = false, valid = true;
    DWORD style = 0x56000000;
    GlassShared::GlassControlKind kind = GlassShared::GlassControlKind::None;
    bool pending = false;               // an update region is waiting for WM_PAINT
};
struct TestDC { HWND owner{}; DWORD flags{}; };

namespace PrbTest
{
    inline ULONGLONG clock = 1000;
    inline HWND foreground{};
    inline bool failDc = false, failPost = false, failTimer = false;
    // The surface of the top-level window: a colour for a point of its client.
    inline std::function<COLORREF(int, int)> surface;
    struct Posted { HWND hwnd{}; UINT message{}; WPARAM wParam{}; LPARAM lParam{}; };
    inline std::vector<Posted> posted;
    struct Timer { HWND hwnd{}; UINT_PTR id{}; UINT elapse{}; };
    inline std::vector<Timer> timers;   // live timers
    inline int timersSet = 0, timersKilled = 0;
    inline std::vector<TestDC*> liveDcs;
    inline int dcsTaken = 0, dcsReleased = 0;
    inline int updateErases = 0;        // GetUpdateRect calls that asked for an erase
    struct Log { std::wstring format; std::vector<long long> numbers; std::vector<std::wstring> texts; };
    inline std::vector<Log> logs;
}

inline BOOL IsWindow(HWND w) { return w && w->valid; }
// Visible: the style of the window and of every ancestor.
inline BOOL IsWindowVisible(HWND w)
{
    for (; w; w = w->parent) if (!w->valid || !w->visible) return FALSE;
    return TRUE;
}
inline BOOL IsIconic(HWND w) { return IsWindow(w) && w->iconic; }
inline HWND GetParent(HWND w) { return IsWindow(w) ? w->parent : nullptr; }
inline HWND GetAncestor(HWND w, UINT flags)
{
    assert(flags == GA_ROOT);
    if (!IsWindow(w)) return nullptr;
    while (w->parent) w = w->parent;
    return w;
}
inline HWND GetForegroundWindow() { return PrbTest::foreground; }
// Copies what fits and returns its length, as Windows does.
inline int GetClassNameW(HWND w, wchar_t* text, int capacity)
{
    if (!IsWindow(w) || capacity <= 0) return 0;
    const auto count = std::min<size_t>(w->cls.size(), static_cast<size_t>(capacity) - 1);
    std::wmemcpy(text, w->cls.c_str(), count); text[count] = 0;
    return static_cast<int>(count);
}
inline BOOL GetWindowRect(HWND w, RECT* rect) { if (!IsWindow(w)) return FALSE; *rect = w->screen; return TRUE; }
inline int MapWindowPoints(HWND from, HWND to, POINT* points, UINT count)
{
    assert(from == HWND_DESKTOP && IsWindow(to) && !to->parent);
    for (UINT i = 0; i < count; ++i) { points[i].x -= to->clientOrigin.x; points[i].y -= to->clientOrigin.y; }
    return 0;
}
inline LONG GetWindowLongW(HWND w, int index) { assert(index == GWL_STYLE); return static_cast<LONG>(w->style); }
inline BOOL GetUpdateRect(HWND w, RECT* rect, BOOL erase)
{
    if (erase) ++PrbTest::updateErases;
    if (!IsWindow(w) || !w->pending) return FALSE;
    *rect = RECT{0, 0, 1, 1};
    return TRUE;
}
inline HDC GetDCEx(HWND w, void* region, DWORD flags)
{
    assert(!region);
    if (PrbTest::failDc || !IsWindow(w)) return nullptr;
    auto* dc = new TestDC{w, flags};
    PrbTest::liveDcs.push_back(dc);
    ++PrbTest::dcsTaken;
    return dc;
}
inline int ReleaseDC(HWND w, HDC dc)
{
    const auto it = std::find(PrbTest::liveDcs.begin(), PrbTest::liveDcs.end(), dc);
    assert(it != PrbTest::liveDcs.end() && dc->owner == w);
    PrbTest::liveDcs.erase(it);
    ++PrbTest::dcsReleased;
    delete dc;
    return 1;
}
// A DC of the top-level window that was not asked to clip its children reads
// the whole surface; any other DC reads nothing here.
inline COLORREF GetPixel(HDC dc, int x, int y)
{
    assert(dc && dc->owner && !dc->owner->parent);
    if (dc->flags != DCX_CACHE || !PrbTest::surface) return CLR_INVALID;
    return PrbTest::surface(x, y);
}
// Every descendant, depth first, as EnumChildWindows walks them.
inline BOOL EnumChildWindows(HWND w, WNDENUMPROC proc, LPARAM lParam)
{
    std::function<bool(HWND)> walk = [&](HWND parent) {
        for (HWND child : parent->children)
        {
            if (!proc(child, lParam)) return false;
            if (!walk(child)) return false;
        }
        return true;
    };
    walk(w);
    return TRUE;
}
inline ULONGLONG GetTickCount64() { return PrbTest::clock; }
inline BOOL PostMessageW(HWND w, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (PrbTest::failPost) return FALSE;
    PrbTest::posted.push_back({w, message, wParam, lParam});
    return TRUE;
}
inline UINT_PTR SetTimer(HWND w, UINT_PTR id, UINT elapse, TIMERPROC proc)
{
    assert(!proc);
    if (PrbTest::failTimer) return 0;
    PrbTest::timers.push_back({w, id, elapse});
    ++PrbTest::timersSet;
    return id;
}
inline BOOL KillTimer(HWND w, UINT_PTR id)
{
    const auto it = std::find_if(PrbTest::timers.begin(), PrbTest::timers.end(),
        [&](const PrbTest::Timer& t) { return t.hwnd == w && t.id == id; });
    if (it == PrbTest::timers.end()) return FALSE;
    PrbTest::timers.erase(it);
    ++PrbTest::timersKilled;
    return TRUE;
}
namespace GlassShared { inline GlassControlKind GlassGetWindowKind(HWND w) { return w->kind; } }
template <typename... Args> inline void DebugLog(int, const wchar_t* format, Args... args)
{
    PrbTest::Log log{format, {}, {}};
    const auto capture = [&](auto value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, wchar_t*> || std::is_same_v<T, const wchar_t*>) log.texts.emplace_back(value);
        else if constexpr (std::is_pointer_v<T>) log.numbers.push_back(static_cast<long long>(reinterpret_cast<std::intptr_t>(value)));
        else log.numbers.push_back(static_cast<long long>(value));
    };
    (capture(args), ...);
    PrbTest::logs.push_back(std::move(log));
}
