using GlassSettings.Core.Platform;
using GlassSettings.Services;

namespace GlassSettings.Core.Platform;

/// <summary>RootBackdropMode для live-reload (snapshot платформы + локальный fallback).</summary>
public static class RootBackdropModePolicy
{
    public const uint ModeOff = 0;
    public const uint ModeDwm = 1;
    public const uint ModeSwca = 2;

    public static bool IsFullGlassEnabled => RootNoBackdropPolicy.IsFullGlassEnabled;

    public static bool IsSwcaBackdropActive()
    {
        if (!IsFullGlassEnabled)
            return false;

        var snapshot = PlatformPresenceService.Instance.CurrentSnapshot;
        if (snapshot.Revision != 0)
            return snapshot.RootBackdropMode == ModeSwca;

        if (NativeHostInterop.IsOpenGlassBackdropPipelineActive())
            return false;

        return true;
    }
}
