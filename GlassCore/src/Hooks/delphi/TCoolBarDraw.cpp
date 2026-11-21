// Отрисовка Delphi TCoolBar (класс окна CoolBar).
//
// Почему отдельный путь, а не rebar-ветка:
//   * у окна CoolBar нет темы — DrawThemeBackground не вызывается вообще;
//   * фон полосы задаётся пустым ExtTextOutW(ETO_OPAQUE, ""), цвет берётся из
//     SetBkColor (см. API-Monitor-5.log);
//   * текст полосы comctl32 рисует обычным DrawTextExW (не DrawThemeText);
//   * рамку рисует сама VCL через DrawEdge на GetWindowDC.
// Поэтому здесь свои слоты (ExtTextOutW/FillRect/PatBlt/DrawTextExW) и свои
// правила: фон → прозрачность, текст → alpha-путь Text::DrawTextWithAlpha.

#include "CorePch.h"
#include <Hooks/delphi/TCoolBarDraw.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/RootSwcaGradient.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <GlassControlKind.h>
#include <GlassBackdropMark.h>
#include <DebugLog.h>
#include <wil.h>
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
            DrawTextExSlot,
            Count,
        };

        struct Session
        {
            HWND hwnd{};
        };

        thread_local std::vector<Session> g_stack;
        thread_local bool g_inHook = false;
        thread_local unsigned g_textDepth = 0;
        std::atomic<bool> g_shutdown{ true };

        int WINAPI MyExtTextOut(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
        int WINAPI MyFillRect(HDC, const RECT*, HBRUSH);
        BOOL WINAPI MyPatBlt(HDC, int, int, int, int, DWORD);
        int WINAPI MyDrawTextEx(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);

        const HookDispatcherDependency<Count, 3> g_dep{ {
            // OHK_EXT TEXTOUT — фон полосы (ETO_OPAQUE) и глифы
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                "ExtTextOutW", reinterpret_cast<PVOID>(MyExtTextOut) },
            // заливка клиента/полосы сплошной кистью
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                "FillRect", reinterpret_cast<PVOID>(MyFillRect) },
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                "PatBlt", reinterpret_cast<PVOID>(MyPatBlt) },
            // текст полосы
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                "DrawTextExW", reinterpret_cast<PVOID>(MyDrawTextEx) },
        } };

        using Dispatcher = HookDispatcher<Count, 3>;

        std::unordered_map<void*, Dispatcher> g_map;
        wil::srwlock g_lock;

        bool EnableOnModule(void* baseAddress, size_t index, bool enable)
        {
            if (!baseAddress)
                return false;

            auto lock = g_lock.lock_exclusive();
            auto it = g_map.find(baseAddress);
            if (it == g_map.end())
            {
                auto [inserted, ok] = g_map.emplace(
                    std::piecewise_construct,
                    std::forward_as_tuple(baseAddress),
                    std::forward_as_tuple(g_dep, baseAddress));
                it = inserted;
                it->second.CacheHookData();
            }
            else
            {
                it->second.moduleAddress = baseAddress;
            }
            it->second.EnableHook(index, enable);
            return true;
        }

        template <size_t index, typename T>
        T GetOrigSafe(HMODULE callerModule)
        {
            auto lock = g_lock.lock_shared();
            auto it = g_map.find(static_cast<void*>(callerModule));
            if (it != g_map.end())
                return it->second.template GetOrg<index, T>();
            return nullptr;
        }

        HMODULE CallerModule(void* returnAddress)
        {
            HMODULE mod = nullptr;
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(returnAddress), &mod);
            return mod;
        }

        [[nodiscard]] bool IsPaintDc(HDC hdc) noexcept
        {
            if (!hdc || g_stack.empty())
                return false;
            const HWND sessionHwnd = g_stack.back().hwnd;
            if (!sessionHwnd || !IsWindow(sessionHwnd))
                return false;
            return WindowFromDC(hdc) == sessionHwnd;
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

        void EnsureTextColor(HDC hdc) noexcept
        {
            if (!hdc)
                return;
            if ((GetTextColor(hdc) & 0xFFFFFF) == 0)
                SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
        }

        // Прозрачность под color key: тот же прямоугольник, но BLACKNESS.
        BOOL BlackenRect(HDC hdc, const RECT* rc) noexcept
        {
            if (!hdc || !rc)
                return FALSE;
            return PatBlt(hdc, rc->left, rc->top,
                rc->right - rc->left, rc->bottom - rc->top, BLACKNESS);
        }

        void Diag(const wchar_t* what, HDC hdc, const RECT* rc) noexcept
        {
            static thread_local ULONGLONG epoch = 0;
            static thread_local unsigned budget = 0;
            const ULONGLONG now = GetTickCount64();
            if (!epoch || now - epoch >= 1000) { epoch = now; budget = 16; }
            if (!budget)
                return;
            --budget;
            DebugLog(DBG_NORMAL,
                L"[GlassCore][CoolBar] %s hdc=0x%p rect=[%ld,%ld,%ld,%ld] color=0x%08X bk=0x%08X",
                what, hdc,
                rc ? rc->left : 0, rc ? rc->top : 0, rc ? rc->right : 0, rc ? rc->bottom : 0,
                static_cast<unsigned>(GetTextColor(hdc)),
                static_cast<unsigned>(GetBkColor(hdc)));
        }

        // ── ExtTextOutW: заливка полосы и глифы ────────────────────────────────
        int WINAPI MyExtTextOut(
            HDC hdc, int x, int y, UINT options, const RECT* rc,
            LPCWSTR text, UINT cch, const INT* dx)
        {
            const HMODULE callerMod = CallerModule(_ReturnAddress());
            auto orig = GetOrigSafe<ExtTextOutSlot, decltype(&ExtTextOutW)>(callerMod);
            if (!orig)
                return ExtTextOutW(hdc, x, y, options, rc, text, cch, dx);
            if (g_inHook || g_stack.empty() || !IsPaintDc(hdc))
                return orig(hdc, x, y, options, rc, text, cch, dx);

            UINT drawOptions = options;
            if ((drawOptions & ETO_OPAQUE) != 0)
            {
                // Фон: тема/VCL заливает прямоугольник цветом из SetBkColor.
                if (rc)
                {
                    g_inHook = true;
                    BlackenRect(hdc, rc);
                    g_inHook = false;
                }
                drawOptions &= ~static_cast<UINT>(ETO_OPAQUE);
                if (!text || !cch)
                {
                    Diag(L"band-fill", hdc, rc);
                    return TRUE;
                }
            }

            if (!text || !cch)
                return orig(hdc, x, y, drawOptions, rc, text, cch, dx);

            // Глифы: alpha-путь того же движка, что у списков.
            const auto origExt = reinterpret_cast<decltype(&::ExtTextOutW)>(
                GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "ExtTextOutW"));
            if (origExt)
            {
                g_inHook = true;
                const HRESULT hr = GlassCore::Text::ExtTextOutWithAlpha(
                    hdc, x, y, drawOptions, rc, text, cch, dx, origExt);
                g_inHook = false;
                if (SUCCEEDED(hr))
                {
                    Diag(L"glyph", hdc, rc);
                    return TRUE;
                }
            }
            return orig(hdc, x, y, drawOptions, rc, text, cch, dx);
        }

        int WINAPI MyFillRect(HDC hdc, const RECT* rc, HBRUSH brush)
        {
            const HMODULE callerMod = CallerModule(_ReturnAddress());
            auto orig = GetOrigSafe<FillRectSlot, decltype(&FillRect)>(callerMod);
            if (!orig)
                return FillRect(hdc, rc, brush);
            if (g_inHook || !rc || g_stack.empty() || !IsPaintDc(hdc) || !IsBackgroundBrush(brush))
                return orig(hdc, rc, brush);

            g_inHook = true;
            const BOOL ok = BlackenRect(hdc, rc);
            g_inHook = false;
            Diag(L"fill", hdc, rc);
            return ok ? TRUE : 0;
        }

        BOOL WINAPI MyPatBlt(HDC hdc, int x, int y, int cx, int cy, DWORD rop)
        {
            const HMODULE callerMod = CallerModule(_ReturnAddress());
            auto orig = GetOrigSafe<PatBltSlot, decltype(&PatBlt)>(callerMod);
            if (!orig)
                return PatBlt(hdc, x, y, cx, cy, rop);
            if (g_inHook || g_stack.empty() || !IsPaintDc(hdc))
                return orig(hdc, x, y, cx, cy, rop);
            if (rop != WHITENESS && rop != PATCOPY)
                return orig(hdc, x, y, cx, cy, rop);

            g_inHook = true;
            const BOOL ok = orig(hdc, x, y, cx, cy, BLACKNESS);
            g_inHook = false;
            DebugLog(DBG_NORMAL,
                L"[GlassCore][CoolBar] patblt hdc=0x%p (%d,%d) %dx%d rop=0x%08X -> BLACKNESS ok=%d",
                hdc, x, y, cx, cy, static_cast<unsigned>(rop), ok ? 1 : 0);
            return ok;
        }

        // ── DrawTextExW: текст полосы ─────────────────────────────────────────
        int WINAPI MyDrawTextEx(
            HDC hdc, LPWSTR text, int cch, LPRECT rc, UINT format, LPDRAWTEXTPARAMS dtp)
        {
            const HMODULE callerMod = CallerModule(_ReturnAddress());
            auto orig = GetOrigSafe<DrawTextExSlot, decltype(&DrawTextExW)>(callerMod);
            if (!orig)
                return DrawTextExW(hdc, text, cch, rc, format, dtp);
            if (g_inHook || g_textDepth > 0 || g_stack.empty() || !rc || dtp)
                return orig(hdc, text, cch, rc, format, dtp);
            if (!IsPaintDc(hdc))
                return orig(hdc, text, cch, rc, format, dtp);
            if ((format & (DT_CALCRECT | DT_MODIFYSTRING)) != 0)
                return orig(hdc, text, cch, rc, format, dtp);

            EnsureTextColor(hdc);
            ++g_textDepth;
            auto popDepth = wil::scope_exit([]() noexcept { --g_textDepth; });

            g_inHook = true;
            int result = 0;
            const HRESULT hr = GlassCore::Text::DrawTextWithAlpha(hdc, text, cch, rc, format, result, true);
            g_inHook = false;
            Diag(hr == S_OK ? L"text" : L"text-failed", hdc, rc);
            if (SUCCEEDED(hr))
                return result;
            return orig(hdc, text, cch, rc, format, dtp);
        }
    } // namespace

    void OnPaintBegin(HWND hWnd)
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return;
        if (!GlassCore::Root::IsFullInjectionMode())
            return;
        if (!hWnd || !IsWindow(hWnd))
            return;
        if (!GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
            return;
        if (GlassShared::GlassGetWindowKind(hWnd) != GlassShared::GlassControlKind::CoolBar)
            return;

        HMODULE comctl = GlassCore::ModuleHandler::g_comctl32New;
        if (!comctl)
            comctl = GlassCore::ModuleHandler::g_comctl32Legacy;
        if (!comctl)
            return;

        g_stack.push_back(Session{ hWnd });

        for (size_t i = 0; i < Count; ++i)
        {
            EnableOnModule(comctl, i, true);
            EnableOnModule(GlassCore::ModuleHandler::g_entryPointModule, i, true);
        }
        if (HMODULE ux = GetModuleHandleW(L"uxtheme.dll"))
            for (size_t i = 0; i < Count; ++i)
                EnableOnModule(ux, i, true);

        DebugLog(DBG_NORMAL, L"[GlassCore][CoolBar] OnPaintBegin: hwnd=0x%p comctl=0x%p depth=%zu",
            hWnd, comctl, g_stack.size());
    }

    void OnPaintEnd()
    {
        if (g_stack.empty())
            return;

        const Session session = g_stack.back();
        g_stack.pop_back();

        HMODULE comctl = GlassCore::ModuleHandler::g_comctl32New;
        if (!comctl)
            comctl = GlassCore::ModuleHandler::g_comctl32Legacy;
        HMODULE ux = GetModuleHandleW(L"uxtheme.dll");

        for (size_t i = 0; i < Count; ++i)
        {
            EnableOnModule(comctl, i, false);
            EnableOnModule(GlassCore::ModuleHandler::g_entryPointModule, i, false);
            if (ux)
                EnableOnModule(ux, i, false);
        }

        DebugLog(DBG_NORMAL, L"[GlassCore][CoolBar] OnPaintEnd: hwnd=0x%p depth=%zu",
            session.hwnd, g_stack.size());
    }

    void Prepare() {}

    void Startup()
    {
        g_shutdown.store(false, std::memory_order_release);

        auto cache = [](void* mod)
        {
            if (!mod)
                return;
            auto lock = g_lock.lock_exclusive();
            auto [it, inserted] = g_map.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(mod),
                std::forward_as_tuple(g_dep, mod));
            if (inserted)
            {
                it->second.CacheHookData();
                DebugLog(DBG_NORMAL, L"[GlassCore][CoolBar] Startup: cached 0x%p", mod);
            }
        };
        cache(GlassCore::ModuleHandler::g_comctl32New);
        cache(GlassCore::ModuleHandler::g_comctl32Legacy);
        cache(GlassCore::ModuleHandler::g_entryPointModule);
        cache(GetModuleHandleW(L"uxtheme.dll"));
    }

    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_seq_cst);
        auto lock = g_lock.lock_exclusive();
        for (auto& entry : g_map)
            entry.second.DisableAllHooks();
        g_map.clear();
        g_stack.clear();
        g_shutdown.store(false, std::memory_order_seq_cst);
    }
}
