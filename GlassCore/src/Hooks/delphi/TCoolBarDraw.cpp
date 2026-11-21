// Отдельный путь Delphi TCoolBar (точный класс окна CoolBar).
// Фон и текст — разные операции: ETO_OPAQUE/FillRect/PatBlt против
// DrawTextW/DrawTextExW/DrawThemeText. Наличие GetWindowTheme(hwnd) не
// определяет маршрут текста внутри comctl32, поэтому проверяем сами API.
// Рамку VCL (DrawEdge) и узкие полосы/точки native chrome не подменяем.

#include "CorePch.h"
#include <Hooks/delphi/TCoolBarDraw.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/RootSwcaGradient.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <GlassControlKind.h>
#include <GlassBackdropMark.h>
#include <DebugLog.h>
#include <wil.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <array>
#include <atomic>
#include <tuple>
#include <unordered_map>
#include <vector>

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::CoolBarControl
{
    namespace
    {
        enum : size_t
        {
            ExtTextOutSlot = 0,
            FillRectSlot,
            PatBltSlot,
            DrawTextSlot,
            DrawTextExSlot,
            DrawThemeTextSlot,
            Count,
        };
        enum : size_t
        {
            ComctlNewModule = 0,
            ComctlLegacyModule,
            EntryPointModule,
            UxThemeModule,
            ModuleCount,
        };

        // Отдельный thunk для каждого источника импорта. При вызове через
        // IATRegistry-цепочку _ReturnAddress уже может указывать в GlassCore,
        // а не в исходный comctl32/uxtheme. По нему нельзя выбирать original.
        template <size_t module> BOOL WINAPI MyExtTextOut(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
        template <size_t module> int WINAPI MyFillRect(HDC, const RECT*, HBRUSH);
        template <size_t module> BOOL WINAPI MyPatBlt(HDC, int, int, int, int, DWORD);
        template <size_t module> int WINAPI MyDrawText(HDC, LPCWSTR, int, LPRECT, UINT);
        template <size_t module> int WINAPI MyDrawTextEx(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);
        template <size_t module> HRESULT WINAPI MyDrawThemeText(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, DWORD, LPCRECT);

        template <size_t module>
        HookDispatcherDependency<Count, 3> Dependencies()
        {
            return {{
                std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                    "ExtTextOutW", reinterpret_cast<PVOID>(MyExtTextOut<module>) },
                std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                    "FillRect", reinterpret_cast<PVOID>(MyFillRect<module>) },
                std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                    "PatBlt", reinterpret_cast<PVOID>(MyPatBlt<module>) },
                std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                    "DrawTextW", reinterpret_cast<PVOID>(MyDrawText<module>) },
                std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                    "DrawTextExW", reinterpret_cast<PVOID>(MyDrawTextEx<module>) },
                std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
                    "DrawThemeText", reinterpret_cast<PVOID>(MyDrawThemeText<module>) },
            }};
        }

        const std::array<HookDispatcherDependency<Count, 3>, ModuleCount> g_dep{
            Dependencies<ComctlNewModule>(), Dependencies<ComctlLegacyModule>(),
            Dependencies<EntryPointModule>(), Dependencies<UxThemeModule>()
        };
        using Dispatcher = HookDispatcher<Count, 3>;
        std::unordered_map<void*, Dispatcher> g_map;
        wil::srwlock g_lock;
        std::atomic<bool> g_shutdown{ true };

        struct HeldModule
        {
            HMODULE base{};
            std::array<bool, Count> acquired{};
        };
        struct Session
        {
            HWND hwnd{};
            HDC suppliedDc{}; // WM_PRINTCLIENT / WM_PAINT с явно переданным DC
            std::array<HeldModule, ModuleCount> modules{};
            bool diag{};
        };
        thread_local std::vector<Session> g_stack;
        thread_local bool g_inHook = false;

        std::array<HMODULE, ModuleCount> Modules() noexcept
        {
            return { GlassCore::ModuleHandler::g_comctl32New,
                GlassCore::ModuleHandler::g_comctl32Legacy,
                GlassCore::ModuleHandler::g_entryPointModule,
                GetModuleHandleW(L"uxtheme.dll") };
        }
        constexpr const wchar_t* kModuleNames[ModuleCount]{
            L"comctl32-v6", L"comctl32-legacy", L"application", L"uxtheme"
        };

        // Отдельные бюджеты: заливки не должны съедать диагностику текста.
        bool TakeDiagBudget(size_t category) noexcept
        {
            static thread_local ULONGLONG epoch[3]{};
            static thread_local unsigned budget[3]{};
            const ULONGLONG now = GetTickCount64();
            if (!epoch[category] || now - epoch[category] >= 1000)
            {
                epoch[category] = now;
                budget[category] = 16;
            }
            if (!budget[category])
                return false;
            --budget[category];
            return true;
        }

        // Вызывается только под g_lock.
        Dispatcher& CacheModule(HMODULE base, size_t module)
        {
            if (auto it = g_map.find(static_cast<void*>(base)); it != g_map.end())
                return it->second;
            auto [it, inserted] = g_map.emplace(std::piecewise_construct,
                std::forward_as_tuple(static_cast<void*>(base)),
                std::forward_as_tuple(g_dep[module], base));
            if (inserted)
                it->second.CacheHookData();
            return it->second;
        }

        HeldModule AcquireModule(HMODULE base, size_t module, bool diag)
        {
            HeldModule held{ base };
            unsigned mask = 0;
            {
                auto lock = g_lock.lock_exclusive();
                auto& dispatcher = CacheModule(base, module);
                for (size_t i = 0; i < Count; ++i)
                {
                    const int before = dispatcher.GetRef(i);
                    dispatcher.EnableHook(i, true);
                    held.acquired[i] = dispatcher.GetRef(i) > before;
                    if (held.acquired[i])
                        mask |= 1u << i;
                }
            }
            if (diag)
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][CoolBar] hooks module=%s base=0x%p acquired=0x%02X (ETO=01 Fill=02 Pat=04 Text=08 TextEx=10 ThemeText=20)",
                    kModuleNames[module], base, mask);
            return held;
        }

        void ReleaseModule(const HeldModule& held)
        {
            if (!held.base)
                return;
            auto lock = g_lock.lock_exclusive();
            auto it = g_map.find(static_cast<void*>(held.base));
            if (it == g_map.end())
                return;
            for (size_t i = 0; i < Count; ++i)
                if (held.acquired[i])
                    it->second.EnableHook(i, false);
        }

        template <size_t module, size_t index, typename T>
        T GetOrigSafe()
        {
            const HMODULE base = Modules()[module];
            auto lock = g_lock.lock_shared();
            const auto it = g_map.find(static_cast<void*>(base));
            return it != g_map.end() ? it->second.template GetOrg<index, T>() : nullptr;
        }

        [[nodiscard]] bool IsPaintDc(HDC hdc) noexcept
        {
            if (!hdc || g_stack.empty() || g_shutdown.load(std::memory_order_acquire))
                return false;
            const Session& session = g_stack.back();
            const HWND sessionHwnd = session.hwnd;
            if (!sessionHwnd || !IsWindow(sessionHwnd)
                || !GlassShared::GlassIsBackdropAllowedForHwnd(sessionHwnd))
                return false;
            // Print DC — достоверный аргумент сообщения; может быть memory DC
            // либо DC родителя. Произвольные memory DC сессии не разрешаем.
            if (session.suppliedDc && hdc == session.suppliedDc)
                return true;
            if (const HWND owner = WindowFromDC(hdc))
                return owner == sessionHwnd;
            return GetObjectType(hdc) == OBJ_MEMDC
                && GlassCore::PaintDispatcher::IsTopWindowDrawingSession(sessionHwnd)
                && GlassCore::PaintDispatcher::IsPaintStackDcForHwnd(sessionHwnd, hdc);
        }

        [[nodiscard]] bool IsBackgroundBrush(HBRUSH brush) noexcept
        {
            LOGBRUSH lb{};
            if (!brush || GetObjectW(brush, sizeof(lb), &lb) != sizeof(lb) || lb.lbStyle != BS_SOLID)
                return false;
            const COLORREF color = lb.lbColor & 0xFFFFFF;
            return color == (GetSysColor(COLOR_WINDOW) & 0xFFFFFF)
                || color == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFF)
                || color == (GetSysColor(COLOR_3DFACE) & 0xFFFFFF)
                || color == RGB(255, 255, 255)
                || color == RGB(0, 0, 0);
        }

        [[nodiscard]] bool IsChromeStripe(const RECT* rc) noexcept
        {
            if (!rc || IsRectEmpty(rc))
                return false;
            const HWND hwnd = g_stack.empty() ? nullptr : g_stack.back().hwnd;
            const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 96;
            const int edgeX = GetSystemMetricsForDpi(SM_CXEDGE, dpi ? dpi : 96);
            const int edgeY = GetSystemMetricsForDpi(SM_CYEDGE, dpi ? dpi : 96);
            // uxtheme рисует ETO_OPAQUE не только фон, но и нижнюю линию.
            // Полный rc операции, не GetClipBox: частичный repaint фона
            // не превращается из-за клипа в «границу».
            return rc->right - rc->left <= edgeX || rc->bottom - rc->top <= edgeY;
        }

        BOOL BlackenRect(HDC hdc, const RECT* rc) noexcept
        {
            return hdc && rc && PatBlt(hdc, rc->left, rc->top,
                rc->right - rc->left, rc->bottom - rc->top, BLACKNESS);
        }

        void Diag(const wchar_t* what, HDC hdc, const RECT* rc) noexcept
        {
            if (!TakeDiagBudget(2))
                return;
            DebugLog(DBG_NORMAL,
                L"[GlassCore][CoolBar] %s hdc=0x%p dcWnd=0x%p rect=[%ld,%ld,%ld,%ld] color=0x%08X bk=0x%08X bkMode=%d",
                what, hdc, WindowFromDC(hdc),
                rc ? rc->left : 0, rc ? rc->top : 0, rc ? rc->right : 0, rc ? rc->bottom : 0,
                static_cast<unsigned>(GetTextColor(hdc)),
                static_cast<unsigned>(GetBkColor(hdc)), GetBkMode(hdc));
        }

        void TextDiag(const wchar_t* api, size_t module, HDC hdc, const RECT* rc,
            UINT flags, const wchar_t* reason, HRESULT hr, int result) noexcept
        {
            if (!TakeDiagBudget(1))
                return;
            DebugLog(DBG_NORMAL,
                L"[GlassCore][CoolBar] text api=%s module=%s reason=%s hr=0x%08X result=%d hwnd=0x%p dc=0x%p dcWnd=0x%p dcType=%lu host=0x%p stackOwner=0x%p rect=[%ld,%ld,%ld,%ld] flags=0x%X color=0x%08X bk=0x%08X bkMode=%d",
                api, kModuleNames[module], reason, static_cast<unsigned>(hr), result,
                g_stack.empty() ? nullptr : g_stack.back().hwnd, hdc, WindowFromDC(hdc),
                GetObjectType(hdc), GlassCore::PaintDispatcher::GetPaintSessionHostHwnd(),
                GlassCore::PaintDispatcher::GetStackOwnerForDc(hdc),
                rc ? rc->left : 0, rc ? rc->top : 0, rc ? rc->right : 0, rc ? rc->bottom : 0,
                flags, static_cast<unsigned>(GetTextColor(hdc)),
                static_cast<unsigned>(GetBkColor(hdc)), GetBkMode(hdc));
        }

        bool TryDrawCaption(const wchar_t* api, size_t module, HDC hdc, LPCWSTR text,
            int cch, LPRECT rc, UINT flags, int& result, HTHEME theme = nullptr, int part = 0, int state = 0)
        {
            if (g_inHook || g_stack.empty())
                return false;
            if (!IsPaintDc(hdc))
            {
                TextDiag(api, module, hdc, rc, flags, L"not-session-dc", S_FALSE, 0);
                return false;
            }
            if (!text || cch == 0 || !rc || IsRectEmpty(rc)
                || (flags & (DT_CALCRECT | DT_MODIFYSTRING)) != 0)
                return false;

            wil::unique_hfont themeFont;
            const int saved = SaveDC(hdc);
            if (!saved)
                return false;
            auto restoreDc = wil::scope_exit([&]() noexcept { RestoreDC(hdc, saved); });
            if (theme)
            {
                LOGFONTW font{};
                if (SUCCEEDED(GetThemeFont(theme, hdc, part, state, TMT_FONT, &font)))
                {
                    themeFont.reset(CreateFontIndirectW(&font));
                    if (themeFont)
                        SelectObject(hdc, themeFont.get());
                }
                COLORREF color{};
                if (SUCCEEDED(GetThemeColor(theme, part, state, TMT_TEXTCOLOR, &color)))
                    SetTextColor(hdc, color);
            }
            // Не только ETO_OPAQUE: в classic-ветке сам BkMode=OPAQUE
            // создаёт белый прямоугольник под глифами, даже без этого флага.
            SetBkMode(hdc, TRANSPARENT);
            g_inHook = true;
            auto leaveHook = wil::scope_exit([]() noexcept { g_inHook = false; });
            const HRESULT hr = GlassCore::Text::DrawTextWithAlpha(hdc, text, cch, rc, flags, result, true);
            TextDiag(api, module, hdc, rc, flags, SUCCEEDED(hr) ? L"alpha" : L"alpha-failed", hr, result);
            return SUCCEEDED(hr);
        }

        template <size_t module>
        BOOL WINAPI MyExtTextOut(HDC hdc, int x, int y, UINT options, const RECT* rc,
            LPCWSTR text, UINT cch, const INT* dx)
        {
            const auto orig = GetOrigSafe<module, ExtTextOutSlot, decltype(&::ExtTextOutW)>();
            if (!orig)
                return ExtTextOutW(hdc, x, y, options, rc, text, cch, dx);
            if (g_inHook || !IsPaintDc(hdc))
            {
                if (!g_inHook && !g_stack.empty() && text && cch)
                    TextDiag(L"ExtTextOutW", module, hdc, rc, options, L"not-session-dc", S_FALSE, 0);
                return orig(hdc, x, y, options, rc, text, cch, dx);
            }

            g_inHook = true;
            auto leaveHook = wil::scope_exit([]() noexcept { g_inHook = false; });
            UINT drawOptions = options;
            if ((drawOptions & ETO_OPAQUE) != 0 && rc)
            {
                if ((!text || !cch) && IsChromeStripe(rc))
                {
                    Diag(L"chrome-fill-preserved", hdc, rc);
                    return orig(hdc, x, y, options, rc, text, cch, dx);
                }
                if (!BlackenRect(hdc, rc))
                    return orig(hdc, x, y, options, rc, text, cch, dx);
                drawOptions &= ~static_cast<UINT>(ETO_OPAQUE);
                if (!text || !cch)
                {
                    Diag(L"band-fill", hdc, rc);
                    return TRUE;
                }
            }
            if (!text || !cch)
                return orig(hdc, x, y, drawOptions, rc, text, cch, dx);

            const int saved = SaveDC(hdc);
            if (!saved)
                return orig(hdc, x, y, drawOptions, rc, text, cch, dx);
            auto restoreDc = wil::scope_exit([&]() noexcept { RestoreDC(hdc, saved); });
            SetBkMode(hdc, TRANSPARENT);
            const HRESULT hr = GlassCore::Text::ExtTextOutWithAlpha(
                hdc, x, y, drawOptions, rc, text, cch, dx, orig);
            TextDiag(L"ExtTextOutW", module, hdc, rc, drawOptions,
                SUCCEEDED(hr) ? L"alpha" : L"alpha-failed", hr, SUCCEEDED(hr) ? 1 : 0);
            return SUCCEEDED(hr) ? TRUE : orig(hdc, x, y, drawOptions, rc, text, cch, dx);
        }

        template <size_t module>
        int WINAPI MyFillRect(HDC hdc, const RECT* rc, HBRUSH brush)
        {
            const auto orig = GetOrigSafe<module, FillRectSlot, decltype(&::FillRect)>();
            if (!orig)
                return FillRect(hdc, rc, brush);
            if (g_inHook || !rc || !IsPaintDc(hdc) || !IsBackgroundBrush(brush) || IsChromeStripe(rc))
                return orig(hdc, rc, brush);
            g_inHook = true;
            auto leaveHook = wil::scope_exit([]() noexcept { g_inHook = false; });
            if (!BlackenRect(hdc, rc))
                return orig(hdc, rc, brush);
            Diag(L"fill", hdc, rc);
            return TRUE;
        }

        template <size_t module>
        BOOL WINAPI MyPatBlt(HDC hdc, int x, int y, int cx, int cy, DWORD rop)
        {
            const auto orig = GetOrigSafe<module, PatBltSlot, decltype(&::PatBlt)>();
            if (!orig)
                return PatBlt(hdc, x, y, cx, cy, rop);
            if (g_inHook || !IsPaintDc(hdc))
                return orig(hdc, x, y, cx, cy, rop);
            const RECT rc{ x, y, x + cx, y + cy };
            if ((rop != WHITENESS && rop != PATCOPY) || IsChromeStripe(&rc)
                || (rop == PATCOPY && !IsBackgroundBrush(static_cast<HBRUSH>(GetCurrentObject(hdc, OBJ_BRUSH)))))
                return orig(hdc, x, y, cx, cy, rop);
            g_inHook = true;
            auto leaveHook = wil::scope_exit([]() noexcept { g_inHook = false; });
            Diag(L"patblt", hdc, &rc);
            return orig(hdc, x, y, cx, cy, BLACKNESS);
        }

        template <size_t module>
        int WINAPI MyDrawText(HDC hdc, LPCWSTR text, int cch, LPRECT rc, UINT flags)
        {
            const auto orig = GetOrigSafe<module, DrawTextSlot, decltype(&::DrawTextW)>();
            if (!orig)
                return DrawTextW(hdc, text, cch, rc, flags);
            int result = 0;
            if (TryDrawCaption(L"DrawTextW", module, hdc, text, cch, rc, flags, result))
                return result;
            return orig(hdc, text, cch, rc, flags);
        }

        template <size_t module>
        int WINAPI MyDrawTextEx(HDC hdc, LPWSTR text, int cch, LPRECT rc, UINT flags, LPDRAWTEXTPARAMS dtp)
        {
            const auto orig = GetOrigSafe<module, DrawTextExSlot, decltype(&::DrawTextExW)>();
            if (!orig)
                return DrawTextExW(hdc, text, cch, rc, flags, dtp);
            int result = 0;
            if (!dtp && TryDrawCaption(L"DrawTextExW", module, hdc, text, cch, rc, flags, result))
                return result;
            if (dtp && !g_inHook && !g_stack.empty())
                TextDiag(L"DrawTextExW", module, hdc, rc, flags, L"drawtext-params", S_FALSE, 0);
            return orig(hdc, text, cch, rc, flags, dtp);
        }

        template <size_t module>
        HRESULT WINAPI MyDrawThemeText(HTHEME theme, HDC hdc, int part, int state,
            LPCWSTR text, int cch, DWORD flags, DWORD flags2, LPCRECT rc)
        {
            const auto orig = GetOrigSafe<module, DrawThemeTextSlot, decltype(&::DrawThemeText)>();
            if (!orig)
                return DrawThemeText(theme, hdc, part, state, text, cch, flags, flags2, rc);
            int result = 0;
            RECT drawRect = rc ? *rc : RECT{};
            if (!flags2 && TryDrawCaption(L"DrawThemeText", module, hdc, text, cch,
                rc ? &drawRect : nullptr, flags, result, theme, part, state))
                return S_OK;
            if (flags2 && !g_inHook && !g_stack.empty())
                TextDiag(L"DrawThemeText", module, hdc, rc, flags, L"theme-flags2", S_FALSE, 0);
            return orig(theme, hdc, part, state, text, cch, flags, flags2, rc);
        }
    }

    bool OnPaintBegin(HWND hWnd, HDC suppliedDc)
    {
        const wchar_t* skip = nullptr;
        if (g_shutdown.load(std::memory_order_acquire)) skip = L"shutdown";
        else if (!GlassCore::Root::IsFullInjectionMode()) skip = L"not-full";
        else if (!hWnd || !IsWindow(hWnd)) skip = L"invalid-window";
        else if (!GlassShared::GlassIsBackdropAllowedForHwnd(hWnd)) skip = L"backdrop-disabled";
        else if (GlassShared::GlassGetWindowKind(hWnd) != GlassShared::GlassControlKind::CoolBar) skip = L"wrong-kind";
        if (skip)
        {
            if (TakeDiagBudget(0))
                DebugLog(DBG_NORMAL, L"[GlassCore][CoolBar] OnPaintBegin: skip=%s hwnd=0x%p", skip, hWnd);
            return false;
        }

        Session session{};
        session.hwnd = hWnd;
        session.suppliedDc = suppliedDc;
        session.diag = TakeDiagBudget(0);
        const auto modules = Modules();
        for (size_t m = 0; m < ModuleCount; ++m)
        {
            if (!modules[m])
                continue;
            bool duplicate = false;
            for (size_t prev = 0; prev < m; ++prev)
                if (modules[prev] == modules[m])
                    duplicate = true;
            if (!duplicate)
                session.modules[m] = AcquireModule(modules[m], m, session.diag);
        }
        g_stack.push_back(session);
        if (session.diag)
            DebugLog(DBG_NORMAL,
                L"[GlassCore][CoolBar] OnPaintBegin: hwnd=0x%p suppliedDc=0x%p theme=0x%p depth=%zu",
                hWnd, suppliedDc, GetWindowTheme(hWnd), g_stack.size());
        return true;
    }

    void OnPaintEnd()
    {
        if (g_stack.empty())
            return;
        const Session session = g_stack.back();
        g_stack.pop_back();
        // Только реально приобретённые refs и те же базы, что в Begin.
        // Ни загрузка второго comctl32, ни вложенный paint не меняют пару.
        for (size_t m = ModuleCount; m > 0; --m)
            ReleaseModule(session.modules[m - 1]);
        if (session.diag)
            DebugLog(DBG_NORMAL, L"[GlassCore][CoolBar] OnPaintEnd: hwnd=0x%p depth=%zu",
                session.hwnd, g_stack.size());
    }

    void Prepare() {}

    void Startup()
    {
        const auto modules = Modules();
        {
            auto lock = g_lock.lock_exclusive();
            for (size_t m = 0; m < ModuleCount; ++m)
                if (modules[m])
                    CacheModule(modules[m], m);
        }
        g_shutdown.store(false, std::memory_order_release);
        DebugLog(DBG_NORMAL,
            L"[GlassCore][CoolBar] Startup: text-routes=DrawTextW|DrawTextExW|DrawThemeText; print-dc=1; both-comctl=1");
    }

    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_seq_cst);
        auto lock = g_lock.lock_exclusive();
        for (auto& entry : g_map)
            entry.second.DisableAllHooks();
        g_map.clear();
        g_stack.clear();
    }
}
