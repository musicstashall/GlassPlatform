using GlassSettings.Core.Registry;
using GlassSettings.Core.Settings;

namespace GlassSettings.Core.Flyout;

public static class FlyoutCornerRegistry
{
    public static readonly string[] ValueNames = ["CornerType", "EnableDropShadow"];

    public static bool IsAnyPresent(string regSub) =>
        GlassSectionGate.IsAnyPresent(regSub, ValueNames);

    public static void DeleteAll(string regSub) =>
        GlassSectionGate.DeleteAll(regSub, ValueNames);

    /// <summary>Gate ON: CornerType по умолчанию = крупное скругление (2), не «По умолчанию».</summary>
    public static void EnsureCornersEnabled(string regSub)
    {
        if (!GlassRegistry.ReadDword(regSub, "CornerType").HasValue)
            GlassRegistry.WriteDword(regSub, "CornerType", 2);
    }
}
