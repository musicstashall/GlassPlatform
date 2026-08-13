// Scoped SetBkColor probe для WM_CTLCOLORSTATIC (донор BWM WinHandler + FormsHooks).

#include "CorePch.h"

#include <Subsystems/CtlColorProbe.h>
#include <Core/Intercept/HookHelper.h>
#include <wil.h>

using namespace GlassCore::Hooks;

namespace GlassCore::CtlColorProbe
{
    namespace
    {
        wil::srwlock g_lock;
        std::atomic<bool> g_shutdown{ false };
        int g_probeRef = 0;
        bool g_hookAttached = false;

        decltype(&::SetBkColor) g_trueSetBkColor = ::SetBkColor;

        COLORREF WINAPI Probe_MySetBkColor(HDC hdc, COLORREF color) noexcept
        {
            if (g_shutdown.load(std::memory_order_acquire) || g_probeRef <= 0)
                return g_trueSetBkColor(hdc, color);

            const COLORREF rgb = color & 0xFFFFFF;
            if (rgb == (GetSysColor(COLOR_WINDOW) & 0xFFFFFF)
                || rgb == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFF))
            {
                color = RGB(0, 0, 0);
            }
            return g_trueSetBkColor(hdc, color);
        }

        void ApplyHookLocked(bool enable) noexcept
        {
            if (enable)
            {
                if (g_hookAttached)
                    return;
                const HRESULT hr = Detours::Write([&]() {
                    Detours::Attach(
                        reinterpret_cast<void**>(&g_trueSetBkColor),
                        reinterpret_cast<void*>(&Probe_MySetBkColor));
                });
                if (FAILED(hr))
                {
                    DebugLog(DBG_ERROR,
                        L"[GlassCore][CtlColorProbe] SetBkColor Attach hr=0x%08X",
                        static_cast<unsigned>(hr));
                    return;
                }
                g_hookAttached = true;
                return;
            }

            if (!g_hookAttached)
                return;
            const HRESULT hr = Detours::Write([&]() {
                Detours::Detach(
                    reinterpret_cast<void**>(&g_trueSetBkColor),
                    reinterpret_cast<void*>(&Probe_MySetBkColor));
            });
            if (FAILED(hr))
            {
                DebugLog(DBG_ERROR,
                    L"[GlassCore][CtlColorProbe] SetBkColor Detach hr=0x%08X",
                    static_cast<unsigned>(hr));
                return;
            }
            g_hookAttached = false;
        }
    }

    void SetBkColorProbeAttach(bool enable) noexcept
    {
        auto lock = g_lock.lock_exclusive();
        if (enable && g_shutdown.load(std::memory_order_acquire))
            return;

        bool hookChanged = false;
        if (enable && g_probeRef == 0)
            hookChanged = true;
        if (!enable && g_probeRef == 1)
            hookChanged = true;

        g_probeRef += enable ? 1 : -1;
        if (g_probeRef < 0)
            g_probeRef = 0;

        if (hookChanged)
            ApplyHookLocked(enable);
    }

    void Shutdown() noexcept
    {
        g_shutdown.store(true, std::memory_order_release);
        auto lock = g_lock.lock_exclusive();
        g_probeRef = 0;
        ApplyHookLocked(false);
    }
}
