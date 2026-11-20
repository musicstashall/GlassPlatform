// VCL owner-draw: only the HDC and rectangle supplied in WM_DRAWITEM.
#include "CorePch.h"
#include <Hooks/delphi/TListBoxDraw.h>
#include <Hooks/delphi/TBackgroundHooks.h>
#include <Hooks/UxThemeHooks.h>
#include <Helpers/ThemeHelper.h>
#include <vssym32.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/RootSwcaGradient.h>
#include <GlassControlKind.h>
#include <GlassBackdropMark.h>
#include <WinApiUtils.h>
#include <DebugLog.h>
#include <array>
#include <climits>
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
            bool themeDetourAcquired{};
            std::array<bool, Count> hooks{};
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
            unsigned long long diagId{};
            UINT diagItem{};
            int diagState{};
            RECT diagThemeRect{};
            bool diagHasThemeRect{};
#endif
        };
        thread_local std::vector<Session> g_stack;
        thread_local bool g_inHook = false;
        thread_local bool g_inCheckTheme = false;

#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
        thread_local unsigned long long g_diagSequence = 0;
        thread_local ULONGLONG g_diagEpoch = 0;
        thread_local unsigned g_diagBudget = 0;
        bool CheckProbeBudget() noexcept
        {
            const ULONGLONG now = GetTickCount64();
            if (!g_diagEpoch || now - g_diagEpoch >= 1000) { g_diagEpoch = now; g_diagBudget = 96; }
            if (!g_diagBudget) return false;
            --g_diagBudget;
            return true;
        }
        void LogCheckPixels(const Session& s, const wchar_t* stage, const RECT& r, int state) noexcept
        {
            if (!s.checkList || !s.dc || !CheckProbeBudget()) return;
            const long long width = static_cast<long long>(r.right) - r.left;
            const long long height = static_cast<long long>(r.bottom) - r.top;
            if (width < 2 || height < 2 || width > 256 || height > 256 || r.left == LONG_MIN) return;
            const DWORD lastError = GetLastError();
            GdiFlush();
            // A few RGB samples, not a screenshot or text. CLR_INVALID means the
            // DC could not be read. GetPixel cannot report destination alpha.
            const POINT points[]{ {r.left - 1, r.top + static_cast<LONG>(height / 2)},
                {r.left, r.top}, {r.left + 1, r.top + 1},
                {r.left + static_cast<LONG>(width / 2), r.top + static_cast<LONG>(height / 2)},
                {r.right - 1, r.bottom - 1}, {r.right, r.top + static_cast<LONG>(height / 2)} };
            COLORREF pixels[_countof(points)]{};
            for (size_t i = 0; i < _countof(points); ++i)
                pixels[i] = PtInRect(&s.item, points[i]) ? GetPixel(s.dc, points[i].x, points[i].y) : CLR_INVALID;
            RECT clip{};
            const int clipKind = GetClipBox(s.dc, &clip);
            DebugLog(DBG_NORMAL,
                L"[GlassCore][CheckBgProbe] stage=%s id=%llu hwnd=0x%p item=%u state=%d dc=0x%p dcType=%u rect=[%ld,%ld,%ld,%ld] clipKind=%d rgbOutsideLeft=0x%08X rgbTL=0x%08X rgbInner=0x%08X rgbCenter=0x%08X rgbBR=0x%08X rgbOutsideRight=0x%08X",
                stage, s.diagId, s.hwnd, s.diagItem, state, s.dc, static_cast<unsigned>(GetObjectType(s.dc)),
                r.left, r.top, r.right, r.bottom, clipKind, static_cast<unsigned>(pixels[0]),
                static_cast<unsigned>(pixels[1]), static_cast<unsigned>(pixels[2]),
                static_cast<unsigned>(pixels[3]), static_cast<unsigned>(pixels[4]), static_cast<unsigned>(pixels[5]));
            SetLastError(lastError);
        }
        void LogCheckBuffer(const Session& s, HPAINTBUFFER buffer, const RECT& r, int state) noexcept
        {
            if (!s.checkList || !CheckProbeBudget()) return;
            const DWORD lastError = GetLastError();
            const long long width = static_cast<long long>(r.right) - r.left;
            const long long height = static_cast<long long>(r.bottom) - r.top;
            RGBQUAD* bits = nullptr;
            int stride = 0;
            const HRESULT bitsResult = GetBufferedPaintBits(buffer, &bits, &stride);
            unsigned a0 = 0, a255 = 0, apart = 0, white = 0;
            if (SUCCEEDED(bitsResult) && bits && width > 0 && height > 0
                && width <= 256 && height <= 256 && stride >= width)
            {
                GdiFlush();
                for (int y = 0; y < height; ++y)
                    for (int x = 0; x < width; ++x)
                    {
                        const RGBQUAD& p = bits[static_cast<size_t>(y) * stride + x];
                        if (p.rgbReserved == 0) ++a0;
                        else if (p.rgbReserved == 255) ++a255;
                        else ++apart;
                        if (p.rgbReserved != 0 && p.rgbRed >= 240 && p.rgbGreen >= 240 && p.rgbBlue >= 240) ++white;
                    }
            }
            DebugLog(DBG_NORMAL,
                L"[GlassCore][CheckBgProbe] stage=theme.buffer id=%llu hwnd=0x%p item=%u state=%d bitsHr=0x%08X available=%d size=%lldx%lld stride=%d alpha0=%u alpha255=%u alphaPartial=%u whiteNonzeroAlpha=%u",
                s.diagId, s.hwnd, s.diagItem, state, static_cast<unsigned>(bitsResult), bits ? 1 : 0,
                width, height, stride, a0, a255, apart, white);
            SetLastError(lastError);
        }
        void LogCheckFill(const Session& s, HDC dc, const RECT& r, const LOGBRUSH& brush,
            const wchar_t* branch, BOOL result) noexcept
        {
            if (!s.checkList || !CheckProbeBudget()) return;
            const DWORD lastError = GetLastError();
            RECT clip{};
            const int clipKind = GetClipBox(dc, &clip);
            DebugLog(DBG_NORMAL,
                L"[GlassCore][CheckBgProbe] stage=fill id=%llu hwnd=0x%p item=%u dc=0x%p rect=[%ld,%ld,%ld,%ld] brushStyle=%u brushColor=0x%08X clipKind=%d branch=%s result=%d",
                s.diagId, s.hwnd, s.diagItem, dc, r.left, r.top, r.right, r.bottom,
                static_cast<unsigned>(brush.lbStyle), static_cast<unsigned>(brush.lbColor), clipKind, branch, result);
            SetLastError(lastError);
        }
