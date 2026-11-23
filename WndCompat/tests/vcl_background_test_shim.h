// Environment doubles for the complete production TBackgroundHooks translation
// unit. They model what the module can observe: the EXE's FillRect import, the
// order in which PaintDispatcher calls its providers around BeginPaint, a VCL
// window that erases itself when it receives WM_ERASEBKGND, and what a DC tells
// about itself (its kind, its window, its clip). There are no threads: a test
// says which one is calling and what another one did before a lock was taken.
// This is not Windows GDI, DWM, the update region, the real PE import table or
// the VCL.
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
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
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
using ULONGLONG = std::uint64_t;
using WCHAR = wchar_t;
using LPCSTR = const char*;
using LPCWSTR = const wchar_t*;
using LPWSTR = wchar_t*;
using LRESULT = std::intptr_t;
using WPARAM = std::uintptr_t;
using LPARAM = std::intptr_t;
using HGDIOBJ = void*;
struct RECT { LONG left{}, top{}, right{}, bottom{}; };
struct POINT { LONG x{}, y{}; };
struct LOGBRUSH { UINT lbStyle{}; COLORREF lbColor{}; std::uintptr_t lbHatch{}; };
struct TestWindow;
struct TestDC;
struct TestModule;
struct TestBitmap { int marker{}; };
using HWND = TestWindow*;
using HDC = TestDC*;
using HMODULE = TestModule*;
using HBRUSH = LOGBRUSH*;
struct PAINTSTRUCT { HDC hdc{}; BOOL fErase{}; RECT rcPaint{}; };
using LPPAINTSTRUCT = PAINTSTRUCT*;
struct DRAWITEMSTRUCT { UINT CtlType{}, CtlID{}, itemID{}, itemAction{}, itemState{}; HWND hwndItem{}; HDC hDC{}; RECT rcItem{}; };
inline constexpr BOOL TRUE = 1, FALSE = 0;
inline constexpr UINT BS_SOLID = 0, BS_HATCHED = 2, OBJ_BITMAP = 7;
inline constexpr DWORD OBJ_DC = 3, OBJ_MEMDC = 10;
inline constexpr int COLOR_WINDOW = 5, COLOR_BTNFACE = 15;
inline constexpr int MM_TEXT = 1, MM_ANISOTROPIC = 8;
inline constexpr int NULLREGION = 1, SIMPLEREGION = 2;
inline constexpr DWORD BLACKNESS = 0x42;
inline constexpr UINT WM_ERASEBKGND = 0x14, ODA_DRAWENTIRE = 1, ODA_SELECT = 2;
inline constexpr UINT LB_GETTOPINDEX = 0x18e, LB_GETCOUNT = 0x18b, LB_GETITEMRECT = 0x198;
inline constexpr LRESULT LB_ERR = -1;
inline constexpr int DBG_NORMAL = 1;

namespace GlassShared { enum class GlassControlKind { None, ListBox, Edit, ComboBox, RichEdit50W, ListView, TreeView }; }
struct TestWindow
{
    std::wstring cls = L"TListBox";
    bool valid = true, backdrop = true;
    GlassShared::GlassControlKind kind = GlassShared::GlassControlKind::ListBox;
    RECT client{0, 0, 200, 120};
    // TWinControl.WMEraseBkgnd: FillRect(Message.DC, ClientRect, FBrush.Handle),
    // unless the control is double-buffered and the message is the system's.
    HBRUSH brush{};
    bool erases = true, doubleBuffered = false;
    bool inputDefault = false;          // the confirmed default CTLCOLOR brush of a TNumberBox
    int top = 0, count = 3, rowHeight = 16;
    DWORD thread = 1;                   // the thread the window belongs to
};
struct TestDC
{
    HWND owner{};
    TestBitmap* bitmap{};
    int map = MM_TEXT;
    POINT viewport{}, window{};
    int clipKind = SIMPLEREGION;
    // A client DC is clipped to the client; a window DC (frame painting) is not.
    bool frame = false;
    RECT frameClip{};
    bool screen = false;                // a display DC with no window behind it
};
struct TestModule
{
    std::unordered_map<std::string, void*> originals, hooks;
    std::unordered_map<std::string, int> refs;
};

namespace BgTest
{
    inline bool full = true;
    inline bool insideDraw = false;                     // an owner-draw item session of TListBoxDraw
    inline HWND topWindow{};                            // the innermost window session of the dispatcher
    inline std::unordered_map<HDC, HWND> paintDcs;      // paint DCs the dispatcher has recorded
    inline thread_local DWORD error{};
    inline ULONGLONG clock = 100;
    inline COLORREF windowColor = 0xffffff, faceColor = 0xf0f0f0;
    inline bool failBlack = false, failObject = false;
    inline int nativeFillResult = 1;
    inline DWORD thread = 1;                            // the calling thread
    inline int windowLookups = 0;                       // WindowFromDC calls
    inline int classLookups = 0;                        // GetClassNameW calls
    // Runs once, right before the next exclusive lock is taken: what another
    // thread did between a check made without the lock and the lock itself.
    inline std::function<void()> beforeExclusive;

