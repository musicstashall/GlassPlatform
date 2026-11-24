// VCL client background: observe EXE FillRect before BeginPaint returns, then
// validate its HDC against BeginPaint or a subsequent WM_DRAWITEM before editing.
//
// VCL erases a wrapped native control itself (TWinControl.WMEraseBkgnd fills
// the client with the control's brush) whenever Windows sends WM_ERASEBKGND.
// Windows sends it from inside BeginPaint only when the erase is still pending
// there. After a window was shown, restored or resized it sends the message on
// its own, at once and outside any paint, with a DC of its own: no session is
// open, and the control keeps the colour VCL gave it until WM_PAINT arrives.
// The WM_PAINT that follows erases nothing.
//
// Two answers. Once a control whose client Glass paints has been seen, the
// import stays hooked (Keep), and the default fill of such a control's whole
// client, made outside every session of that window, is replaced where it is
// made (EraseOutside): the control is not white in between. A memo keeps the
// colour its application gave it the way the edit hooks keep it in WM_PAINT: as
// a tint over that base (Tint). And a list repaints only its rows, so it must
// not keep below the last one what was erased before: when its BeginPaint
// brought no erase, the list is asked to erase once more into the DC BeginPaint
// has just returned (EraseAgain).
#include "CorePch.h"
#include <Hooks/delphi/TBackgroundHooks.h>
#include <Hooks/delphi/TListBoxDraw.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/RootSwcaGradient.h>
#include <GlassControlKind.h>
#include <GlassBackdropMark.h>
#include <GlassDelphiInputBackground.h>
#include <WinApiUtils.h>
#include <array>
#include <DebugLog.h>
#include <vector>