#endif

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
            // VCL's own DC clip governs each fill, including checkbox backgrounds.
            // Do not infer or alter checkbox geometry or clipping here.
            g_inHook = true;
            auto guard = wil::scope_exit([]() noexcept { g_inHook = false; });
            if (!sample && SystemBackground(lb.lbColor))
            {
                const BOOL cleared = PatBlt(dc, r->left, r->top, r->right-r->left, r->bottom-r->top, BLACKNESS);
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
                LogCheckFill(s, dc, *r, lb, L"system-background", cleared);
#endif
                return cleared ? 1 : orig(dc, r, brush);
            }
            wil::unique_hbrush alpha{ Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(lb.lbColor, 255) };
            const int result = alpha ? orig(dc, r, alpha.get()) : orig(dc, r, brush);
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
            LogCheckFill(s, dc, *r, lb, alpha ? L"alpha-brush" : L"original", result);
#endif
            return result;
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
        {
            auto lock = g_lock.lock_exclusive();
            g_dispatcher.DisableAllHooks();
        }
        // Shutdown can occur on this thread within an owner-draw session. Other
        // threads keep their own marker and release it in EndDraw.
        for (auto& s : g_stack)
        {
            if (s.themeDetourAcquired)
            {
                s.themeDetourAcquired = false;
                GlassCore::Hooks::UxThemeHooks::EnableThemeHooks(false);
            }
        }
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
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
        s.diagId = ++g_diagSequence;
        s.diagItem = dis->itemID;
