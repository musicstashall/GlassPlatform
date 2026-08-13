using System.Threading;
using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;

namespace GlassSettings.Core.Flyout;

/// <summary>
/// Live-reload панели задач — паритет TaskbarBorders.md (без HWND_BROADCAST для tray-only).
/// </summary>
public static class FlyoutLiveReload
{
    private const string TrayRegSub = "Tray";
    private const int BorderPreviewThrottleMs = 150;
    private static long _lastBorderPreviewReloadTick;

    public static void NotifyFlyoutChange() =>
        LiveReloadScheduler.ScheduleFlyoutReload();

    public static void NotifyTaskbarBorderMode() =>
        LiveReloadScheduler.ScheduleFlags(ReloadRenderFlags.TaskbarBoth, dwmNotify: false);

    public static void NotifyTaskbarBorderModeNow() =>
        InvokeReloadImmediate(ReloadRenderFlags.TaskbarBoth);

    /// <summary>После смены политики OG / BorderDrawMode — полный tray reload (corners + XAML).</summary>
    public static void NotifyTaskbarBorderPolicyNow() =>
        InvokeReloadImmediate(ReloadRenderFlags.TaskbarBoth);

    public static void NotifyTaskbarBackdrop() =>
        LiveReloadScheduler.ScheduleFlags(ReloadRenderFlags.TaskbarSwca, dwmNotify: false);

    public static void NotifyTaskbarBackdropNow() =>
        InvokeReloadImmediate(ReloadRenderFlags.TaskbarSwca);

    /// <summary>Live preview бордюра — при XAML только TaskbarXaml (без лишнего SWCA churn).</summary>
    public static void NotifyTaskbarBorderColorPreview()
    {
        var now = Environment.TickCount64;
        var last = Interlocked.Read(ref _lastBorderPreviewReloadTick);
        if (now - last < BorderPreviewThrottleMs)
            return;

        Interlocked.Exchange(ref _lastBorderPreviewReloadTick, now);
        InvokeReloadImmediate(GetTrayBorderColorReloadFlags());
    }

    /// <summary>Commit цвета бордюра — по BorderDrawMode (паритет NotifyFlyoutBorderLiveIfTray).</summary>
    public static void NotifyTaskbarBorderColorPreviewEnd()
    {
        Interlocked.Exchange(ref _lastBorderPreviewReloadTick, Environment.TickCount64);
        InvokeReloadImmediate(GetTrayBorderColorReloadFlags());
    }

    /// <summary>Debounced registry + reload для live preview gradient на Taskbar (бар).</summary>
    public static void ScheduleTaskbarGradientPreview(uint argb) =>
        ScheduleTaskbarGradientPreview(TrayColorRegSub(), argb);

    /// <summary>Debounced registry + reload для live preview gradient в указанный Tray subkey.</summary>
    public static void ScheduleTaskbarGradientPreview(string regSub, uint argb) =>
        LiveReloadScheduler.ScheduleFlyoutRegistryWrite(
            regSub,
            ThemeRegistry.GradientColorName(),
            argb,
            IsTrayBorderDrawModeXaml() ? ReloadRenderFlags.TaskbarBoth : ReloadRenderFlags.TaskbarSwca);

    /// <summary>Debounced registry + reload для live preview border на Taskbar (бар).</summary>
    public static void ScheduleTaskbarBorderColorPreview(uint argb) =>
        ScheduleTaskbarBorderColorPreview(TrayColorRegSub(), argb);

    /// <summary>Debounced registry + reload для live preview border в указанный Tray subkey.</summary>
    public static void ScheduleTaskbarBorderColorPreview(string regSub, uint argb) =>
        LiveReloadScheduler.ScheduleFlyoutRegistryWrite(
            regSub, ThemeRegistry.BorderColorName(), argb, GetTrayBorderColorReloadFlags());

    private static uint GetTrayBorderColorReloadFlags() =>
        IsTrayBorderDrawModeXaml()
            ? ReloadRenderFlags.TaskbarXaml
            : ReloadRenderFlags.TaskbarSwca;

    private static bool IsTrayBorderDrawModeXaml() =>
        GlassRegistry.ReadDwordFb(TrayRegSub, "BorderDrawMode", 0) != 0;

    private static string TrayColorRegSub() =>
        IsTrayBorderDrawModeXaml() ? "Tray\\Xaml" : TrayRegSub;

    private static void InvokeReloadImmediate(uint flags) =>
        ReloadRenderService.SignalReloadRender(flags);
}
