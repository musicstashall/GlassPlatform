// Environment doubles for the production WndCompat/Root/DelphiShowErase.h.
// They model what the unit can see and do: a tree of windows in Z order with
// frames, styles, threads, properties and update regions, regions as sets of
// points, and the surface of the top-level window as a colour for every point
// of its client. A DC writes there only where it is allowed to: one that was
// asked for without child clipping reaches the points under the children.
// This is not Windows: no window procedure runs, nothing is composed, and when
// Windows sends WM_NCPAINT and WM_ERASEBKGND is what the test says it is.
#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#define WINAPI
#define CALLBACK
using BOOL = int;
using UINT = unsigned int;
using LONG = std::int32_t;
using DWORD = std::uint32_t;
using COLORREF = DWORD;
using ULONGLONG = std::uint64_t;
using WPARAM = std::uintptr_t;
using LPARAM = std::intptr_t;
using HANDLE = void*;
struct RECT { LONG left{}, top{}, right{}, bottom{}; };
struct POINT { LONG x{}, y{}; };
struct TestWindow;
struct TestDC;
struct TestRegion;
using HWND = TestWindow*;
using HDC = TestDC*;
using HRGN = TestRegion*;
struct WINDOWPOS { HWND hwnd{}; HWND hwndInsertAfter{}; int x{}, y{}, cx{}, cy{}; UINT flags{}; };
inline constexpr BOOL TRUE = 1, FALSE = 0;
inline constexpr UINT WM_PAINT = 0x000f, WM_ERASEBKGND = 0x0014, WM_WINDOWPOSCHANGING = 0x0046;
inline constexpr UINT WM_NCDESTROY = 0x0082, WM_NCPAINT = 0x0085;
inline constexpr UINT SWP_NOSIZE = 0x0001, SWP_SHOWWINDOW = 0x0040, SWP_FRAMECHANGED = 0x0020;
inline constexpr UINT GA_ROOT = 2;
inline constexpr UINT GW_HWNDLAST = 1, GW_HWNDPREV = 3, GW_CHILD = 5;
inline constexpr DWORD DCX_CACHE = 0x2, DCX_CLIPCHILDREN = 0x8;
inline constexpr int GWL_STYLE = -16, GWL_EXSTYLE = -20;
inline constexpr DWORD WS_VISIBLE = 0x10000000, WS_EX_LAYERED = 0x00080000, WS_EX_LAYOUTRTL = 0x00400000;
inline constexpr int ERROR = 0, NULLREGION = 1, SIMPLEREGION = 2, COMPLEXREGION = 3;
inline constexpr int RGN_AND = 1, RGN_OR = 2, RGN_DIFF = 4;
inline constexpr DWORD BLACKNESS = 0x00000042;
inline constexpr HWND HWND_DESKTOP = nullptr;
inline constexpr COLORREF CLR_INVALID = 0xffffffffu;
inline constexpr int DBG_NORMAL = 1;

using Points = std::set<std::pair<LONG, LONG>>;
inline Points RectPoints(const RECT& r)
{
    Points points;
    for (LONG y = r.top; y < r.bottom; ++y) for (LONG x = r.left; x < r.right; ++x) points.emplace(x, y);
    return points;
}
struct TestRegion { Points points; };
struct TestWindow
{
    std::wstring cls;
    HWND parent{};
    std::vector<HWND> children;         // direct children; front() is the top of the Z order
    RECT screen{};                      // the window rectangle, in screen coordinates
    RECT frame{};                       // how wide its non-client frame is on each side
    bool valid = true, iconic = false, backdrop = true;
    DWORD style = WS_VISIBLE, ex = 0;
    DWORD thread = 1;
    Points update;                      // what is waiting for WM_PAINT, in client coordinates
    std::unordered_map<std::wstring, HANDLE> props;
    RECT Client() const                 // the client rectangle, in screen coordinates
    { return RECT{screen.left + frame.left, screen.top + frame.top, screen.right - frame.right, screen.bottom - frame.bottom}; }
};
struct TestDC { HWND owner{}; DWORD flags{}; bool clipped = false; Points clip; };

