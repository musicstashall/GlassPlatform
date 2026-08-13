#include "CorePch.h"

#include <Subsystems/Tooltip.h>
#include <Subsystems/TooltipContext.h>
#include <Subsystems/Settings.h>
#include <Hooks/TooltipHooks.h>
#include <Hooks/KbxLabelHooks.h>
#include <RegHelper.h>
#include <Helpers/ThemeHelper.h>
#include <CommCtrl.h>
#include <cstring>
#include <optional>

namespace
{
    GlassCore::Tooltip::Detail::RuntimeContext g_runtime{};
    std::atomic<bool> g_tooltipSubsystemShutdown{ false };

    [[nodiscard]] bool IsParentSysTreeView32(HWND hWnd) noexcept
    {
        const HWND parent = reinterpret_cast<HWND>(GetWindowLongPtrW(hWnd, GWLP_HWNDPARENT));
        if (!parent)
            return false;
        WCHAR cn[32]{};
        if (!GetClassNameW(parent, cn, static_cast<int>(std::size(cn))))
            return false;
        return _wcsicmp(cn, L"SysTreeView32") == 0;
    }
}

namespace GlassCore::Tooltip::Detail
{
    RuntimeContext& Context() noexcept
    {
        return g_runtime;
    }

    bool IsShuttingDown() noexcept
    {
        return g_tooltipSubsystemShutdown.load(std::memory_order_acquire);
    }
}

namespace GlassCore::Tooltip
{
    [[nodiscard]] static bool ShouldTooltipUseDarkMode(HWND hWnd) noexcept
    {
        const DWORD trayTid = GetWindowThreadProcessId(FindWindowW(L"Shell_TrayWnd", nullptr), nullptr);
        const DWORD secTid = GetWindowThreadProcessId(FindWindowW(L"Shell_SecondaryTrayWnd", nullptr), nullptr);
        const DWORD wndTid = GetWindowThreadProcessId(hWnd, nullptr);

        if (trayTid && trayTid == wndTid)
            return Shared::ThemeHelper::ShouldSystemUseDarkMode();
        if (secTid && secTid == wndTid)
            return Shared::ThemeHelper::ShouldSystemUseDarkMode();

        if (IsParentSysTreeView32(hWnd))
            return Shared::ThemeHelper::ShouldAppsUseDarkMode()
                && Shared::ThemeHelper::IsDarkModeAllowedForApp();

        return Shared::ThemeHelper::ShouldAppsUseDarkMode()
            && Shared::ThemeHelper::IsDarkModeAllowedForApp();
    }

    static void UpdateContext(HWND hWnd, std::optional<bool> forcedDarkMode)
    {
        auto& ctx = Detail::Context();
        ctx.hwnd = hWnd;

        if (forcedDarkMode.has_value())
            ctx.useDarkMode = *forcedDarkMode;
        else
            ctx.useDarkMode = ShouldTooltipUseDarkMode(hWnd);

        GlassCore::Settings::QueryTooltipRenderingContext(ctx.rendering, ctx.useDarkMode);

        ctx.noSystemDropShadow = Shared::RegHelper::Get<DWORD>(
            { L"Tooltip" }, L"NoSystemDropShadow", 0) != 0;

        GlassCore::Settings::QueryBackdropEffectContext(L"Tooltip", ctx.useDarkMode, ctx.backdrop);
        GlassCore::Settings::QueryBorderContext(L"Tooltip", ctx.useDarkMode, ctx.border);
        GlassCore::Settings::ApplyEffect(hWnd, ctx.useDarkMode, ctx.backdrop, ctx.border);
    }

    static void UpdateMarginHooksFromRegistry()
    {
        if (GlassCore::Settings::IsTooltipCustomizationDisabled())
        {
            TooltipHooks::EnableMarginHooks(false);
            return;
        }

        GlassCore::Settings::TooltipRenderingContext rc{};
        GlassCore::Settings::QueryTooltipRenderingContext(rc, Detail::Context().useDarkMode);

        if (rc.marginsType == 0 &&
            rc.margins.cxLeftWidth == 0 && rc.margins.cxRightWidth == 0 &&
            rc.margins.cyTopHeight == 0 && rc.margins.cyBottomHeight == 0)
        {
            TooltipHooks::EnableMarginHooks(false);
        }
        else
        {
            TooltipHooks::EnableMarginHooks(true);
        }
    }

    // ── Экспортируемые коллбэки для TooltipEventProvider / Exports.cpp ────────

    void OnTooltipSubclassAttached(HWND hWnd)
    {
        if (g_tooltipSubsystemShutdown.load(std::memory_order_acquire))
            return;
        if (GlassCore::Settings::IsTooltipCustomizationDisabled())
            return;

        UpdateContext(hWnd, std::nullopt);
        SendMessageW(hWnd, WM_THEMECHANGED, 0, 0);
        SendMessageW(hWnd, TTM_UPDATE, 0, 0);
    }