namespace GlassCore::DelphiBackground
{
    namespace
    {
        using namespace GlassCore::Hooks;
        using namespace std::literals;
        int WINAPI ObserveFill(HDC, const RECT*, HBRUSH);
        const HookDispatcherDependency<1, 3> g_dep{{
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv,
                "ext-ms-win-ntuser-misc-l1-1-0.dll"sv }, "FillRect", reinterpret_cast<void*>(ObserveFill) },
        }};
        HookDispatcher<1, 3> g_dispatcher{g_dep, nullptr};
        wil::srwlock g_lock;
        std::atomic<bool> g_shutdown{true};
        std::atomic<bool> g_kept{false};  // one reference of the import held between sessions
        struct Session
        {
            HWND hwnd{};
            unsigned long long serial{};
            bool paint{};
            bool list{};
            bool armed{};
            bool number{};
            bool afterBegin{};
            HDC paintDc{};
            HDC observedDc{};
            HGDIOBJ observedBitmap{};
            RECT observedRect{};
            bool defaultBrush{};
            bool erasing{};       // EraseAgain is sending WM_ERASEBKGND for paintDc
            bool erasedAgain{};   // ...and its default fill was replaced
        };
        thread_local std::vector<Session> g_sessions;
        thread_local unsigned long long g_serial = 0;
        thread_local bool g_inside = false;

        bool IsTarget(HWND hwnd, bool& isList) noexcept
        {
            if (!hwnd || !GlassCore::Root::IsFullInjectionMode()
                || !GlassShared::GlassIsBackdropAllowedForHwnd(hwnd)) return false;
            WCHAR cls[64]{};
            if (!GetClassNameW(hwnd, cls, _countof(cls))) return false;
            isList = _wcsicmp(cls, L"TListBox") == 0 || _wcsicmp(cls, L"TCheckListBox") == 0
                || _wcsicmp(cls, L"TColorListBox") == 0
                || _wcsicmp(cls, L"TFileListBox") == 0
                || _wcsicmp(cls, L"TDirectoryListBox") == 0;
            if (isList) return GlassShared::GlassGetWindowKind(hwnd) == GlassShared::GlassControlKind::ListBox;
            return (_wcsicmp(cls, L"TNumberBox") == 0 || _wcsicmp(cls, L"THotKey") == 0)
                && GlassShared::GlassGetWindowKind(hwnd) == GlassShared::GlassControlKind::Edit;
        }
        // The VCL classes whose client Glass paints: in the end their default
        // background is transparent, whatever VCL erased. The names and kinds are
        // the classifier's. `windowOnly`: for this class COLOR_BTNFACE is a colour
        // of its own, which the edit hooks keep - like any other colour of such a
        // control, as a tint. A tab control is repainted whole by its WM_PAINT.
        struct EraseClass { const wchar_t* name; GlassShared::GlassControlKind kind; bool windowOnly; };
        constexpr EraseClass kEraseClasses[] = {
            { L"TListBox", GlassShared::GlassControlKind::ListBox, false },
            { L"TCheckListBox", GlassShared::GlassControlKind::ListBox, false },
            { L"TColorListBox", GlassShared::GlassControlKind::ListBox, false },
            { L"TFileListBox", GlassShared::GlassControlKind::ListBox, false },
            { L"TDirectoryListBox", GlassShared::GlassControlKind::ListBox, false },
            { L"TEdit", GlassShared::GlassControlKind::Edit, false },
            { L"TLabeledEdit", GlassShared::GlassControlKind::Edit, false },
            { L"TButtonedEdit", GlassShared::GlassControlKind::Edit, false },
            { L"TMaskEdit", GlassShared::GlassControlKind::Edit, false },
            { L"TSearchBox", GlassShared::GlassControlKind::Edit, false },
            { L"TNumberBox", GlassShared::GlassControlKind::Edit, false },
            { L"TSpinEdit", GlassShared::GlassControlKind::Edit, false },
            { L"THotKey", GlassShared::GlassControlKind::Edit, false },
            { L"TInplaceEdit", GlassShared::GlassControlKind::Edit, false },
            { L"TInplaceEditList", GlassShared::GlassControlKind::Edit, false },
            { L"TMemo", GlassShared::GlassControlKind::Edit, true },
            { L"TRichEdit", GlassShared::GlassControlKind::RichEdit50W, true },
            { L"TListView", GlassShared::GlassControlKind::ListView, false },
            { L"TSystemListView", GlassShared::GlassControlKind::ListView, false },
            { L"TTreeView", GlassShared::GlassControlKind::TreeView, false },
            { L"TResTreeView", GlassShared::GlassControlKind::TreeView, false },
            { L"TTabControl", GlassShared::GlassControlKind::TabControl, false },
            { L"TPageControl", GlassShared::GlassControlKind::TabControl, false },
        };
        struct EraseTarget { WCHAR cls[64]{}; bool windowOnly{}; bool number{}; };
        bool IsEraseTarget(HWND hwnd, EraseTarget& target) noexcept
        {
            using Kind = GlassShared::GlassControlKind;
            if (!hwnd || !GlassCore::Root::IsFullInjectionMode()) return false;
            const Kind kind = GlassShared::GlassGetWindowKind(hwnd);
            if ((kind != Kind::ListBox && kind != Kind::Edit && kind != Kind::RichEdit50W
                    && kind != Kind::ListView && kind != Kind::TreeView && kind != Kind::TabControl)
                || !GetClassNameW(hwnd, target.cls, _countof(target.cls))) return false;
            for (const EraseClass& c : kEraseClasses)
            {
                if (c.kind != kind || _wcsicmp(target.cls, c.name) != 0) continue;
                target.windowOnly = c.windowOnly;
                target.number = _wcsicmp(target.cls, L"TNumberBox") == 0;
                return GlassShared::GlassIsBackdropAllowedForHwnd(hwnd);
            }
            return false;
        }
        bool IsSystemBrush(HBRUSH brush, bool windowOnly = false) noexcept
        {
            LOGBRUSH lb{};
            if (!brush || GetObjectW(brush, sizeof(lb), &lb) != sizeof(lb) || lb.lbStyle != BS_SOLID) return false;
            const COLORREF rgb = lb.lbColor & 0xFFFFFFu;
            return rgb == (GetSysColor(COLOR_WINDOW) & 0xFFFFFFu)
                || (!windowOnly && rgb == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFFu));
        }
        bool IsFullClient(HWND hwnd, HDC dc, const RECT* r) noexcept
        {
            RECT client{};
            POINT viewport{}, window{};
            return dc && r && GetClientRect(hwnd, &client) && !IsRectEmpty(&client)
                && EqualRect(r, &client) && GetMapMode(dc) == MM_TEXT
                && GetViewportOrgEx(dc, &viewport) && GetWindowOrgEx(dc, &window)
                && viewport.x == 0 && viewport.y == 0 && window.x == 0 && window.y == 0;
        }
        bool AcquireLocked() noexcept // g_lock is held exclusively
        {
            if (g_shutdown.load(std::memory_order_acquire)) return false;
            HMODULE ep = GlassCore::ModuleHandler::g_entryPointModule;
            if (!ep) return false;
            g_dispatcher.moduleAddress = ep;
            const int before = g_dispatcher.GetRef(0);
            g_dispatcher.EnableHook(0, true);
            return g_dispatcher.GetRef(0) > before;
        }
        bool Acquire() noexcept
        {
            auto lock = g_lock.lock_exclusive();
            return AcquireLocked();
        }
        // The erase of such a control can arrive at any moment from now on, so
        // one reference outlives the sessions. Shutdown releases it.
        void Keep(HWND hwnd) noexcept
        {
            EraseTarget target{};
            if (g_kept.load(std::memory_order_acquire) || !IsEraseTarget(hwnd, target)) return;
            auto lock = g_lock.lock_exclusive();
            if (!g_kept.load(std::memory_order_acquire) && AcquireLocked())
                g_kept.store(true, std::memory_order_release);
        }
        void Begin(HWND hwnd, bool paint) noexcept
        {
            bool list = false;
            if (g_shutdown.load(std::memory_order_acquire)) return;
            Keep(hwnd);
            Session s{};
            s.hwnd = hwnd; s.serial = ++g_serial; s.paint = paint;
            if (IsTarget(hwnd, list))
            {
                s.list = list;
                WCHAR cls[64]{};
                s.number = GetClassNameW(hwnd, cls, _countof(cls)) && _wcsicmp(cls, L"TNumberBox") == 0;
                s.armed = Acquire();
            }
            g_sessions.push_back(s);
        }
        void End(HWND hwnd, bool paint) noexcept
        {
            for (auto it = g_sessions.rbegin(); it != g_sessions.rend(); ++it)
            {
                if (it->hwnd != hwnd || it->paint != paint) continue;
                const Session s = *it;
                g_sessions.erase((it + 1).base());
                if (s.armed)
                {
                    auto lock = g_lock.lock_exclusive();
                    if (!g_shutdown.load(std::memory_order_acquire)) g_dispatcher.EnableHook(0, false);
                }
                return;
            }
        }
        void PaintBegin(HWND h, HMODULE) { Begin(h, true); }
        void PaintEnd(HWND h, HMODULE) { End(h, true); }
        void DcBegin(HWND h, HMODULE) { Begin(h, false); }
        void DcEnd(HWND h, HMODULE) { End(h, false); }

        // The colour an application gave such a control is kept the way the edit
        // hooks keep it in WM_PAINT: the transparent base, then the colour at
        // alpha 100 (the selection colour stays opaque). A pattern, or a brush
        // that tells nothing about itself, is not a colour. 0: nothing was drawn.
        int Tint(HDC dc, const RECT* r, HBRUSH brush) noexcept
        {
            LOGBRUSH lb{};
            if (!brush || GetObjectW(brush, sizeof(lb), &lb) != sizeof(lb) || lb.lbStyle != BS_SOLID) return 0;
            const COLORREF rgb = lb.lbColor & 0xFFFFFFu;
            const HBRUSH tint = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(rgb,
                rgb == (GetSysColor(COLOR_HIGHLIGHT) & 0xFFFFFFu) ? 255 : 100);
            if (!tint) return 0;
            int drawn = 0;
            if (PatBlt(dc, r->left, r->top, r->right-r->left, r->bottom-r->top, BLACKNESS))
                drawn = FillRect(dc, r, tint) ? 2 : 1;
            DeleteObject(tint);
            return drawn;
        }
