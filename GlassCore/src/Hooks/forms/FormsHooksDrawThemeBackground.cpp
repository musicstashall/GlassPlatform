// Unified DrawThemeBackground Detour for System.Windows.Forms.ni.dll.

#include "CorePch.h"

#include <Hooks/forms/FormsHooks.h>
#include <Hooks/forms/FormsHooksInternal.h>
#include <Hooks/forms/FormsHooksDtbPolicies.h>
#include <Core/Intercept/HookHelper.h>
#include <Helpers/ThemeHelper.h>
#include <Subsystems/PaintDispatcher.h>
#include <DebugLog.h>
#include <wil.h>

using namespace GlassCore::Hooks;

namespace GlassCore::FormsHooks
{
    namespace
    {
        std::atomic<bool> g_drawThemeBgShutdown{ false };
        wil::srwlock      g_drawThemeBgLock;
        size_t            g_drawThemeBgRef{};
        decltype(&::DrawThemeBackground) g_trueDrawThemeBackground = ::DrawThemeBackground;
        thread_local uint32_t g_formsNiPaintSeq{};
    }

    HRESULT WINAPI FormsNi_MyDrawThemeBackground(
        HTHEME hTheme,
        HDC hdc,
        int iPartId,
        int iStateId,
        LPCRECT pRect,
        LPCRECT pClipRect)
    {
        if (g_drawThemeBgShutdown.load(std::memory_order_acquire))
            return g_trueDrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        const HMODULE callerMod = DetourGetContainingModule(_ReturnAddress());

        if (!Internal::IsSystemWindowsFormsNiModule(callerMod))
            return g_trueDrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        const auto orig = reinterpret_cast<Shared::WinApiUtils::DrawThemeBackground_pfn>(g_trueDrawThemeBackground);
        HRESULT hr = S_OK;

        if (DrawThemeBackground::Button::TryPaint(
                orig, callerMod, hTheme, hdc, iPartId, iStateId, pRect, pClipRect, hr))
            return hr;

        if (DrawThemeBackground::ComboBox::TryPaint(
                orig, callerMod, hTheme, hdc, iPartId, iStateId, pRect, pClipRect, hr))
            return hr;

        if (DrawThemeBackground::DateTimePicker::TryPaint(
                orig, hTheme, hdc, iPartId, iStateId, pRect, pClipRect, hr))
            return hr;

        return g_trueDrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
    }

    void DrawThemeBackgroundAttach(bool enable) noexcept
    {
        auto lock = g_drawThemeBgLock.lock_exclusive();

        if (enable)
        {
            if (!GetModuleHandleW(L"System.Windows.Forms.ni.dll"))
                return;

            if (g_drawThemeBgRef == 0)
            {
                const HRESULT attachHr = Detours::Write([&]() {
                    Detours::Attach(
                        reinterpret_cast<void**>(&g_trueDrawThemeBackground),
                        reinterpret_cast<void*>(&FormsNi_MyDrawThemeBackground));
                });
                if (FAILED(attachHr))
                {
                    DebugLog(DBG_ERROR,
                        L"[GlassCore][FormsHooks] DrawThemeBackgroundAttach Attach hr=0x%08X",
                        static_cast<unsigned>(attachHr));
                    return;
                }
            }
            ++g_drawThemeBgRef;
            return;
        }

        if (g_drawThemeBgRef == 0)
            return;

        --g_drawThemeBgRef;
        if (g_drawThemeBgRef == 0)
        {
            const HRESULT detachHr = Detours::Write([&]() {
                Detours::Detach(
                    reinterpret_cast<void**>(&g_trueDrawThemeBackground),
                    reinterpret_cast<void*>(&FormsNi_MyDrawThemeBackground));
            });
            if (FAILED(detachHr))
                DebugLog(DBG_ERROR,
                    L"[GlassCore][FormsHooks] DrawThemeBackgroundAttach Detach hr=0x%08X",
                    static_cast<unsigned>(detachHr));
        }
    }

    void ForceDetachDrawThemeBackground() noexcept
    {
        g_drawThemeBgShutdown.store(true, std::memory_order_release);
        auto lock = g_drawThemeBgLock.lock_exclusive();
        if (g_drawThemeBgRef == 0)
            return;
        const HRESULT hr = Detours::Write([&]() {
            Detours::Detach(
                reinterpret_cast<void**>(&g_trueDrawThemeBackground),
                reinterpret_cast<void*>(&FormsNi_MyDrawThemeBackground));
        });
        if (FAILED(hr))
            DebugLog(DBG_ERROR,
                L"[GlassCore][FormsHooks] ForceDetachDrawThemeBackground hr=0x%08X",
                static_cast<unsigned>(hr));
        g_drawThemeBgRef = 0;
    }
}