    void OnTooltipSubclassDetached(HWND hWnd, BOOL isShutdown)
    {
        if (g_tooltipSubsystemShutdown.load(std::memory_order_acquire))
            return;
        if (isShutdown)
            return;

        GlassCore::Settings::DropEffect(L"Tooltip", hWnd);

        auto& ctx = Detail::Context();
        ctx.noMarginsHandling = true;
        SendMessageW(hWnd, WM_THEMECHANGED, 0, 0);
        SendMessageW(hWnd, TTM_UPDATE, 0, 0);
        ctx.noMarginsHandling = false;
    }

    bool OnTooltipPaintSessionBegin(HWND hWnd)
    {
        if (g_tooltipSubsystemShutdown.load(std::memory_order_acquire))
            return false;

        TooltipHooks::EnableComctlPaintHooks(true);

        const auto prevRendering = Detail::Context().rendering;
        UpdateContext(hWnd, std::nullopt);
        const auto& now = Detail::Context().rendering;

        const bool marginsChanged =
            prevRendering.marginsType != now.marginsType ||
            std::memcmp(&prevRendering.margins, &now.margins, sizeof(MARGINS)) != 0;

        if (marginsChanged)
        {
            SendMessageW(hWnd, WM_THEMECHANGED, 0, 0);
            PostMessageW(hWnd, TTM_UPDATE, 0, 0);
            return true;
        }

        return false;
    }

    void OnTooltipPaintSessionEnd(HWND /*hWnd*/)
    {
        TooltipHooks::EnableComctlPaintHooks(false);
    }

    void OnKbxLabelSubclassAttached(HWND hWnd)
    {
        if (g_tooltipSubsystemShutdown.load(std::memory_order_acquire))
            return;
        if (GlassCore::Settings::IsTooltipCustomizationDisabled())
            return;

        const bool dark = Shared::ThemeHelper::ShouldAppsUseDarkMode()
            && Shared::ThemeHelper::IsDarkModeAllowedForApp();
        UpdateContext(hWnd, dark);
    }

    void OnKbxLabelSubclassDetached(HWND hWnd, BOOL isShutdown)
    {
        if (g_tooltipSubsystemShutdown.load(std::memory_order_acquire))
            return;
        if (isShutdown)
            return;
        GlassCore::Settings::DropEffect(L"Tooltip", hWnd);
    }

    void OnKbxLabelPaintSessionBegin(HWND /*hWnd*/)
    {
        if (g_tooltipSubsystemShutdown.load(std::memory_order_acquire))
            return;
        KbxLabelHooks::EnablePaintHooks(true);
    }

    void OnKbxLabelPaintSessionEnd(HWND /*hWnd*/)
    {
        KbxLabelHooks::EnablePaintHooks(false);
    }

    [[nodiscard]] bool IsTooltipIatReady() noexcept
    {
        return TooltipHooks::IsComctl32Ready();
    }

    bool IsTooltipCustomizationDisabledForAttach() noexcept
    {
        return GlassCore::Settings::IsTooltipCustomizationDisabled();
    }

    void OnSubsystemUpdate()
    {
        if (g_tooltipSubsystemShutdown.load(std::memory_order_acquire))
            return;

        if (GlassCore::Settings::IsTooltipCustomizationDisabled())
        {
            TooltipHooks::EnableComctlPaintHooks(false);
            TooltipHooks::EnableMarginHooks(false);
            KbxLabelHooks::EnablePaintHooks(false);
            return;
        }

        UpdateMarginHooksFromRegistry();
    }

    // ── Жизненный цикл ──────────────────────────────────────────────────────

    void Prepare()
    {
        TooltipHooks::Prepare();
        KbxLabelHooks::Prepare();
        DebugLog(DBG_NORMAL, L"[GlassCore][Tooltip] Prepare");
    }

    void Startup()
    {
        g_tooltipSubsystemShutdown.store(false, std::memory_order_release);
        TooltipHooks::Startup();
        KbxLabelHooks::Shutdown();
        KbxLabelHooks::Startup();
        UpdateMarginHooksFromRegistry();
        DebugLog(DBG_NORMAL, L"[GlassCore][Tooltip] Startup");
    }

    void Shutdown()
    {
        // Не сбрасывать флаг в false: иначе после GlassCore_Shutdown субклассы WndCompat ещё живы,
        // WM_PAINT снова включит IAT в comctl32 на наш код → FreeLibrary(GlassCore) → краш процесса.
        g_tooltipSubsystemShutdown.store(true, std::memory_order_release);
        TooltipHooks::Shutdown();
        KbxLabelHooks::Shutdown();
        DebugLog(DBG_NORMAL, L"[GlassCore][Tooltip] Shutdown");
    }

    void Update()
    {
        OnSubsystemUpdate();
    }

    void OnTooltipCreated(HWND hWnd)
    {
        DebugLog(DBG_NORMAL, L"[GlassCore][Tooltip] OnTooltipCreated: 0x%p", hWnd);
    }

    void OnTooltipDestroyed(HWND hWnd)
    {
        DebugLog(DBG_NORMAL, L"[GlassCore][Tooltip] OnTooltipDestroyed: 0x%p", hWnd);
    }
}