#if defined(_DEBUG)
        // A fill of the EXE that reached a VCL window which is not in the table,
        // outside that window's sessions: what a parent erased into the DC of its
        // child, or the erase of a class that is not decided here. Reported only:
        // nothing is drawn and nothing depends on it.
        void ReportOther(HWND hwnd, HDC dc, const RECT* r, HBRUSH brush) noexcept
        {
            WCHAR cls[64]{};
            if (!hwnd || !GlassCore::Root::IsFullInjectionMode()
                || GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId()
                || !GetClassNameW(hwnd, cls, _countof(cls)) || cls[0] != L'T') return;
            static thread_local ULONGLONG epoch = 0;
            static thread_local unsigned budget = 0;
            const ULONGLONG now = GetTickCount64();
            if (!epoch || now - epoch >= 1000) { epoch = now; budget = 64; }
            if (!budget) return;
            --budget;
            const RECT rect = r ? *r : RECT{};
            RECT client{}, clip{};
            GetClientRect(hwnd, &client);
            const int clipKind = GetClipBox(dc, &clip);
            POINT viewport{}, window{};
            GetViewportOrgEx(dc, &viewport);
            GetWindowOrgEx(dc, &window);
            LOGBRUSH lb{};
            const bool known = GetObjectW(brush, sizeof(lb), &lb) == sizeof(lb);
            DebugLog(DBG_NORMAL, L"[GlassCore][DelphiBackground] erase-other hwnd=0x%p class=%s kind=%u dc=0x%p rect=[%ld,%ld,%ld,%ld] client=[%ld,%ld] clip=%d[%ld,%ld,%ld,%ld] org=[%ld,%ld,%ld,%ld] style=%d rgb=0x%06X",
                hwnd, cls, static_cast<unsigned>(GlassShared::GlassGetWindowKind(hwnd)), dc,
                rect.left, rect.top, rect.right, rect.bottom, client.right, client.bottom,
                clipKind, clip.left, clip.top, clip.right, clip.bottom,
                viewport.x, viewport.y, window.x, window.y,
                known ? static_cast<int>(lb.lbStyle) : -1,
                known ? static_cast<unsigned>(lb.lbColor & 0xFFFFFFu) : 0u);
        }
