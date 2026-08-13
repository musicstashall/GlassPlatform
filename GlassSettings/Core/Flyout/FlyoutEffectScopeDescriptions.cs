using GlassSettings.Navigation;
using GlassSettings.Strings;

namespace GlassSettings.Core.Flyout;

public static class FlyoutEffectScopeDescriptions
{
    public static string ForPivotIndex(int index) =>
        FormatFromTargets("Flyout_EffectScopeTargets", index);

    public static string ForKind(FlyoutKind kind) => ForPivotIndex(kind switch
    {
        FlyoutKind.DropDown => 1,
        FlyoutKind.ToolTip => 2,
        _ => 0
    });

    public static string ForRootPivotIndex(int index) =>
        FormatFromTargets("Flyout_EffectScopeRootTargets", index);

    public static string ForTaskbar() =>
        GlassSettingsResources.Format(
            "Flyout_EffectScopeDesc",
            GlassSettingsResources.Get("Flyout_EffectScopeTaskbarTarget"));

    private static string FormatFromTargets(string targetsResourceKey, int index)
    {
        var targets = GlassSettingsResources.GetSemicolonList(targetsResourceKey);
        var targetIndex = Math.Clamp(index, 0, targets.Length - 1);
        return GlassSettingsResources.Format("Flyout_EffectScopeDesc", targets[targetIndex]);
    }
}