namespace Shared { enum class InjectionMode : DWORD { Forbidden = 0, PopupOnly = 1, Full = 2 }; }
namespace SeTest
{
    inline ULONGLONG clock = 1000;
    inline DWORD thread = 1, process = 0x000073F8;
    inline Shared::InjectionMode mode = Shared::InjectionMode::Full;
    inline constexpr COLORREF kLight = 0x00f0e0d0;
    // The surface of the top-level window: the points of its client that were
    // written, with what; every other point holds kLight.
    inline std::map<std::pair<LONG, LONG>, COLORREF> surface;
    inline std::set<TestRegion*> liveRegions;
    inline std::vector<TestDC*> liveDcs;
    inline int regionsMade = 0, dcsTaken = 0, dcsReleased = 0, blits = 0, clipChildrenBlits = 0;
    inline int updateQueries = 0, updateErases = 0, pixelReads = 0;
    // Failures: the n-th call of a kind fails (counted from 1), or a call for a window does.
    inline int failCreate = 0, failCombine = 0, failSetRect = 0, failOffset = 0, failSelect = 0;
    inline int creates = 0, combines = 0, setRects = 0, offsets = 0, selects = 0;
    inline bool failDc = false, failBlit = false, failBox = false;
    inline HWND failUpdate{}, failClient{}, failRect{};
    struct Log { std::wstring format; std::vector<long long> numbers; std::vector<std::wstring> texts; };
    inline std::vector<Log> logs;
    inline bool Hit(int& counter, int failAt) { return ++counter == failAt; }
}

inline BOOL IsWindow(HWND w) { return w && w->valid; }
inline BOOL IsWindowVisible(HWND w)
{
    for (; w; w = w->parent) if (!w->valid || !(w->style & WS_VISIBLE)) return FALSE;
    return TRUE;
}
inline BOOL IsIconic(HWND w) { return IsWindow(w) && w->iconic; }
inline HWND GetAncestor(HWND w, UINT flags)
{
    assert(flags == GA_ROOT);
    if (!IsWindow(w)) return nullptr;
    while (w->parent) w = w->parent;
    return w;
}
inline HWND GetWindow(HWND w, UINT command)
{
    if (!IsWindow(w)) return nullptr;
    if (command == GW_CHILD) return w->children.empty() ? nullptr : w->children.front();
    if (!w->parent) return command == GW_HWNDLAST ? w : nullptr;
    const auto& siblings = w->parent->children;
    const auto it = std::find(siblings.begin(), siblings.end(), w);
    assert(it != siblings.end());
    if (command == GW_HWNDLAST) return siblings.back();
    assert(command == GW_HWNDPREV);
    return it == siblings.begin() ? nullptr : *(it - 1);
}
inline LONG GetWindowLongW(HWND w, int index)
{
    assert(index == GWL_STYLE || index == GWL_EXSTYLE);
    return static_cast<LONG>(index == GWL_STYLE ? w->style : w->ex);
}
inline DWORD GetWindowThreadProcessId(HWND w, DWORD* process) { assert(!process); return IsWindow(w) ? w->thread : 0; }
inline DWORD GetCurrentThreadId() { return SeTest::thread; }
inline DWORD GetCurrentProcessId() { return SeTest::process; }
inline HANDLE GetPropW(HWND w, const wchar_t* name)
{
    if (!IsWindow(w)) return nullptr;
    const auto it = w->props.find(name);
    return it == w->props.end() ? nullptr : it->second;
}
inline ULONGLONG GetTickCount64() { return SeTest::clock; }
inline BOOL GetWindowRect(HWND w, RECT* rect)
{
    if (!IsWindow(w) || w == SeTest::failRect) return FALSE;
    *rect = w->screen;
    return TRUE;
}
inline BOOL GetClientRect(HWND w, RECT* rect)
{
    if (!IsWindow(w) || w == SeTest::failClient) return FALSE;
    const RECT c = w->Client();
    *rect = RECT{0, 0, std::max<LONG>(0, c.right - c.left), std::max<LONG>(0, c.bottom - c.top)};
    return TRUE;
}
inline int MapWindowPoints(HWND from, HWND to, POINT* points, UINT count)
{
    assert(IsWindow(to) && (!from || IsWindow(from)));
    const RECT a = from ? from->Client() : RECT{}, b = to->Client();
    for (UINT i = 0; i < count; ++i) { points[i].x += a.left - b.left; points[i].y += a.top - b.top; }
    return 0;
}
inline BOOL IsRectEmpty(const RECT* r) { return r->right <= r->left || r->bottom <= r->top; }
inline BOOL IntersectRect(RECT* out, const RECT* a, const RECT* b)
{
    const RECT r{std::max(a->left, b->left), std::max(a->top, b->top), std::min(a->right, b->right), std::min(a->bottom, b->bottom)};
    if (IsRectEmpty(&r)) { *out = RECT{}; return FALSE; }
    *out = r;
    return TRUE;
}