    // Everything that reaches a surface or a window, in order.
    enum class Kind { NativeFill, DirectFill, Black, EraseMessage };
    struct Event
    {
        Kind kind{};
        HDC dc{};
        RECT rect{};
        HBRUSH brush{};
        HWND hwnd{};
        WPARAM wParam{};
        LPARAM lParam{};
    };
    inline std::vector<Event> events;
    inline std::function<void(HWND, HDC)> duringErase;   // runs inside the window's erase handler, before its fill
    inline std::function<void()> duringNativeFill;
    struct Log { std::wstring format; std::vector<long long> numbers; std::vector<std::wstring> texts; };
    inline std::vector<Log> logs;

    inline int Count(Kind kind)
    { return static_cast<int>(std::count_if(events.begin(), events.end(), [=](const Event& e) { return e.kind == kind; })); }
}

inline DWORD GetLastError() { return BgTest::error; }
inline void SetLastError(DWORD error) { BgTest::error = error; }
inline BOOL IsWindow(HWND w) { return w && w->valid; }
inline int GetClassNameW(HWND w, LPWSTR text, int capacity)
{
    ++BgTest::classLookups;
    if (!IsWindow(w) || capacity <= static_cast<int>(w->cls.size())) return 0;
    std::wcscpy(text, w->cls.c_str()); return static_cast<int>(w->cls.size());
}
inline BOOL GetClientRect(HWND w, RECT* rc) { if (!IsWindow(w)) return FALSE; *rc = w->client; return TRUE; }
inline BOOL IsRectEmpty(const RECT* rc) { return rc->right <= rc->left || rc->bottom <= rc->top; }
inline BOOL EqualRect(const RECT* a, const RECT* b)
{ return a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom; }
inline HWND WindowFromDC(HDC dc) { ++BgTest::windowLookups; return dc ? dc->owner : nullptr; }
// A DC with a window behind it is a display DC, and so is one of the screen;
// the buffers are memory DCs. No DC is no object.
inline DWORD GetObjectType(HDC dc) { return !dc ? 0 : dc->owner || dc->screen ? OBJ_DC : OBJ_MEMDC; }
inline DWORD GetCurrentThreadId() { return BgTest::thread; }
inline DWORD GetWindowThreadProcessId(HWND w, DWORD* process) { assert(!process); return IsWindow(w) ? w->thread : 0; }
inline int GetMapMode(HDC dc) { return dc->map; }
inline BOOL GetViewportOrgEx(HDC dc, POINT* out) { *out = dc->viewport; return TRUE; }
inline BOOL GetWindowOrgEx(HDC dc, POINT* out) { *out = dc->window; return TRUE; }
inline HGDIOBJ GetCurrentObject(HDC dc, UINT type) { assert(type == OBJ_BITMAP); return dc->bitmap; }
inline int GetClipBox(HDC dc, RECT* out)
{ *out = dc->frame ? dc->frameClip : dc->owner ? dc->owner->client : RECT{}; return dc->clipKind; }
inline COLORREF GetSysColor(int index)
{ assert(index == COLOR_WINDOW || index == COLOR_BTNFACE); return index == COLOR_WINDOW ? BgTest::windowColor : BgTest::faceColor; }
inline int GetObjectW(HBRUSH brush, int bytes, void* out)
{
    if (BgTest::failObject || !brush || bytes != static_cast<int>(sizeof(LOGBRUSH))) return 0;
    std::memcpy(out, brush, sizeof(LOGBRUSH)); return bytes;
}
inline ULONGLONG GetTickCount64() { return BgTest::clock; }
inline BOOL PatBlt(HDC dc, int x, int y, int w, int h, DWORD rop)
{
    assert(dc && rop == BLACKNESS);
    SetLastError(0xb101);
    if (BgTest::failBlack) return FALSE;
    BgTest::events.push_back({BgTest::Kind::Black, dc, RECT{x, y, x + w, y + h}});
    return TRUE;
}
// The true function: what the import reaches without a hook, or through the
// original a hook was given.
inline int NativeFillRect(HDC dc, const RECT* rect, HBRUSH brush)
{
    BgTest::events.push_back({BgTest::Kind::NativeFill, dc, rect ? *rect : RECT{}, brush});
    if (BgTest::duringNativeFill) BgTest::duringNativeFill();
    SetLastError(0xb102);
    return BgTest::nativeFillResult;
}
// What the production unit calls by name when it has no original to forward to.
inline int FillRect(HDC dc, const RECT* rect, HBRUSH brush)
{
    BgTest::events.push_back({BgTest::Kind::DirectFill, dc, rect ? *rect : RECT{}, brush});
    return 1;
}
// A call the EXE makes through its import table.
inline int ExeFillRect(TestModule& exe, HDC dc, const RECT* rect, HBRUSH brush)
{
    using Fn = int (WINAPI*)(HDC, const RECT*, HBRUSH);
    const auto it = exe.hooks.find("FillRect");
    return it != exe.hooks.end() ? reinterpret_cast<Fn>(it->second)(dc, rect, brush) : NativeFillRect(dc, rect, brush);
}
namespace GlassCore::ModuleHandler { inline HMODULE g_entryPointModule{}; }
// The window procedure of a VCL list, as far as this unit talks to it.
inline LRESULT SendMessageW(HWND w, UINT msg, WPARAM wParam, LPARAM lParam)
{
    assert(IsWindow(w));
    if (msg == LB_GETTOPINDEX) return w->top;
    if (msg == LB_GETCOUNT) return w->count;
    if (msg == LB_GETITEMRECT)
    {
        const auto index = static_cast<int>(wParam);
        if (index < 0 || index >= w->count) return LB_ERR;
        auto* out = reinterpret_cast<RECT*>(lParam);
        const LONG rowTop = (index - w->top) * w->rowHeight;
        *out = RECT{w->client.left, rowTop, w->client.right, rowTop + w->rowHeight};
        return 1;
    }
    assert(msg == WM_ERASEBKGND);
    const HDC dc = reinterpret_cast<HDC>(wParam);
    BgTest::events.push_back({BgTest::Kind::EraseMessage, dc, RECT{}, nullptr, w, wParam, lParam});
    if (BgTest::duringErase) BgTest::duringErase(w, dc);
    if (w->erases && (!w->doubleBuffered || wParam == static_cast<WPARAM>(lParam)))
        ExeFillRect(*GlassCore::ModuleHandler::g_entryPointModule, dc, &w->client, w->brush);
    return 1;
}
template <typename... Args> inline void DebugLog(int, const wchar_t* format, Args... args)
{
    BgTest::Log log{format, {}, {}};
    const auto capture = [&](auto value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, wchar_t*> || std::is_same_v<T, const wchar_t*>) log.texts.emplace_back(value);
        else if constexpr (std::is_pointer_v<T>) log.numbers.push_back(static_cast<long long>(reinterpret_cast<std::intptr_t>(value)));
        else log.numbers.push_back(static_cast<long long>(value));
    };
    (capture(args), ...);
    BgTest::logs.push_back(std::move(log));
    SetLastError(0xb103);
}

