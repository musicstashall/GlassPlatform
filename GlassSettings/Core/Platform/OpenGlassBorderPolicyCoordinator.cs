using GlassSettings.Core.Customization;
using GlassSettings.Core.Flyout;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Snapshot;
using GlassSettings.Services;
using GlassSettings.Strings;

namespace GlassSettings.Core.Platform;

/// <summary>
/// Политика бордюров при OpenGlass pipeline (окно Dwm / snapshot).
/// </summary>
public static class OpenGlassBorderPolicyCoordinator
{
    private static bool _presenceHooked;
    private static bool _lastNoticeVisible;
    private static bool _lastPipelineAffectingBorders;

    public static event EventHandler? Changed;

    /// <summary>Подписка на snapshot платформы — обновление info-карточек без start/stop OG.</summary>
    public static void AttachPresenceListener()
    {
        if (_presenceHooked)
            return;

        _presenceHooked = true;
        _lastNoticeVisible = ShouldShowBorderColorLimitNotice();
        _lastPipelineAffectingBorders = IsOpenGlassAffectingBorders();
        PlatformPresenceService.Instance.PresenceChanged += OnPresenceChanged;
    }

    public static bool IsOpenGlassAffectingBorders()
    {
        var snapshot = PlatformPresenceService.Instance.CurrentSnapshot;
        if (snapshot.Revision != 0)
            return snapshot.OpenGlassPipelineActive != 0;

        return NativeHostInterop.IsOpenGlassBackdropPipelineActive();
    }

    public static bool ShouldShowBorderColorLimitNotice() =>
        WindowsOsHelper.IsWindows11OrGreater() && IsOpenGlassAffectingBorders();

    public static string FormatBorderLimitNoticeMessage(bool includeTaskbarSuffix)
    {
        var message = GlassSettingsResources.Get("Notice_OpenGlassBorderBody");
        if (includeTaskbarSuffix)
            message += "\n\n" + GlassSettingsResources.Get("Notice_OpenGlassBorderTraySuffix");
        return message;
    }

    public static void ApplyStartupPolicy() => ApplyPolicy();

    /// <summary>
    /// Только UI-уведомление (info-баннеры). Панель не трогаем —
    /// reload панели делает TaskbarCompat / явные команды FlyoutLiveReload.
    /// </summary>
    public static void ApplyPolicy()
    {
        Changed?.Invoke(null, EventArgs.Empty);
    }

    public static void NotifyOpenGlassStateChanged()
    {
        // Смена OG pipeline — явная команда reload панели (не навигация по страницам GS).
        if (IsOpenGlassAffectingBorders())
            FlyoutLiveReload.NotifyTaskbarBackdropNow();
        else
            FlyoutLiveReload.NotifyTaskbarBorderPolicyNow();

        Changed?.Invoke(null, EventArgs.Empty);
    }

    private static void OnPresenceChanged(object? sender, EventArgs e)
    {
        var nowNotice = ShouldShowBorderColorLimitNotice();
        var nowPipeline = IsOpenGlassAffectingBorders();
        var changed = nowNotice != _lastNoticeVisible || nowPipeline != _lastPipelineAffectingBorders;
        if (!changed)
            return;

        _lastNoticeVisible = nowNotice;
        _lastPipelineAffectingBorders = nowPipeline;
        Changed?.Invoke(null, EventArgs.Empty);
    }
}
