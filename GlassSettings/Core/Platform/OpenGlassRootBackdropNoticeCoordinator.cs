using GlassSettings.Services;
using GlassSettings.Strings;

namespace GlassSettings.Core.Platform;

/// <summary>Баннер Root-окна при orchestration OpenGlass Running.</summary>
public static class OpenGlassRootBackdropNoticeCoordinator
{
    private static bool _presenceHooked;
    private static bool _lastRunning;

    public static event EventHandler? Changed;

    public static void AttachPresenceListener()
    {
        if (_presenceHooked)
            return;

        _presenceHooked = true;
        _lastRunning = IsOpenGlassOrchestrationRunning();
        PlatformPresenceService.Instance.PresenceChanged += OnPresenceChanged;
        OpenGlassUiPolicy.Changed += (_, _) => Changed?.Invoke(null, EventArgs.Empty);
    }

    public static bool IsOpenGlassOrchestrationRunning()
    {
        var snapshot = PlatformPresenceService.Instance.CurrentSnapshot;
        if (snapshot.Revision != 0)
        {
            return snapshot.OpenGlassOrchestration
                   == (int)NativeHostInterop.NativeOpenGlassHostUiState.Running;
        }

        return OpenGlassHostService.QueryOpenGlassHostUiState() == OpenGlassHostUiState.Running;
    }

    public static bool ShouldShowRootBackdropLimitNotice() =>
        RootBackdropModePolicy.IsFullGlassEnabled && IsOpenGlassOrchestrationRunning();

    public static string FormatActionHint() =>
        OpenGlassUiPolicy.IsOrchestratedExternally()
            ? GlassSettingsResources.Get("Notice_OgRootBackdrop_ActionExternal")
            : GlassSettingsResources.Get("Notice_OgRootBackdrop_ActionGlassBefore")
              + GlassSettingsResources.Get("Notice_OgRootBackdrop_ActionGlassLink")
              + GlassSettingsResources.Get("Notice_OgRootBackdrop_ActionGlassAfter");

    private static void OnPresenceChanged(object? sender, EventArgs e)
    {
        var now = IsOpenGlassOrchestrationRunning();
        if (now == _lastRunning)
            return;

        _lastRunning = now;
        Changed?.Invoke(null, EventArgs.Empty);
    }
}
