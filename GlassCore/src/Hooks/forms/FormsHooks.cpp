// Глобальные Detours для System.Windows.Forms.ni.dll (подавление фона WinForms Static/Button).
// BWM: FormsHooks::ColorBrushAttach + ModuleHandler при LOAD forms.ni.

#include "CorePch.h"

#include <Hooks/forms/FormsHooks.h>
#include <Hooks/forms/FormsHooksInternal.h>
#include <Core/Intercept/HookHelper.h>
#include <WinApiUtils.h>
#include <wil.h>

using namespace GlassCore::Hooks;

namespace GlassCore::FormsHooks
{
    void ForceDetachDrawTextEx() noexcept;
    void ForceDetachDrawThemeBackground() noexcept;

    static wil::srwlock              g_lock;
    static std::atomic<bool>         g_shutdown{ false };
    static bool                      g_brushHooksAttached{ false };

    static decltype(&::CreateSolidBrush) g_trueCreateSolidBrush = ::CreateSolidBrush;
    static decltype(&::GetSysColorBrush) g_trueGetSysColorBrush = ::GetSysColorBrush;

    HBRUSH WINAPI FormsNi_MyCreateSolidBrush(COLORREF color)
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return g_trueCreateSolidBrush(color);

        const HMODULE callerMod = Internal::GetCallerModule(_ReturnAddress());
        if (!Internal::IsSystemWindowsFormsNiModule(callerMod))
            return g_trueCreateSolidBrush(color);

        DebugLog(DBG_NORMAL,
            L"[GlassCore][FormsHooks] CreateSolidBrush: caller=0x%p color=0x%08X",
            callerMod, static_cast<unsigned>(color));

        const COLORREF rgb = color & 0x00FFFFFF;
        if (rgb == (GetSysColor(COLOR_WINDOW) & 0x00FFFFFF)
            || rgb == (GetSysColor(COLOR_BTNFACE) & 0x00FFFFFF)
            || rgb == (GetSysColor(COLOR_BTNSHADOW) & 0x00FFFFFF))
        {
            color = (color & 0xFF000000) | RGB(0, 0, 0);
            return g_trueCreateSolidBrush(color);
        }

        if (HBRUSH alphaBrush = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255))
            return alphaBrush;

        return g_trueCreateSolidBrush(color);
    }

    HBRUSH WINAPI FormsNi_MyGetSysColorBrush(int nIndex)
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return g_trueGetSysColorBrush(nIndex);

        const HMODULE callerMod = Internal::GetCallerModule(_ReturnAddress());
        if (Internal::IsSystemWindowsFormsNiModule(callerMod)
            && (nIndex == COLOR_WINDOW || nIndex == COLOR_BTNFACE))
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][FormsHooks] GetSysColorBrush: caller=0x%p index=%d → BLACK_BRUSH",
                callerMod, nIndex);
            return static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        }

        return g_trueGetSysColorBrush(nIndex);
    }

    void ColorBrushAttach(bool enable) noexcept
    {
        auto lock = g_lock.lock_exclusive();

        if (enable && g_shutdown.load(std::memory_order_acquire))
            return;

        if (enable)
        {
            if (g_brushHooksAttached)
                return;

            const HRESULT hr = Detours::Write([&]() {
                Detours::Attach(
                    reinterpret_cast<void**>(&g_trueCreateSolidBrush),
                    reinterpret_cast<void*>(&FormsNi_MyCreateSolidBrush));
                Detours::Attach(
                    reinterpret_cast<void**>(&g_trueGetSysColorBrush),
                    reinterpret_cast<void*>(&FormsNi_MyGetSysColorBrush));
            });
            if (FAILED(hr))
            {
                DebugLog(DBG_ERROR,
                    L"[GlassCore][FormsHooks] ColorBrushAttach Attach hr=0x%08X",
                    static_cast<unsigned>(hr));
                return;
            }

            g_brushHooksAttached = true;
            DebugLog(DBG_IMPORTANT, L"[GlassCore][FormsHooks] ColorBrushAttach: attached");
            return;
        }

        if (!g_brushHooksAttached)
            return;

        const HRESULT hr = Detours::Write([&]() {
            Detours::Detach(
                reinterpret_cast<void**>(&g_trueCreateSolidBrush),
                reinterpret_cast<void*>(&FormsNi_MyCreateSolidBrush));
            Detours::Detach(
                reinterpret_cast<void**>(&g_trueGetSysColorBrush),
                reinterpret_cast<void*>(&FormsNi_MyGetSysColorBrush));
        });
        if (FAILED(hr))
        {
            DebugLog(DBG_ERROR,
                L"[GlassCore][FormsHooks] ColorBrushAttach Detach hr=0x%08X",
                static_cast<unsigned>(hr));
            return;
        }

        g_brushHooksAttached = false;
        DebugLog(DBG_IMPORTANT, L"[GlassCore][FormsHooks] ColorBrushAttach: detached");
    }

    void Startup() noexcept
    {
        g_shutdown.store(false, std::memory_order_release);
        if (GetModuleHandleW(L"System.Windows.Forms.ni.dll"))
            ColorBrushAttach(true);
    }

    void Shutdown() noexcept
    {
        g_shutdown.store(true, std::memory_order_release);
        ForceDetachDrawTextEx();
        ForceDetachDrawThemeBackground();
        ColorBrushAttach(false);
    }
}
