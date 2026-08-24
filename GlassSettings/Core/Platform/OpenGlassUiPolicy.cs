using GlassSettings.Services;

namespace GlassSettings.Core.Platform;

/// <summary>Видимость настроек OpenGlass в GlassSettings (bundled + владелец оркестрации).</summary>
public static class OpenGlassUiPolicy
{
    private static bool _presenceHooked;
    private static bool _lastFeatureExposed;

    public static event EventHandler? Changed;

    public static void AttachPresenceListener()
    {
        if (_presenceHooked)
            return;

        _presenceHooked = true;
        _lastFeatureExposed = IsFeatureExposedInUi();
        PlatformPresenceService.Instance.PresenceChanged += OnPresenceChanged;
    }

    public static bool IsBundledWithApplication() => OpenGlassPathPolicy.IsBundledWithApplication();

    public static bool IsOrchestratedExternally()
    {
        if (!NativeHostInterop.OpenGlassOrchestrationInstalled())
            return false;

        return !OrchestrationTargetsBundledPackage();
    }

    public static bool IsFeatureExposedInUi() =>
        IsBundledWithApplication() && !IsOrchestratedExternally();

    public static bool OrchestrationTargetsBundledPackage()
    {
        if (!IsBundledWithApplication())
            return false;

        var dll = PlatformPaths.OpenGlassDllPath;
        if (OpenGlassPathPolicy.IsPathBundled(dll))
            return true;

        var host = PlatformPaths.OpenGlassHostPath;
        if (OpenGlassPathPolicy.IsPathBundled(host))
            return true;

        if (NativeHostInterop.TryGetOpenGlassServiceBinPath(out var serviceBin)
            && OpenGlassPathPolicy.IsPathBundled(serviceBin))
        {
            return true;
        }

        return false;
    }

    private static void OnPresenceChanged(object? sender, EventArgs e)
    {
        var now = IsFeatureExposedInUi();
        if (now == _lastFeatureExposed)
            return;

        _lastFeatureExposed = now;
        Changed?.Invoke(null, EventArgs.Empty);
    }
}
