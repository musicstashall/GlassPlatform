// VCL owner-draw: only the HDC and rectangle supplied in WM_DRAWITEM.
#include "CorePch.h"
#include <Hooks/delphi/TListBoxDraw.h>
#include <Hooks/delphi/TBackgroundHooks.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/RootSwcaGradient.h>
#include <GlassControlKind.h>
#include <GlassBackdropMark.h>
#include <WinApiUtils.h>
#include <DebugLog.h>
#include <array>
#include <vector>

namespace GlassCore::DelphiListBox
{
    namespace
    {
        using namespace GlassCore::Hooks;
        using namespace std::literals;
        enum : size_t { FillSlot, ExtTextSlot, DrawTextSlot, Count };
        int WINAPI MyFill(HDC, const RECT*, HBRUSH);
        BOOL WINAPI MyExtText(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
        int WINAPI MyDrawText(HDC, LPCWSTR, int, LPRECT, UINT);
        const HookDispatcherDependency<Count, 3> g_dep{{
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv,
                "ext-ms-win-ntuser-misc-l1-1-0.dll"sv }, "FillRect", reinterpret_cast<void*>(MyFill) },
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv,
                "ext-ms-win-gdi-desktop-l1-1-0.dll"sv }, "ExtTextOutW", reinterpret_cast<void*>(MyExtText) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv,
                "ext-ms-win-ntuser-misc-l1-1-0.dll"sv }, "DrawTextW", reinterpret_cast<void*>(MyDrawText) },
        }};
        HookDispatcher<Count, 3> g_dispatcher{ g_dep, nullptr };
        wil::srwlock g_lock;
        std::atomic<bool> g_shutdown{true};
        struct Session
        {
            HWND hwnd;
            HDC dc;
            RECT item;
            bool colorList;
            bool checkList;
            std::array<bool, Count> hooks{};
        };
        thread_local std::vector<Session> g_stack;
        thread_local bool g_inHook = false;

        template <size_t index, typename T> T Original() noexcept
        {
            auto lock = g_lock.lock_shared();
            return g_dispatcher.GetOrg<index, T>();
        }
        const Session* Current(HDC dc) noexcept
        {
            if (g_shutdown.load(std::memory_order_acquire) || g_inHook || g_stack.empty()) return nullptr;
            const auto& s = g_stack.back();
            return dc && dc == s.dc ? &s : nullptr;
        }
        bool SystemBackground(COLORREF c) noexcept
        {
            c &= 0xFFFFFFu;
            return c == (GetSysColor(COLOR_WINDOW) & 0xFFFFFFu)
                || c == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFFu);
        }
        bool IsColorSample(const Session& s, const RECT& r) noexcept
        {
            const LONG w = s.item.right - s.item.left;
            const LONG h = s.item.bottom - s.item.top;
            // Separate inset swatch, never confuse it with the row background.
            return s.colorList && w > 0 && h > 2 && r.left > s.item.left
                && r.left - s.item.left <= h && r.right > r.left
                && r.right - r.left <= w / 3 && r.top > s.item.top
                && r.bottom < s.item.bottom && r.bottom > r.top;
        }
        int WINAPI MyFill(HDC dc, const RECT* r, HBRUSH brush)
        {
            auto orig = Original<FillSlot, decltype(&::FillRect)>();
            if (!orig) return FillRect(dc, r, brush);
            const auto* current = Current(dc);
            if (!current || !r || !brush) return orig(dc, r, brush);
            const Session s = *current;
            RECT clipped{};
            if (!IntersectRect(&clipped, r, &s.item)) return orig(dc, r, brush);
            // Do not change a call crossing the item boundary.
            if (!EqualRect(&clipped, r)) return orig(dc, r, brush);
            LOGBRUSH lb{};
            if (GetObjectW(brush, sizeof(lb), &lb) != sizeof(lb) || lb.lbStyle != BS_SOLID)
                return orig(dc, r, brush);
            const bool sample = IsColorSample(s, *r);
            // The narrow left part contains the native/VCL checkbox drawing.
            // Keep the checkbox glyphs and complex-clip interior with VCL.
            const LONG itemHeight = s.item.bottom - s.item.top;
            if (s.checkList && r->left == s.item.left && itemHeight > 0
                && r->right - r->left <= 2 * itemHeight)
            {
                // Logged VCL background covers the full item height with a
                // simple clip; the later checkbox fill uses a complex clip.
                RECT clip{};
                if (r->top == s.item.top && r->bottom == s.item.bottom
                    && GetClipBox(dc, &clip) == SIMPLEREGION && SystemBackground(lb.lbColor))
                {
                    if (PatBlt(dc, r->left, r->top, r->right - r->left,
                        r->bottom - r->top, BLACKNESS)) return 1;
                }
                return orig(dc, r, brush);
            }
            g_inHook = true;
            auto guard = wil::scope_exit([]() noexcept { g_inHook = false; });
            if (!sample && SystemBackground(lb.lbColor))
                return PatBlt(dc, r->left, r->top, r->right-r->left, r->bottom-r->top, BLACKNESS) ? 1 : orig(dc, r, brush);
            wil::unique_hbrush alpha{ Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(lb.lbColor, 255) };
            return alpha ? orig(dc, r, alpha.get()) : orig(dc, r, brush);
        }
        BOOL WINAPI MyExtText(HDC dc, int x, int y, UINT options, const RECT* rect,
            LPCWSTR text, UINT count, const INT* dx)
        {
            auto orig = Original<ExtTextSlot, decltype(&::ExtTextOutW)>();
            if (!orig) return ExtTextOutW(dc, x, y, options, rect, text, count, dx);
            const auto* current = Current(dc);
            if (!current || (GetTextAlign(dc) & TA_UPDATECP) || (options & ETO_PDY)
                || ((options & ETO_OPAQUE) && !rect)) return orig(dc, x, y, options, rect, text, count, dx);
            const Session s = *current;
            const int saved = SaveDC(dc);
            if (!saved) return orig(dc, x, y, options, rect, text, count, dx);
            auto restore = wil::scope_exit([&]() noexcept { RestoreDC(dc, saved); });
            if (IntersectClipRect(dc, s.item.left, s.item.top, s.item.right, s.item.bottom) == ERROR)
                return orig(dc, x, y, options, rect, text, count, dx);
            g_inHook = true;
            auto guard = wil::scope_exit([]() noexcept { g_inHook = false; });
            UINT drawOptions = options;
            if ((options & ETO_OPAQUE) && rect)
            {
                const COLORREF bk = GetBkColor(dc);
                // Root may already map COLOR_WINDOW to black in the text DC.
                if ((bk & 0xFFFFFFu) == 0 || SystemBackground(bk))
                {
                    if (!PatBlt(dc, rect->left, rect->top, rect->right-rect->left, rect->bottom-rect->top, BLACKNESS))
                        return orig(dc, x, y, options, rect, text, count, dx);
                }
                else
                {
                    wil::unique_hbrush alpha{ Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(bk, 255) };
                    if (!alpha || !FillRect(dc, rect, alpha.get()))
                        return orig(dc, x, y, options, rect, text, count, dx);
                }
                drawOptions &= ~static_cast<UINT>(ETO_OPAQUE);
            }
            if (!count) return TRUE;
            if (!text) return orig(dc, x, y, drawOptions, rect, text, count, dx);
            if (SUCCEEDED(GlassCore::Text::ExtTextOutWithAlpha(dc, x, y, drawOptions, rect, text, count, dx, orig)))
                return TRUE;
            return orig(dc, x, y, drawOptions, rect, text, count, dx);
        }
        int WINAPI MyDrawText(HDC dc, LPCWSTR text, int count, LPRECT rect, UINT format)
        {
            auto orig = Original<DrawTextSlot, decltype(&::DrawTextW)>();
            if (!orig) return DrawTextW(dc, text, count, rect, format);
            const auto* current = Current(dc);
            if (!current || !rect || !text || (format & (DT_CALCRECT | DT_MODIFYSTRING)))
                return orig(dc, text, count, rect, format);
            const Session s = *current;
            const int saved = SaveDC(dc);
            if (!saved) return orig(dc, text, count, rect, format);
            auto restore = wil::scope_exit([&]() noexcept { RestoreDC(dc, saved); });
            if (IntersectClipRect(dc, s.item.left, s.item.top, s.item.right, s.item.bottom) == ERROR)
                return orig(dc, text, count, rect, format);
            g_inHook = true;
            auto guard = wil::scope_exit([]() noexcept { g_inHook = false; });
            int result = 0;
            if (SUCCEEDED(GlassCore::Text::DrawTextWithAlpha(dc, text, count, rect, format, result))) return result;
            return orig(dc, text, count, rect, format);
        }
    }

    void Startup() noexcept { g_shutdown.store(false, std::memory_order_release); }
    void Shutdown() noexcept
    {
        g_shutdown.store(true, std::memory_order_release);
        auto lock = g_lock.lock_exclusive();
        g_dispatcher.DisableAllHooks();
    }
    bool IsInsideDraw() noexcept { return !g_stack.empty(); }
    bool BeginDraw(HWND parent, const DRAWITEMSTRUCT* dis) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire) || !GlassCore::Root::IsFullInjectionMode()
            || !dis || dis->CtlType != ODT_LISTBOX || !dis->hDC || !dis->hwndItem
            || GetParent(dis->hwndItem) != parent || IsRectEmpty(&dis->rcItem)
            || GlassShared::GlassGetWindowKind(dis->hwndItem) != GlassShared::GlassControlKind::ListBox
            || !GlassShared::GlassIsBackdropAllowedForHwnd(dis->hwndItem)) return false;
        WCHAR cls[64]{};
        if (!GetClassNameW(dis->hwndItem, cls, _countof(cls))) return false;
        const bool color = _wcsicmp(cls, L"TColorListBox") == 0;
        if (!color && _wcsicmp(cls, L"TListBox") != 0 && _wcsicmp(cls, L"TCheckListBox") != 0) return false;
        GlassCore::DelphiBackground::ClearObservedListTail(dis->hwndItem, dis);
        HMODULE mod = GlassCore::ModuleHandler::g_entryPointModule;
        if (!mod) return false;
        Session s{dis->hwndItem, dis->hDC, dis->rcItem, color,
            _wcsicmp(cls, L"TCheckListBox") == 0};
        {
            auto lock = g_lock.lock_exclusive();
            if (g_shutdown.load(std::memory_order_acquire)) return false;
            g_dispatcher.moduleAddress = mod;
            for (size_t i = 0; i < Count; ++i)
            {
                const int before = g_dispatcher.GetRef(i);
                g_dispatcher.EnableHook(i, true);
                s.hooks[i] = g_dispatcher.GetRef(i) > before;
            }
        }
        g_stack.push_back(s);
