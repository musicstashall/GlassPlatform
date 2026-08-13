using GlassSettings.Core.Customization;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Settings;
using GlassSettings.Services;

namespace GlassSettings.Core.Menu;

/// <summary>Параметры Hot / DisabledHot / Focusing / Separator под BWM\Menu\{Kind} (RegHelper: { Kind, Menu }).</summary>
public static class MenuItemRegistry
{
    public const string MenuScopeSubKey = "Menu";

    public const string DisabledValueName = "Disabled";
    public const string CornerRadiusValueName = "CornerRadius";
    public const string EnableThemeColorizationValueName = "EnableThemeColorization";
    public const string WidthValueName = "Width";
    public const string DarkModeColorValueName = "DarkMode_Color";
    public const string LightModeColorValueName = "LightMode_Color";
    public const string DarkModeThemeColorizationTypeValueName = "DarkMode_ThemeColorizationType";
    public const string LightModeThemeColorizationTypeValueName = "LightMode_ThemeColorizationType";

    public const string DefaultThemeColorizationType = "ImmersiveStartHoverBackground";

    private static readonly string[] BaseValueNames =
    [
        DisabledValueName,
        CornerRadiusValueName,
        EnableThemeColorizationValueName,
        DarkModeColorValueName,
        LightModeColorValueName,
    ];

    private static readonly string[] ThemeColorizationTypeValueNames =
    [
        DarkModeThemeColorizationTypeValueName,
        LightModeThemeColorizationTypeValueName,
    ];

    private static readonly string[] WidthValueNames = [WidthValueName];

    public static string RegSub(MenuItemKind kind) =>
        $"{MenuScopeSubKey}\\{KindSegment(kind)}";

    public static string KindSegment(MenuItemKind kind) => kind switch
    {
        MenuItemKind.DisabledHot => "DisabledHot",
        MenuItemKind.Focusing => "Focusing",
        MenuItemKind.Separator => "Separator",
        _ => "Hot",
    };

    public static MenuItemKind KindFromPivotIndex(int index) => index switch
    {
        1 => MenuItemKind.DisabledHot,
        2 => MenuItemKind.Focusing,
        3 => MenuItemKind.Separator,
        _ => MenuItemKind.Hot,
    };

    public static bool HasWidth(MenuItemKind kind) =>
        kind is MenuItemKind.Focusing or MenuItemKind.Separator;

    public static IReadOnlyList<string> AllValueNames(MenuItemKind kind)
    {
        if (HasWidth(kind))
            return [..BaseValueNames, ..WidthValueNames];
        return BaseValueNames;
    }

    public static IReadOnlyList<RegistryParameterRef> GetParameterRefs(MenuItemKind kind)
    {
        var sub = RegSub(kind);
        var list = AllValueNames(kind)
            .Select(name => RegistryParameterRef.Glass(sub, name))
            .ToList();
        list.AddRange(ThemeColorizationTypeValueNames
            .Select(name => RegistryParameterRef.Glass(sub, name)));
        return list;
    }

    public static IReadOnlyList<RegistryParameterRef> AllPopupParameterRefs() =>
        Enum.GetValues<MenuItemKind>()
            .SelectMany(GetParameterRefs)
            .ToList();

    public static bool IsMenuItemSubKey(string relativeSubKey)
    {
        var normalized = relativeSubKey.Replace('/', '\\').Trim('\\');
        if (!normalized.StartsWith($"{MenuScopeSubKey}\\", StringComparison.OrdinalIgnoreCase))
            return false;

        var segment = normalized.Split('\\');
        return segment.Length >= 2 && TryKindFromKindSegment(segment[1], out _);
    }

    public static bool TryKindFromRegSub(string regSub, out MenuItemKind kind)
    {
        var normalized = regSub.Replace('/', '\\').Trim('\\');
        if (!normalized.StartsWith($"{MenuScopeSubKey}\\", StringComparison.OrdinalIgnoreCase))
        {
            kind = MenuItemKind.Hot;
            return false;
        }

        var segment = normalized.Split('\\');
        if (segment.Length < 2)
        {
            kind = MenuItemKind.Hot;
            return false;
        }

        return TryKindFromKindSegment(segment[1], out kind);
    }

    private static bool TryKindFromKindSegment(string segment, out MenuItemKind kind)
    {
        if (segment.Equals("DisabledHot", StringComparison.OrdinalIgnoreCase))
        {
            kind = MenuItemKind.DisabledHot;
            return true;
        }

        if (segment.Equals("Focusing", StringComparison.OrdinalIgnoreCase))
        {
            kind = MenuItemKind.Focusing;
            return true;
        }

        if (segment.Equals("Separator", StringComparison.OrdinalIgnoreCase))
        {
            kind = MenuItemKind.Separator;
            return true;
        }

        if (segment.Equals("Hot", StringComparison.OrdinalIgnoreCase))
        {
            kind = MenuItemKind.Hot;
            return true;
        }

        kind = MenuItemKind.Hot;
        return false;
    }