namespace GlassShared
{
    inline bool GlassIsBackdropAllowedForHwnd(HWND w) { return w && w->backdrop; }
    inline GlassControlKind GlassGetWindowKind(HWND w) { return w->kind; }
    inline bool HasDelphiInputDefaultBackground(HWND w) { return w && w->inputDefault; }
}
namespace wil
{
    struct srwlock
    {
        struct Guard { ~Guard() {} };
        Guard lock_exclusive()
        {
            if (const auto other = std::exchange(BgTest::beforeExclusive, {})) other();
            return {};
        }
        Guard lock_shared() { return {}; }
    };
}
namespace GlassCore::Root { inline bool IsFullInjectionMode() { return BgTest::full; } }
namespace GlassCore::DelphiListBox { inline bool IsInsideDraw() noexcept { return BgTest::insideDraw; } }
namespace GlassCore::PaintDispatcher
{
    struct PaintProvider { void (*onBegin)(HWND, HMODULE); void (*onEnd)(HWND, HMODULE); };
    struct PostPaintProvider { bool (*onAfterBeginPaint)(HWND, HDC, LPPAINTSTRUCT, HMODULE) noexcept; };
    inline std::vector<PaintProvider> paintProviders, dcProviders;
    inline std::vector<PostPaintProvider> postProviders;
    inline void RegisterPaintProvider(PaintProvider p) { paintProviders.push_back(p); }
    inline void RegisterGetDCProvider(PaintProvider p) { dcProviders.push_back(p); }
    inline void RegisterPostPaintProvider(PostPaintProvider p) { postProviders.push_back(p); }
    inline bool IsCurrentPaintDcForHwnd(HWND w, HDC dc)
    { const auto it = BgTest::paintDcs.find(dc); return it != BgTest::paintDcs.end() && it->second == w; }
    inline bool IsTopWindowDrawingSession(HWND w) { return BgTest::topWindow == w; }
}
namespace GlassCore::Hooks
{
    template <size_t Count, size_t Imports> using HookDispatcherDependency = std::array<
        std::tuple<std::array<std::string_view, Imports>, LPCSTR, void*>, Count>;
    // One import slot per name in the module; refs as the production dispatcher
    // counts them. EnableHook reports only a change of the slot, as it does there.
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
            if (!mod || !mod->originals.count(name)) return false;
            if (enable)
            {
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