#endif

        // Windows erased the window on its own (see the header). The fill is
        // told apart by what it lacks: no session of this window is open on its
        // thread, so the DC is neither BeginPaint's nor GetDC's - it came with
        // the message. Only the fill of the whole client is answered: the default
        // one by the transparent base (1), a memo's own colour by its tint (2).
        // 0: the fill stays the application's.
        int EraseOutside(HDC dc, const RECT* r, HBRUSH brush) noexcept
        {
            // Most fills of a VCL application go to a memory DC: no window there.
            if (GetObjectType(dc) != OBJ_DC) return 0;
            const HWND hwnd = WindowFromDC(dc);
            for (const Session& s : g_sessions) if (s.hwnd == hwnd) return 0;
            EraseTarget target{};
            if (!IsEraseTarget(hwnd, target) || GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId())
            {
#if defined(_DEBUG)
                ReportOther(hwnd, dc, r, brush);
#endif
                return 0;
            }
            // A window DC (frame painting) has the same window behind it, but its
            // clip spans the frame. The DC of an erase never leaves the client.
            RECT clip{};
            const int clipKind = GetClipBox(dc, &clip);
            const bool whole = IsFullClient(hwnd, dc, r) && clipKind > NULLREGION
                && clip.left >= r->left && clip.top >= r->top && clip.right <= r->right && clip.bottom <= r->bottom;
            int cleared = 0;
            if (whole && (!target.number || GlassShared::HasDelphiInputDefaultBackground(hwnd)))
            {
                if (IsSystemBrush(brush, target.windowOnly))
                    cleared = PatBlt(dc, r->left, r->top, r->right-r->left, r->bottom-r->top, BLACKNESS) ? 1 : 0;
                else if (target.windowOnly)
                    cleared = Tint(dc, r, brush);
            }
#if defined(_DEBUG)
            // Every fill that came this far is reported, replaced or not: the
            // line tells what the rectangle, the DC and the brush were.
            static thread_local ULONGLONG epoch = 0;
            static thread_local unsigned budget = 0;
            const ULONGLONG now = GetTickCount64();
            if (!epoch || now - epoch >= 1000) { epoch = now; budget = 32; }
            if (budget)
            {
                --budget;
                const RECT rect = r ? *r : RECT{};
                RECT client{};
                GetClientRect(hwnd, &client);
                LOGBRUSH lb{};
                const bool known = GetObjectW(brush, sizeof(lb), &lb) == sizeof(lb);
                DebugLog(DBG_NORMAL, L"[GlassCore][DelphiBackground] erase-outside hwnd=0x%p class=%s dc=0x%p rect=[%ld,%ld,%ld,%ld] client=[%ld,%ld] clip=%d[%ld,%ld,%ld,%ld] whole=%d style=%d rgb=0x%06X cleared=%d",
                    hwnd, target.cls, dc, rect.left, rect.top, rect.right, rect.bottom, client.right, client.bottom,
                    clipKind, clip.left, clip.top, clip.right, clip.bottom, whole ? 1 : 0,
                    known ? static_cast<int>(lb.lbStyle) : -1,
                    known ? static_cast<unsigned>(lb.lbColor & 0xFFFFFFu) : 0u, cleared);
            }
#endif
            return cleared;
        }

        int WINAPI ObserveFill(HDC dc, const RECT* r, HBRUSH brush)
        {
            decltype(&::FillRect) orig{};
            { auto lock = g_lock.lock_shared(); orig = g_dispatcher.GetOrg<0, decltype(&::FillRect)>(); }
            if (!orig) return FillRect(dc, r, brush);
            if (g_shutdown.load(std::memory_order_acquire) || g_inside) return orig(dc, r, brush);
            // The checks belong to this unit, not to the application's call.
            const DWORD entryError = GetLastError();
            const bool erased = EraseOutside(dc, r, brush) != 0;
            SetLastError(entryError);
            if (erased) return 1;
            if (g_sessions.empty()) return orig(dc, r, brush);
            const size_t slot = g_sessions.size() - 1;
            const Session s = g_sessions[slot];
            if (!s.armed || (s.list && GlassCore::DelphiListBox::IsInsideDraw()) || !IsFullClient(s.hwnd, dc, r))
                return orig(dc, r, brush);
            const bool sys = IsSystemBrush(brush)
                && (!s.number || GlassShared::HasDelphiInputDefaultBackground(s.hwnd));
            // The erase EraseAgain asked for. BeginPaint returned this DC for this
            // window a moment ago; the dispatcher has not recorded it yet. Any
            // other DC and any other brush are the application's own business.
            if (s.erasing)
            {
                if (s.paintDc != dc || !sys
                    || !PatBlt(dc, r->left, r->top, r->right-r->left, r->bottom-r->top, BLACKNESS))
                    return orig(dc, r, brush);
                g_sessions[slot].erasedAgain = true; // nothing above runs the application's code
                return 1;
            }
            // Once BeginPaint has returned, only the exact returned client DC is eligible.
            if (s.paint && s.afterBegin && s.paintDc == dc && sys
                && GlassCore::PaintDispatcher::IsCurrentPaintDcForHwnd(s.hwnd, dc))
            {
                if (PatBlt(dc, r->left, r->top, r->right-r->left, r->bottom-r->top, BLACKNESS))
                {
#if defined(_DEBUG)
                    static thread_local ULONGLONG epoch = 0;
                    static thread_local unsigned budget = 0;
                    const ULONGLONG now = GetTickCount64();
                    if (!epoch || now - epoch >= 1000) { epoch = now; budget = 12; }
                    if (budget) { --budget; DebugLog(DBG_NORMAL, L"[GlassCore][DelphiBackground] paint fill hwnd=0x%p dc=0x%p", s.hwnd, dc); }
#endif
                    return 1;
                }
                return orig(dc, r, brush);
            }
            // Pre-BeginPaint: only an actual window DC may become a paint candidate.
            // GetDC/MEMDC candidate is observation-only until an exact DRAWITEM match.
            const bool candidate = s.paint ? (!s.afterBegin && WindowFromDC(dc) == s.hwnd)
                : (s.list && GlassCore::PaintDispatcher::IsTopWindowDrawingSession(s.hwnd));
            const HGDIOBJ bitmap = candidate ? GetCurrentObject(dc, OBJ_BITMAP) : nullptr;
            const int result = orig(dc, r, brush);
            const DWORD error = GetLastError();
            if (result && candidate && slot < g_sessions.size()
                && g_sessions[slot].serial == s.serial)
            {
                auto& target = g_sessions[slot];
                target.observedDc = dc; target.observedBitmap = bitmap;
                target.observedRect = *r; target.defaultBrush = sys;
            }
            SetLastError(error);
            return result;
        }
        // Windows erased this list before BeginPaint, outside the session (see the
        // header). The message is the one Windows itself sends: the DC to erase
        // and nothing else. Only the default fill of the whole client is replaced,
        // inside the update region BeginPaint selected into the DC.
        bool EraseAgain(size_t slot, HWND hwnd, HDC dc, const PAINTSTRUCT& ps) noexcept
        {
            const unsigned long long serial = g_sessions[slot].serial;
            g_sessions[slot].erasing = true;
            SendMessageW(hwnd, WM_ERASEBKGND, reinterpret_cast<WPARAM>(dc), 0);
            // The handler is the application's: it may have painted other windows.
            if (slot >= g_sessions.size() || g_sessions[slot].serial != serial) return false;
            g_sessions[slot].erasing = false;
            const bool cleared = g_sessions[slot].erasedAgain;
#if defined(_DEBUG)
            static thread_local ULONGLONG epoch = 0;
            static thread_local unsigned budget = 0;
            const ULONGLONG now = GetTickCount64();
            if (!epoch || now - epoch >= 1000) { epoch = now; budget = 12; }
            if (budget) { --budget; DebugLog(DBG_NORMAL, L"[GlassCore][DelphiBackground] erase-again hwnd=0x%p dc=0x%p paint=[%ld,%ld,%ld,%ld] cleared=%d",
                hwnd, dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right, ps.rcPaint.bottom, cleared ? 1 : 0); }
#else
            (void)ps;
#endif
            return cleared;
        }
        bool AfterPaint(HWND hwnd, HDC dc, LPPAINTSTRUCT ps, HMODULE) noexcept
        {
            if (g_shutdown.load(std::memory_order_acquire) || g_sessions.empty()) return false;
            auto& s = g_sessions.back();
            if (!s.armed || !s.paint || s.hwnd != hwnd) return false;
            s.afterBegin = true; s.paintDc = dc;
            // Nothing was filled while BeginPaint ran: the erase came before it.
            if (s.list && !s.observedDc && dc && ps && !IsRectEmpty(&ps->rcPaint))
                return EraseAgain(g_sessions.size() - 1, hwnd, dc, *ps);
            if (!s.defaultBrush || (s.number && !GlassShared::HasDelphiInputDefaultBackground(hwnd)) || s.observedDc != dc || !IsFullClient(hwnd, dc, &s.observedRect)
                || s.observedBitmap != GetCurrentObject(dc, OBJ_BITMAP)) return false;
            RECT clip{};
            if (GetClipBox(dc, &clip) <= NULLREGION) return false;
            const RECT r = s.observedRect;
            const BOOL cleared = PatBlt(dc, r.left, r.top, r.right-r.left, r.bottom-r.top, BLACKNESS);
#if defined(_DEBUG)
            static thread_local ULONGLONG epoch = 0;
            static thread_local unsigned budget = 0;
            const ULONGLONG now = GetTickCount64();
            if (!epoch || now - epoch >= 1000) { epoch = now; budget = 12; }
            if (budget) { --budget; DebugLog(DBG_NORMAL, L"[GlassCore][DelphiBackground] post-BeginPaint hwnd=0x%p dc=0x%p result=%d", hwnd, dc, cleared); }
#endif
            return cleared != FALSE;
        }
    }
    void Startup() noexcept
    {
        g_shutdown.store(false, std::memory_order_release);
        if (HMODULE ep = GlassCore::ModuleHandler::g_entryPointModule)
        {
            auto lock = g_lock.lock_exclusive();
            g_dispatcher.moduleAddress = ep;
            g_dispatcher.CacheHookData();
        }
        GlassCore::PaintDispatcher::RegisterPaintProvider({PaintBegin, PaintEnd});
        GlassCore::PaintDispatcher::RegisterPostPaintProvider({AfterPaint});
        GlassCore::PaintDispatcher::RegisterGetDCProvider({DcBegin, DcEnd});
    }
    void Shutdown() noexcept
    {
        g_shutdown.store(true, std::memory_order_release);
        auto lock = g_lock.lock_exclusive();
        g_dispatcher.DisableAllHooks();
        g_kept.store(false, std::memory_order_release);
        g_sessions.clear();
    }
    void ClearObservedListTail(HWND hwnd, const DRAWITEMSTRUCT* dis) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire) || !dis || !dis->hDC || dis->hwndItem != hwnd
            || !(dis->itemAction & ODA_DRAWENTIRE) || dis->itemID == static_cast<UINT>(-1)) return;
        for (auto it = g_sessions.rbegin(); it != g_sessions.rend(); ++it)
        {
            if (it->hwnd != hwnd || !it->armed || !it->list || !it->defaultBrush || it->observedDc != dis->hDC
                || it->observedBitmap != GetCurrentObject(dis->hDC, OBJ_BITMAP)
                || !IsFullClient(hwnd, dis->hDC, &it->observedRect)) continue;
            // Consume once: never repeatedly erase beneath already painted items.
            it->defaultBrush = false;
            const RECT client = it->observedRect;
            if (dis->rcItem.top != client.top || dis->rcItem.left != client.left) return;
            const LRESULT top = SendMessageW(hwnd, LB_GETTOPINDEX, 0, 0);
            const LRESULT count = SendMessageW(hwnd, LB_GETCOUNT, 0, 0);
            if (top == LB_ERR || count <= 0 || dis->itemID != static_cast<UINT>(top)) return;
            RECT last{};
            if (SendMessageW(hwnd, LB_GETITEMRECT, static_cast<WPARAM>(count - 1), reinterpret_cast<LPARAM>(&last)) == LB_ERR
                || last.bottom < client.top || last.bottom >= client.bottom) return;
            g_inside = true;
            const BOOL cleared = PatBlt(dis->hDC, client.left, last.bottom,
                client.right-client.left, client.bottom-last.bottom, BLACKNESS);
            g_inside = false;
#if defined(_DEBUG)
            static thread_local ULONGLONG epoch = 0;
            static thread_local unsigned budget = 0;
            const ULONGLONG now = GetTickCount64();
            if (!epoch || now - epoch >= 1000) { epoch = now; budget = 12; }
            if (budget) { --budget; DebugLog(DBG_NORMAL, L"[GlassCore][DelphiBackground] list tail hwnd=0x%p dc=0x%p top=%ld bottom=%ld result=%d",
                hwnd, dis->hDC, last.bottom, client.bottom, cleared); }
#else
            (void)cleared;
#endif
            return;
        }
    }
}
