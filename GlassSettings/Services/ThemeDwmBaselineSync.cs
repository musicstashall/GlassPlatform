using GlassSettings.Core.Platform;

namespace GlassSettings.Services;

/// <summary>
/// Анти-гонка host (WM_THEMECHANGED → DWM policy) ↔ GS (CurrentTheme / UI):
/// suppress RegWatch → ждать стабильный fingerprint DWM → переснять baseline без жёлтого Save.
/// </summary>
public static class ThemeDwmBaselineSync
{
    private static readonly TimeSpan SuppressWindow = TimeSpan.FromSeconds(3);

    /// <summary>
    /// Пауза до settle после ThemeChanged при живом host:
    /// debounce policy 300 ms + запас на Phase1–3 (как MainWindow.ScheduleThemeBaselineReload).
    /// </summary>
    public const int HostPolicySettleDelayMs = 1000;

    /// <summary>Сразу при детектe смены темы — не трактовать промежуточные записи host как unsaved.</summary>
    public static void BeginThemeChangeWindow()
    {
        if (!ShouldSyncDwmBaseline())
            return;

        RegistrySnapshotWatchService.Current?.SuppressExternalDirty(SuppressWindow);
    }

    /// <summary>
    /// После host policy (или своей записи без host): settle fingerprint + CaptureScope DWM.
    /// </summary>
    public static async Task SettleAndReloadBaselineAsync()
    {
        if (!ShouldSyncDwmBaseline())
            return;

        if (SettingsSession.Current.IsShuttingDown)
            return;

        RegistrySnapshotWatchService.Current?.SuppressExternalDirty(SuppressWindow);
        await SettingsSession.Current.ReloadDwmSnapshotBaselineAsync().ConfigureAwait(true);
        // Save-кнопка: baseline = live DWM после settle (как Default accent на Colors).
        OpenGlassThemeSection.CaptureSaveBaselineFromLive();
    }

    /// <summary>GlassCompat host жив — он единственный writer DWM policy на внешней смене темы.</summary>
    public static bool IsGlassCompatHostWritingPolicy() =>
        GlassPlatformService.IsGlassPlatformRunning();

    public static bool ShouldApplyDwmPolicyFromGlassSettings() =>
        NativeHostInterop.ShouldApplyDwmCompositionPolicy()
        && !IsGlassCompatHostWritingPolicy();

    private static bool ShouldSyncDwmBaseline() =>
        OpenGlassUiPolicy.IsFeatureExposedInUi()
        || NativeHostInterop.ShouldApplyDwmCompositionPolicy();
}
