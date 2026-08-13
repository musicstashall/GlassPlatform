using Microsoft.Win32;

namespace GlassSettings.Core.Registry;

public static class DwmRegistry
{
    public static readonly string[] TrackedDwmValueNames =
    [
        "ColorizationColor", "ColorizationColorOverride", "ColorizationColorInactive",
        "ColorizationColorCaption", "ColorizationColorCaptionMaximized",
        "ColorizationColorCaptionInactive", "ColorizationColorCaptionInactiveMaximized",
        "GlassOpacity", "GlassOpacityInactive", "ColorizationOpacity",
        "ColorizationOpacityMaximized", "ColorizationOpacityInactive",
        "ColorizationOpacityInactiveMaximized", "ColorizationOpaqueBlend",
        "ColorizationOpaqueBlendPriority", "ColorizationGlassReflectionOpacity",
        "ColorizationGlassReflectionOpacityMaximized", "ColorizationGlassReflectionOpacityInactive",
        "ColorizationGlassReflectionOpacityInactiveMaximized", "GlassType", "GlassOverrideAccent",
        "BlurDeviation", "BlurOptimization", "RoundRectRadius", "MaterialOpacity",
        "ColorizationGlassReflectionIntensity", "ColorizationGlassReflectionParallaxIntensity",
        "ColorizationGlassReflectionPolicy", "CaptionButtons", "CenterCaption", "TextGlowMode",
        "DisableModernBorders", "UseDirect3DRendering", "CustomThemeReflection", "CustomThemeMaterial",
        "CustomThemeAtlas",
        "DisableGlassOnBattery", "GlassSafetyZoneMode", "DisabledHooks",
    ];

    public static bool ValueExists(string valueName) =>
        ValueExists(DwmRegistryHive.Hkcu, valueName);

    public static bool ValueExists(DwmRegistryHive hive, string valueName)
    {
        if (string.IsNullOrEmpty(valueName))
            return false;

        using var key = OpenSubKey(hive, writable: false);
        return key?.GetValue(valueName) is not null;
    }

    public static uint? ReadDword(string valueName) =>
        ReadDword(DwmRegistryHive.Hkcu, valueName);

    public static uint? ReadDword(DwmRegistryHive hive, string valueName)
    {
        if (string.IsNullOrEmpty(valueName))
            return null;

        using var key = OpenSubKey(hive, writable: false);
        if (key is null) return null;
        var raw = key.GetValue(valueName);
        return raw switch { int i => unchecked((uint)i), uint u => u, _ => null };
    }

    public static string? ReadString(string valueName) =>
        ReadString(DwmRegistryHive.Hkcu, valueName);

    public static string? ReadString(DwmRegistryHive hive, string valueName)
    {
        using var key = OpenSubKey(hive, writable: false);
        return key?.GetValue(valueName) as string;
    }

    public static bool WriteDword(string valueName, uint value) =>
        WriteDword(DwmRegistryHive.Hkcu, valueName, value);

    public static bool WriteDword(DwmRegistryHive hive, string valueName, uint value)
    {
        using var key = OpenSubKey(hive, writable: true) ?? CreateSubKey(hive);
        if (key is null)
            return false;

        key.SetValue(valueName, unchecked((int)value), RegistryValueKind.DWord);
        return true;
    }

    public static bool WriteString(string valueName, string value) =>
        WriteString(DwmRegistryHive.Hkcu, valueName, value);

    public static bool WriteString(DwmRegistryHive hive, string valueName, string value)
    {
        using var key = OpenSubKey(hive, writable: true) ?? CreateSubKey(hive);
        if (key is null)
            return false;

        key.SetValue(valueName, value, RegistryValueKind.String);
        return true;
    }

    public static void DeleteValue(string valueName) =>
        DeleteValue(DwmRegistryHive.Hkcu, valueName);

    public static void DeleteValue(DwmRegistryHive hive, string valueName)
    {
        using var key = OpenSubKey(hive, writable: true);
        key?.DeleteValue(valueName, false);
    }

    public static RegistryKey? OpenSubKey(DwmRegistryHive hive, bool writable) =>
        ResolveRootKey(hive).OpenSubKey(RegistryPaths.DwmRoot, writable);

    public static RegistryKey? CreateSubKey(DwmRegistryHive hive) =>
        ResolveRootKey(hive).CreateSubKey(RegistryPaths.DwmRoot);

    private static RegistryKey ResolveRootKey(DwmRegistryHive hive) =>
        hive == DwmRegistryHive.Hkcu
            ? global::Microsoft.Win32.Registry.CurrentUser
            : global::Microsoft.Win32.Registry.LocalMachine;
}