#endif
        g_stack.push_back(s);
        if (s.checkList)
        {
            g_stack.back().themeDetourAcquired = GlassCore::Hooks::UxThemeHooks::EnableThemeHooks(true);
            s.themeDetourAcquired = g_stack.back().themeDetourAcquired;
        }
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
            DebugLog(DBG_NORMAL, L"[GlassCore][DelphiListDraw] hwnd=0x%p class=%s dc=0x%p fill=%d ext=%d draw=%d theme=%d state=0x%X item=%u rect=[%ld,%ld,%ld,%ld] client=[%ld,%ld,%ld,%ld]",
                s.hwnd, cls, s.dc, s.hooks[FillSlot], s.hooks[ExtTextSlot], s.hooks[DrawTextSlot], s.themeDetourAcquired,
                dis->itemState, dis->itemID, s.item.left, s.item.top, s.item.right, s.item.bottom,
                client.left, client.top, client.right, client.bottom);
        }
#endif
        return true;
    }
    bool TryCheckListThemeBackground(decltype(&::DrawThemeBackground) orig,
        HTHEME theme, HDC dc, int part, int state, LPCRECT rect, LPCRECT clip,
        HRESULT& result) noexcept
    {
        if (!orig || g_inCheckTheme || !theme || !rect || IsRectEmpty(rect) || part != BP_CHECKBOX)
            return false;
        const auto* current = Current(dc);
        if (!current || !current->checkList || !current->themeDetourAcquired) return false;
        const Session session = *current;
        WCHAR themeClass[MAX_PATH + 1]{};
        if (FAILED(Shared::ThemeHelper::GetThemeClass(theme, themeClass, MAX_PATH))
            || _wcsicmp(themeClass, L"Button") != 0) return false;

#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
        g_stack.back().diagThemeRect = *rect;
        g_stack.back().diagState = state;
        g_stack.back().diagHasThemeRect = true;
        LogCheckPixels(session, L"theme.before", *rect, state);
#endif
        // Use the theme's own drawing and per-pixel alpha, not a reconstructed
        // checkbox, colour key, or a substitute clipping region. The buffer is
        // scoped to the API's original rectangle and the original clip is passed on.
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        BP_PAINTPARAMS params{sizeof(BP_PAINTPARAMS), 0, nullptr, &blend};
        HDC bufferDc = nullptr;
        HPAINTBUFFER buffer = BeginBufferedPaint(dc, rect, BPBF_TOPDOWNDIB, &params, &bufferDc);
        if (!buffer || !bufferDc)
        {
            if (buffer) EndBufferedPaint(buffer, FALSE);
            return false;
        }
        if (FAILED(BufferedPaintClear(buffer, nullptr)))
        {
            EndBufferedPaint(buffer, FALSE);
            return false;
        }
        g_inCheckTheme = true;
        const HRESULT draw = orig(theme, bufferDc, part, state, rect, clip);
        g_inCheckTheme = false;
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
        LogCheckBuffer(session, buffer, *rect, state);
#endif
        const HRESULT end = EndBufferedPaint(buffer, SUCCEEDED(draw));
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
        LogCheckPixels(session, L"theme.after", *rect, state);
#endif
#if defined(_DEBUG)
        static thread_local ULONGLONG epoch = 0;
        static thread_local unsigned budget = 0;
        const ULONGLONG now = GetTickCount64();
        if (!epoch || now - epoch >= 1000) { epoch = now; budget = 12; }
        if (budget)
        {
            --budget;
            DebugLog(DBG_NORMAL, L"[GlassCore][DelphiListDraw] checkbox DTB hwnd=0x%p dc=0x%p part=%d state=%d draw=0x%08X end=0x%08X",
                session.hwnd, dc, part, state, static_cast<unsigned>(draw), static_cast<unsigned>(end));
        }
#endif
        if (FAILED(draw) || FAILED(end)) return false;
        result = draw;
        return true;
    }
    void EndDraw() noexcept
    {
        if (g_stack.empty()) return;
        const Session s = g_stack.back();
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
        if (s.diagHasThemeRect) LogCheckPixels(s, L"item.end", s.diagThemeRect, s.diagState);
#endif
        g_stack.pop_back();
        if (s.themeDetourAcquired)
            GlassCore::Hooks::UxThemeHooks::EnableThemeHooks(false);
        auto lock = g_lock.lock_exclusive();
        if (!g_shutdown.load(std::memory_order_acquire))
            for (size_t i = 0; i < Count; ++i)
                if (s.hooks[i]) g_dispatcher.EnableHook(i, false);
    }
}