    public static bool IsAnyPresent(MenuItemKind kind)
    {
        var sub = RegSub(kind);
        if (GlassSectionGate.IsAnyPresent(sub, AllValueNames(kind)))
            return true;

        foreach (var name in ThemeColorizationTypeValueNames)
        {
            if (GlassRegistry.ReadString(sub, name) is not null)
                return true;
        }

        return false;
    }

    /// <summary>Удалить подключ Menu\{Kind} целиком (values + REG_SZ).</summary>
    public static void DeleteKindSubKey(MenuItemKind kind) =>
        GlassRegistry.DeleteSubKeyTree(RegSub(kind));

    public static void DeleteAll(MenuItemKind kind) =>
        DeleteKindSubKey(kind);

    public static void DeleteAll(MenuItemKind kind, bool liveFlyout)
    {
        if (!liveFlyout)
        {
            DeleteAll(kind);
            return;
        }

        var sub = RegSub(kind);
        foreach (var name in AllValueNames(kind))
            SettingsSession.Current.DeleteGlassValue(sub, name, liveFlyout: true);
        foreach (var name in ThemeColorizationTypeValueNames)
            SettingsSession.Current.DeleteGlassValue(sub, name, liveFlyout: true);
        DeleteKindSubKey(kind);
    }

    public static uint ReadEffectiveDword(MenuItemKind kind, string valueName) =>
        GlassRegistry.ReadDwordFb(RegSub(kind), valueName, DefaultDword(kind, valueName));

    public static uint ReadEffectiveColor(MenuItemKind kind, bool darkMode) =>
        ReadEffectiveDword(kind, darkMode ? DarkModeColorValueName : LightModeColorValueName);

    public static void WriteValue(MenuItemKind kind, string valueName, uint value) =>
        GlassRegistry.WriteDword(RegSub(kind), valueName, value);

    public static void WriteAllDefaults(MenuItemKind kind)
    {
        var sub = RegSub(kind);
        foreach (var name in AllValueNames(kind))
        {
            var def = DefaultDword(kind, name);
            if (ShouldDeleteInsteadOfWrite(name, def))
                GlassRegistry.DeleteValue(sub, name);
            else
                GlassRegistry.WriteDword(sub, name, def);
        }
    }

    public static void DeleteValue(MenuItemKind kind, string valueName) =>
        GlassRegistry.DeleteValue(RegSub(kind), valueName);

    public static string ReadEffectiveThemeColorizationType(MenuItemKind kind, bool darkMode)
    {
        var name = darkMode
            ? DarkModeThemeColorizationTypeValueName
            : LightModeThemeColorizationTypeValueName;
        return GlassRegistry.ReadString(RegSub(kind), name) ?? DefaultThemeColorizationType;
    }

    public static void WriteThemeColorizationType(MenuItemKind kind, bool darkMode, string value)
    {
        var name = darkMode
            ? DarkModeThemeColorizationTypeValueName
            : LightModeThemeColorizationTypeValueName;
        if (string.Equals(value, DefaultThemeColorizationType, StringComparison.Ordinal))
            GlassRegistry.DeleteValue(RegSub(kind), name);
        else
            GlassRegistry.WriteString(RegSub(kind), name, value);
    }

    public static bool ShouldDeleteInsteadOfWrite(string valueName, uint value) =>
        valueName == DisabledValueName && value == 0;

    public static bool IsAtDefaultState(MenuItemKind kind)
    {
        foreach (var name in AllValueNames(kind))
        {
            if (ReadEffectiveDword(kind, name) != DefaultDword(kind, name))
                return false;
        }

        return true;
    }

    public static bool ShouldDisableDefaultButton(MenuItemKind kind) => !IsAnyPresent(kind);

    public static uint DefaultDword(MenuItemKind kind, string valueName) => valueName switch
    {
        DisabledValueName => 0,
        CornerRadiusValueName => DefaultCornerRadius(kind),
        EnableThemeColorizationValueName => 0,
        WidthValueName => 1000,
        DarkModeColorValueName => DefaultDarkColor(kind),
        LightModeColorValueName => DefaultLightColor(kind),
        _ => 0,
    };

    private static uint DefaultCornerRadius(MenuItemKind kind) => 8u;

    private static uint DefaultDarkColor(MenuItemKind kind) => kind switch
    {
        MenuItemKind.Hot => 0x41808080,
        MenuItemKind.DisabledHot => 0x00000000,
        MenuItemKind.Focusing => 0xFFFFFFFF,
        MenuItemKind.Separator => 0x30D9D9D9,
        _ => 0,
    };

    private static uint DefaultLightColor(MenuItemKind kind) => kind switch
    {
        MenuItemKind.Hot => 0x30000000,
        MenuItemKind.DisabledHot => 0x00000000,
        MenuItemKind.Focusing => 0xFF000000,
        MenuItemKind.Separator => 0x30262626,
        _ => 0,
    };
}
