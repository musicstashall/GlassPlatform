using GlassSettings.Core.Registry;

namespace GlassSettings.Core.Platform;

/// <summary>
/// Root\NoBackdrop: ключ только со значением 1 (backdrop подавлен) или отсутствует (разрешён).
/// </summary>
public static class RootNoBackdropPolicy
{
    /// <summary>0 = backdrop разрешён; 1 = подавлен.</summary>
    public static uint ReadEffectiveNoBackdrop()
    {
        var existing = GlassRegistry.ReadDword("Root", "NoBackdrop");
        if (existing.HasValue)
            return existing.Value != 0 ? 1u : 0u;

        return 0u;
    }

    public static bool IsFullGlassEnabled => ReadEffectiveNoBackdrop() == 0;
}