#if defined(_DEBUG)
        static thread_local ULONGLONG epoch = 0;
        static thread_local unsigned budget = 0;
        const ULONGLONG now = GetTickCount64();
        if (!epoch || now - epoch >= 1000) { epoch = now; budget = 24; }
        if (budget)
        {
            --budget;
            RECT client{};
            GetClientRect(s.hwnd, &client);
            DebugLog(DBG_NORMAL, L"[GlassCore][DelphiListDraw] hwnd=0x%p class=%s dc=0x%p fill=%d ext=%d draw=%d state=0x%X item=%u rect=[%ld,%ld,%ld,%ld] client=[%ld,%ld,%ld,%ld]",
                s.hwnd, cls, s.dc, s.hooks[FillSlot], s.hooks[ExtTextSlot], s.hooks[DrawTextSlot],
                dis->itemState, dis->itemID, s.item.left, s.item.top, s.item.right, s.item.bottom,
                client.left, client.top, client.right, client.bottom);
        }
#endif
        return true;
    }
    void EndDraw() noexcept
    {
        if (g_stack.empty()) return;
        const Session s = g_stack.back();
        g_stack.pop_back();
        auto lock = g_lock.lock_exclusive();
        if (!g_shutdown.load(std::memory_order_acquire))
            for (size_t i = 0; i < Count; ++i)
                if (s.hooks[i]) g_dispatcher.EnableHook(i, false);
    }
}
