// msctls_progress32 — BWM PaintHandler: без темы → ExtTextOut; с темой → DataDetach (не переносим).
// Glass: только сессия без визуальной темы окна — IAT ExtTextOutW на gdi32.

#include "CorePch.h"
#include <Hooks/ProgressBar.h>
#include <Subsystems/RenderText.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/PaintDispatcher.h>
#include <GlassControlKind.h>
#include <wil.h>

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::ProgressBarControl
{
    namespace
    {
        thread_local int g_paintDepth = 0;

        static std::atomic<bool> g_shutdown{ false };

        BOOL WINAPI Overall_MyExtTextOutW(
            HDC hdc, int x, int y, UINT options, const RECT* lprect,
            LPCWSTR lpString, UINT c, const INT* lpDx);

        enum OverallHookIndex : size_t
        {
            OHK_EXTTEXTOUT = 0,
            OHK_COUNT
        };

        inline const HookDispatcherDependency<OHK_COUNT, 3> g_dep
        { {
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                "ExtTextOutW", reinterpret_cast<PVOID>(Overall_MyExtTextOutW) },
        } };

        using OverallDispatcher = HookDispatcher<OHK_COUNT, 3>;
        static std::unordered_map<void*, OverallDispatcher> g_overallMap;
        static wil::srwlock                                                g_overallLock;

        static bool EnableHooksOnModule(void* baseAddress, size_t index, bool enable)
        {
            if (!baseAddress) return false;
            auto lock = g_overallLock.lock_exclusive();
            auto it = g_overallMap.find(baseAddress);
            if (it == g_overallMap.end())
            {
                auto [inserted, ok] = g_overallMap.emplace(
                    std::piecewise_construct,
                    std::forward_as_tuple(baseAddress),
                    std::forward_as_tuple(g_dep, baseAddress));
                it = inserted;
                it->second.CacheHookData();
            }
            else
                it->second.moduleAddress = baseAddress;
            it->second.EnableHook(index, enable);
            return true;
        }

        static void DisableAllOverallHooks()
        {
            auto lock = g_overallLock.lock_exclusive();
            for (auto& [addr, d] : g_overallMap)
                d.DisableAllHooks();
            g_overallMap.clear();
        }

        static HMODULE GetCallerModule(void* ra)
        {
            HMODULE hMod = nullptr;
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(ra),
                &hMod);
            return hMod;
        }

        template <size_t index, typename T>
        static T GetOverallOrigSafe(HMODULE callerModule)
        {
            auto lock = g_overallLock.lock_shared();
            auto it = g_overallMap.find(static_cast<void*>(callerModule));
            if (it != g_overallMap.end())
                return it->second.template GetOrg<index, T>();
            return nullptr;
        }

        [[nodiscard]] static void* ResolveGdi() noexcept
        {
            if (void* p = GetModuleHandleW(L"gdi32full.dll")) return p;
            return GetModuleHandleW(L"gdi32.dll");
        }

        BOOL WINAPI Overall_MyExtTextOutW(
            HDC hdc, int x, int y, UINT options, const RECT* lprect,
            LPCWSTR lpString, UINT c, const INT* lpDx)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_EXTTEXTOUT, decltype(&ExtTextOutW)>(callerMod);

            if (!orig)
                return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

            if (g_paintDepth <= 0)
                return orig(hdc, x, y, options, lprect, lpString, c, lpDx);

            if ((options & ETO_OPAQUE) && lprect)
                PatBlt(hdc, lprect->left, lprect->top,
                    lprect->right - lprect->left, lprect->bottom - lprect->top, BLACKNESS);

            if (c && lpString && !(options & ETO_GLYPH_INDEX))
            {
                RECT clip{};
                GetClipBox(hdc, &clip);
                clip.left = x;
                clip.top = y;
                int result = 0;
                if (!FAILED(GlassCore::Text::DrawTextWithAlpha(
                        hdc, lpString, static_cast<int>(c), &clip,
                        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP, result)))
                    return TRUE;
            }
            return orig(hdc, x, y, options, lprect, lpString, c, lpDx);
        }

        static void OnBeginPaintGlobal(HWND hWnd, HMODULE /*callerMod*/)
        {
            using namespace GlassShared;
            if (g_shutdown.load(std::memory_order_acquire)) return;
            if (GlassGetWindowKind(hWnd) != GlassControlKind::ProgressBar)
                return;
            if (GetWindowTheme(hWnd))
                return;

            ++g_paintDepth;
            if (void* gdi = ResolveGdi())
                EnableHooksOnModule(gdi, OHK_EXTTEXTOUT, true);
        }

        static void OnEndPaintGlobal(HWND hWnd, HMODULE /*callerMod*/)
        {
            using namespace GlassShared;
            if (GlassGetWindowKind(hWnd) != GlassControlKind::ProgressBar)
                return;
            if (g_paintDepth <= 0)
                return;
            --g_paintDepth;
            if (void* gdi = ResolveGdi())
                EnableHooksOnModule(gdi, OHK_EXTTEXTOUT, false);
        }
    }

    void Prepare() {}

    void Startup()
    {
        GlassCore::PaintDispatcher::RegisterPaintProvider({ OnBeginPaintGlobal, OnEndPaintGlobal });
        auto cache = [&](void* mod)
            {
                if (!mod) return;
                auto lock = g_overallLock.lock_exclusive();
                auto [it, inserted] = g_overallMap.emplace(
                    std::piecewise_construct,
                    std::forward_as_tuple(mod),
                    std::forward_as_tuple(g_dep, mod));
                if (inserted)
                    it->second.CacheHookData();
            };
        cache(ResolveGdi());
    }

    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_seq_cst);
        DisableAllOverallHooks();
        g_paintDepth = 0;
        g_shutdown.store(false, std::memory_order_seq_cst);
    }
}
