using GlassSettings.Core.Customization;
using GlassSettings.Core.Registry;
using GlassSettings.Services;

namespace GlassSettings.Core.Root;

/// <summary>Контрол Root-окна с настраиваемым удалением фона иконок (Root\Control\{Kind}).</summary>
public enum RootControlIconKind
{
    Ribbon,
    ListView,
    TreeView,
    Toolbar,
    Button,
    Static,
}

/// <summary>
/// Удаление фона иконок Ribbon / ListView / TreeView: Root\Control\{Kind}\ColorTreatAsTransparent*
/// (presence color = enable, как BWM Menu CONFIG). Без ключа runtime использует black-key.
/// </summary>
public static class RootControlIconRegistry
{
    public const string ControlScopeSubKey = "Root\\Control";

    public const string ColorTreatAsTransparentValueName = "ColorTreatAsTransparent";
    public const string ColorTreatAsTransparentThresholdValueName = "ColorTreatAsTransparentThreshold";

    public const uint DefaultIconColor = 0xFFFFFFFF;
    public const uint DefaultIconThreshold = 50;

    private static readonly string[] IconBackgroundValueNames =
    [
        ColorTreatAsTransparentValueName,
        ColorTreatAsTransparentThresholdValueName,
    ];

    public static string KindSegment(RootControlIconKind kind) => kind switch
    {
        RootControlIconKind.ListView => "ListView",
        RootControlIconKind.TreeView => "TreeView",
        RootControlIconKind.Toolbar => "Toolbar",
        RootControlIconKind.Button => "Button",
        RootControlIconKind.Static => "Static",
        _ => "Ribbon",
    };

    public static string RegSub(RootControlIconKind kind) =>
        $"{ControlScopeSubKey}\\{KindSegment(kind)}";

    public static RootControlIconKind KindFromPivotIndex(int index) => index switch
    {
        1 => RootControlIconKind.ListView,
        2 => RootControlIconKind.TreeView,
        3 => RootControlIconKind.Toolbar,
        4 => RootControlIconKind.Button,
        5 => RootControlIconKind.Static,
        _ => RootControlIconKind.Ribbon,
    };

    public static IReadOnlyList<RegistryParameterRef> GetParameterRefs(RootControlIconKind kind)
    {
        var sub = RegSub(kind);
        return IconBackgroundValueNames
            .Select(name => RegistryParameterRef.Glass(sub, name))
            .ToList();
    }

    public static IReadOnlyList<RegistryParameterRef> AllParameterRefs() =>
        Enum.GetValues<RootControlIconKind>()
            .SelectMany(GetParameterRefs)
            .ToList();

    public static bool TryKindFromRegSub(string regSub, out RootControlIconKind kind)
    {
        var normalized = regSub.Replace('/', '\\').Trim('\\');
        foreach (var candidate in Enum.GetValues<RootControlIconKind>())
        {
            if (normalized.Equals(RegSub(candidate), StringComparison.OrdinalIgnoreCase))
            {
                kind = candidate;
                return true;
            }
        }

        kind = RootControlIconKind.Ribbon;
        return false;
    }

    public static bool IsIconBackgroundPresent(RootControlIconKind kind) =>
        GlassRegistry.ReadDword(RegSub(kind), ColorTreatAsTransparentValueName) is not null;

    public static uint ReadEffectiveIconColor(RootControlIconKind kind) =>
        GlassRegistry.ReadDwordFb(RegSub(kind), ColorTreatAsTransparentValueName, DefaultIconColor);

    public static uint ReadEffectiveIconThreshold(RootControlIconKind kind) =>
        GlassRegistry.ReadDwordFb(RegSub(kind), ColorTreatAsTransparentThresholdValueName, DefaultIconThreshold);

    public static void WriteIconBackground(RootControlIconKind kind, uint argb, uint threshold)
    {
        var sub = RegSub(kind);
        SettingsSession.Current.WriteGlassDword(sub, ColorTreatAsTransparentValueName, argb, liveFlyout: false);
        if (threshold == DefaultIconThreshold)
            SettingsSession.Current.DeleteGlassValue(sub, ColorTreatAsTransparentThresholdValueName, liveFlyout: false);
        else
            SettingsSession.Current.WriteGlassDword(sub, ColorTreatAsTransparentThresholdValueName, threshold, liveFlyout: false);
    }

    public static void DeleteIconBackground(RootControlIconKind kind)
    {
        var sub = RegSub(kind);
        foreach (var name in IconBackgroundValueNames)
            SettingsSession.Current.DeleteGlassValue(sub, name, liveFlyout: false);
    }
}