inline int KindOf(const Points& points)
{
    if (points.empty()) return NULLREGION;
    LONG l = points.begin()->first, t = points.begin()->second, r = l, b = t;
    for (const auto& p : points) { l = std::min(l, p.first); r = std::max(r, p.first); t = std::min(t, p.second); b = std::max(b, p.second); }
    return static_cast<long long>(points.size()) == static_cast<long long>(r - l + 1) * (b - t + 1) ? SIMPLEREGION : COMPLEXREGION;
}
inline HRGN CreateRectRgn(int l, int t, int r, int b)
{
    if (SeTest::Hit(SeTest::creates, SeTest::failCreate)) return nullptr;
    auto* region = new TestRegion{RectPoints(RECT{l, t, r, b})};
    SeTest::liveRegions.insert(region);
    ++SeTest::regionsMade;
    return region;
}
inline BOOL DeleteObject(HRGN region)
{
    assert(SeTest::liveRegions.count(region));
    SeTest::liveRegions.erase(region);
    delete region;
    return TRUE;
}
inline BOOL SetRectRgn(HRGN region, int l, int t, int r, int b)
{
    assert(SeTest::liveRegions.count(region));
    if (SeTest::Hit(SeTest::setRects, SeTest::failSetRect)) return FALSE;
    region->points = RectPoints(RECT{l, t, r, b});
    return TRUE;
}
inline int CombineRgn(HRGN dst, HRGN a, HRGN b, int mode)
{
    assert(SeTest::liveRegions.count(dst) && SeTest::liveRegions.count(a) && SeTest::liveRegions.count(b));
    if (SeTest::Hit(SeTest::combines, SeTest::failCombine)) return ERROR;
    Points out;
    if (mode == RGN_AND) { for (const auto& p : a->points) if (b->points.count(p)) out.insert(p); }
    else if (mode == RGN_OR) { out = a->points; out.insert(b->points.begin(), b->points.end()); }
    else { assert(mode == RGN_DIFF); for (const auto& p : a->points) if (!b->points.count(p)) out.insert(p); }
    dst->points = std::move(out);
    return KindOf(dst->points);
}
inline int OffsetRgn(HRGN region, int dx, int dy)
{
    assert(SeTest::liveRegions.count(region));
    if (SeTest::Hit(SeTest::offsets, SeTest::failOffset)) return ERROR;
    Points out;
    for (const auto& p : region->points) out.emplace(p.first + dx, p.second + dy);
    region->points = std::move(out);
    return KindOf(region->points);
}
inline int GetRgnBox(HRGN region, RECT* box)
{
    assert(SeTest::liveRegions.count(region));
    *box = RECT{};
    if (SeTest::failBox) return ERROR;
    if (region->points.empty()) return NULLREGION;
    RECT r{region->points.begin()->first, region->points.begin()->second, 0, 0};
    r.right = r.left; r.bottom = r.top;
    for (const auto& p : region->points)
    { r.left = std::min(r.left, p.first); r.right = std::max(r.right, p.first); r.top = std::min(r.top, p.second); r.bottom = std::max(r.bottom, p.second); }
    *box = RECT{r.left, r.top, r.right + 1, r.bottom + 1};
    return KindOf(region->points);
}
inline BOOL PtInRegion(HRGN region, int x, int y)
{ assert(SeTest::liveRegions.count(region)); return region->points.count({x, y}) ? TRUE : FALSE; }
// The update region as it is kept for the window: the model does not cut it
// to what the parents let through.
inline int GetUpdateRgn(HWND w, HRGN region, BOOL erase)
{
    assert(SeTest::liveRegions.count(region));
    ++SeTest::updateQueries;
    if (erase) ++SeTest::updateErases;
    if (!IsWindow(w) || w == SeTest::failUpdate) return ERROR;
    region->points = w->update;
    return KindOf(region->points);
}
inline HDC GetDCEx(HWND w, HRGN region, DWORD flags)
{
    assert(!region);
    if (SeTest::failDc || !IsWindow(w)) return nullptr;
    auto* dc = new TestDC{w, flags, false, {}};
    SeTest::liveDcs.push_back(dc);
    ++SeTest::dcsTaken;
    return dc;
}
inline int ReleaseDC(HWND w, HDC dc)
{
    const auto it = std::find(SeTest::liveDcs.begin(), SeTest::liveDcs.end(), dc);
    assert(it != SeTest::liveDcs.end() && dc->owner == w);
    assert(!dc->clipped);               // the clip was taken back first
    SeTest::liveDcs.erase(it);
    ++SeTest::dcsReleased;
    delete dc;
    return 1;
}
inline int SelectClipRgn(HDC dc, HRGN region)
{
    assert(dc && (!region || SeTest::liveRegions.count(region)));
    if (SeTest::Hit(SeTest::selects, SeTest::failSelect)) return ERROR;
    dc->clipped = region != nullptr;
    dc->clip = region ? region->points : Points{};
    return region ? KindOf(dc->clip) : SIMPLEREGION;
}
// True: a point of the client that some visible child window covers.
inline bool UnderChild(HWND w, LONG x, LONG y)
{
    for (HWND child : w->children)
    {
        if (!child->valid || !(child->style & WS_VISIBLE)) continue;
        const RECT b = w->Client();
        const RECT r{child->screen.left - b.left, child->screen.top - b.top, child->screen.right - b.left, child->screen.bottom - b.top};
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return true;
    }
    return false;
}
inline BOOL PatBlt(HDC dc, int x, int y, int width, int height, DWORD rop)
{
    assert(dc && rop == BLACKNESS && !dc->owner->parent);
    ++SeTest::blits;
    if (SeTest::failBlit) return FALSE;
    RECT client{};
    GetClientRect(dc->owner, &client);
    if (dc->flags != DCX_CACHE) ++SeTest::clipChildrenBlits;
    for (LONG py = y; py < y + height; ++py)
    {
        for (LONG px = x; px < x + width; ++px)
        {
            if (px < 0 || py < 0 || px >= client.right || py >= client.bottom) continue;
            if (dc->clipped && !dc->clip.count({px, py})) continue;
            // Only a DC that was not asked to clip the children reaches what lies under them.
            if (dc->flags != DCX_CACHE && UnderChild(dc->owner, px, py)) continue;
            SeTest::surface[{px, py}] = 0;
        }
    }
    return TRUE;
}
inline COLORREF GetPixel(HDC dc, int x, int y)
{
    assert(dc && !dc->owner->parent && !dc->clipped);
    ++SeTest::pixelReads;
    RECT client{};
    GetClientRect(dc->owner, &client);
    if (x < 0 || y < 0 || x >= client.right || y >= client.bottom) return CLR_INVALID;
    const auto it = SeTest::surface.find({x, y});
    return it == SeTest::surface.end() ? SeTest::kLight : it->second;
}

namespace WndCompat { inline Shared::InjectionMode WinEventHook_GetMode() { return SeTest::mode; } }
namespace GlassShared { inline bool GlassIsBackdropAllowedForHwnd(HWND w) { return w && w->backdrop; } }
template <typename... Args> inline void DebugLog(int, const wchar_t* format, Args... args)
{
    SeTest::Log log{format, {}, {}};
    const auto capture = [&](auto value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, wchar_t*> || std::is_same_v<T, const wchar_t*>) log.texts.emplace_back(value);
        else if constexpr (std::is_pointer_v<T>) log.numbers.push_back(static_cast<long long>(reinterpret_cast<std::intptr_t>(value)));
        else log.numbers.push_back(static_cast<long long>(value));
    };
    (capture(args), ...);
    SeTest::logs.push_back(std::move(log));
}
