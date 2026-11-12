using GlassSettings.Services;

namespace GlassSettings.Core.Platform;

/// <summary>Видимость настроек любой обнаруженной установки OpenGlass.</summary>
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

    // Location is discovery information, not a reason to hide an installed OG.
    // Upstream Inno owns package installation; Settings controls the discovered
    // instance through its existing service/task APIs, regardless of directory.
    public static bool IsFeatureExposedInUi() =>
        PlatformPaths.OpenGlassDllPath is { Length: > 0 } dll && File.Exists(dll);

    private static void OnPresenceChanged(object? sender, EventArgs e)
    {
        // Снимок уже знает, есть ли задача OpenGlass. Повторный планировщик с UI
        // после сброса кэша на старте Glass держит прокрутку больше секунды.
        var now = IsFeatureExposedWithoutScheduler();
        if (now == _lastFeatureExposed)
            return;

        _lastFeatureExposed = now;
        Changed?.Invoke(null, EventArgs.Empty);
    }

    private static bool IsFeatureExposedWithoutScheduler() => IsFeatureExposedInUi();
}
