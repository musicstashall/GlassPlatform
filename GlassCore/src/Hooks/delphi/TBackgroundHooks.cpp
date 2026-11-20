// VCL client background: observe EXE FillRect before BeginPaint returns, then
// validate its HDC against BeginPaint or a subsequent WM_DRAWITEM before editing.
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
                || _wcsicmp(cls, L"TColorListBox") == 0;
            if (isList) return GlassShared::GlassGetWindowKind(hwnd) == GlassShared::GlassControlKind::ListBox;
            return (_wcsicmp(cls, L"TNumberBox") == 0 || _wcsicmp(cls, L"THotKey") == 0)
                && GlassShared::GlassGetWindowKind(hwnd) == GlassShared::GlassControlKind::Edit;
        }
        bool IsSystemBrush(HBRUSH brush) noexcept
        {
            LOGBRUSH lb{};
            if (!brush || GetObjectW(brush, sizeof(lb), &lb) != sizeof(lb) || lb.lbStyle != BS_SOLID) return false;
            const COLORREF rgb = lb.lbColor & 0xFFFFFFu;
            return rgb == (GetSysColor(COLOR_WINDOW) & 0xFFFFFFu)
                || rgb == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFFu);
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
        bool Acquire() noexcept
        {
            auto lock = g_lock.lock_exclusive();
            if (g_shutdown.load(std::memory_order_acquire)) return false;
            HMODULE ep = GlassCore::ModuleHandler::g_entryPointModule;
            if (!ep) return false;
            g_dispatcher.moduleAddress = ep;
            const int before = g_dispatcher.GetRef(0);
            g_dispatcher.EnableHook(0, true);
            return g_dispatcher.GetRef(0) > before;
        }
        void Begin(HWND hwnd, bool paint) noexcept
        {
            bool list = false;
            if (g_shutdown.load(std::memory_order_acquire)) return;
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

        int WINAPI ObserveFill(HDC dc, const RECT* r, HBRUSH brush)
        {
            decltype(&::FillRect) orig{};
            { auto lock = g_lock.lock_shared(); orig = g_dispatcher.GetOrg<0, decltype(&::FillRect)>(); }
            if (!orig) return FillRect(dc, r, brush);
            if (g_shutdown.load(std::memory_order_acquire) || g_inside || g_sessions.empty()) return orig(dc, r, brush);
            const size_t slot = g_sessions.size() - 1;
            const Session s = g_sessions[slot];
            if (!s.armed || (s.list && GlassCore::DelphiListBox::IsInsideDraw()) || !IsFullClient(s.hwnd, dc, r))
                return orig(dc, r, brush);
            const bool sys = IsSystemBrush(brush)
                && (!s.number || GlassShared::HasDelphiInputDefaultBackground(s.hwnd));
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
        bool AfterPaint(HWND hwnd, HDC dc, LPPAINTSTRUCT, HMODULE) noexcept
        {
            if (g_shutdown.load(std::memory_order_acquire) || g_sessions.empty()) return false;
            auto& s = g_sessions.back();
            if (!s.armed || !s.paint || s.hwnd != hwnd) return false;
            s.afterBegin = true; s.paintDc = dc;
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
