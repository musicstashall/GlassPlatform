using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;

namespace GlassSettings.Core.Flyout;

/// <summary>
/// UI-чтение flyout-параметров в раздельном scope: только локальные ключи раздела,
/// без fallthrough в общие настройки (корень BWM).
/// </summary>
public static class FlyoutSeparateScopeUi
{
    public const string GeneralRegSub = "";

    public static bool EffectUsesGeneral(string partRegSub) =>
        IsSeparatePartScope(partRegSub)
        && !IsStrictLocalEffectPresent(partRegSub)
        && IsStrictLocalEffectPresent(GeneralRegSub);

    public static bool GradientUsesGeneral(string partRegSub) =>
        IsSeparatePartScope(partRegSub)
        && !FlyoutThemeRegistry.IsGradientPresent(partRegSub)
        && FlyoutThemeRegistry.IsGradientPresent(GeneralRegSub);

    public static bool BorderUsesGeneral(string partRegSub) =>
        IsSeparatePartScope(partRegSub)
        && !FlyoutThemeRegistry.IsBorderPresent(partRegSub)
        && FlyoutThemeRegistry.IsBorderPresent(GeneralRegSub);

    public static bool CornersUseGeneral(string partRegSub) =>
        IsSeparatePartScope(partRegSub)
        && !FlyoutCornerRegistry.IsAnyPresent(partRegSub)
        && FlyoutCornerRegistry.IsAnyPresent(GeneralRegSub);

    public static bool AnimationUsesGeneral(string partRegSub) =>
        IsSeparatePartScope(partRegSub)
        && !FlyoutAnimRegistry.IsAnyPresent(partRegSub)
        && FlyoutAnimRegistry.IsAnyPresent(GeneralRegSub);

    public static bool IsStrictLocalEffectPresent(string partRegSub) =>
        GlassRegistry.ReadDword(partRegSub, FlyoutEffectRegistry.ValueName).HasValue;

    public static uint ReadEffectComboIndexForUi(string partRegSub)
    {
        if (GlassRegistry.ReadDword(partRegSub, FlyoutEffectRegistry.ValueName) is { } local)
            return local;

        return FlyoutEffectRegistry.DefaultFlyoutEffectType;
    }

    public static uint ReadGradientArgbForUi(string partRegSub)
    {
        var valueName = ThemeRegistry.GradientColorName();
        if (GlassRegistry.ReadDword(partRegSub, valueName) is { } local)
            return local;

        return FlyoutThemeRegistry.DefaultGradientArgb();
    }

    public static uint ReadBorderArgbForUi(string partRegSub)
    {
        var valueName = ThemeRegistry.BorderColorName();
        if (GlassRegistry.ReadDword(partRegSub, valueName) is { } local)
            return local;

        if (!FlyoutThemeRegistry.IsBorderPresent(GeneralRegSub))
            return FlyoutThemeRegistry.ReadBorderArgbForUi(partRegSub, valueName);

        return ReadFlyoutBorderSystemDefaultArgb(partRegSub, valueName);
    }

    public static bool ReadNoBorderColorForUi(string partRegSub) =>
        GlassRegistry.ReadDword(partRegSub, "NoBorderColor") is { } value && value != 0;

    public static uint ReadCornerTypeForUi(string partRegSub)
    {
        if (GlassRegistry.ReadDword(partRegSub, "CornerType") is { } local)
            return local;

        return DefaultCornerTypeForPart();
    }

    public static bool ReadDropShadowForUi(string partRegSub) =>
        GlassRegistry.ReadDword(partRegSub, "EnableDropShadow") is { } value && value != 0;

    public static uint ReadAnimForUi(string partRegSub, string valueName)
    {
        var local = GlassRegistry.ReadDword(FlyoutAnimRegistry.AnimSubKey(partRegSub), valueName);
        return local ?? DefaultAnimValue(valueName);
    }

    private static uint DefaultAnimValue(string valueName) => valueName switch
    {
        "PopInStyle" => FlyoutAnimRegistry.DefaultPopInStyle,
        "FadeOutTime" => FlyoutAnimRegistry.DefaultFadeOutTime,
        "PopInTime" => FlyoutAnimRegistry.DefaultPopInTime,
        "FadeInTime" => FlyoutAnimRegistry.DefaultFadeInTime,
        "StartRatio" => FlyoutAnimRegistry.DefaultStartRatio,
        "ImmediateInterrupting" => FlyoutAnimRegistry.DefaultImmediateInterrupting,
        _ => 0
    };

    private static bool IsSeparatePartScope(string partRegSub) =>
        !string.IsNullOrEmpty(partRegSub);

    private static uint DefaultCornerTypeForPart() =>
        WindowsOsHelper.IsWindows11OrGreater() ? 3u : 0u;

    /// <summary>
    /// Системный flyout-border без учёта общих border-color ключей (только probe-read).
    /// </summary>
    private static uint ReadFlyoutBorderSystemDefaultArgb(string partRegSub, string colorValueName)
    {
        var saved = SnapshotGeneralBorderColorKeys();
        try
        {
            DeleteGeneralBorderColorKeys();
            return FlyoutThemeRegistry.ReadBorderArgbForUi(partRegSub, colorValueName);
        }
        finally
        {
            RestoreGeneralBorderColorKeys(saved);
        }
    }

    private sealed class GeneralBorderColorSnapshot
    {
        public uint? DarkModeBorderColor { get; init; }
        public uint? LightModeBorderColor { get; init; }
    }

    private static GeneralBorderColorSnapshot SnapshotGeneralBorderColorKeys() => new()
    {
        DarkModeBorderColor = GlassRegistry.ReadDword(GeneralRegSub, "DarkMode_BorderColor"),
        LightModeBorderColor = GlassRegistry.ReadDword(GeneralRegSub, "LightMode_BorderColor")
    };

    private static void DeleteGeneralBorderColorKeys()
    {
        GlassRegistry.DeleteValue(GeneralRegSub, "DarkMode_BorderColor");
        GlassRegistry.DeleteValue(GeneralRegSub, "LightMode_BorderColor");
    }

    private static void RestoreGeneralBorderColorKeys(GeneralBorderColorSnapshot saved)
    {
        if (saved.DarkModeBorderColor is { } dark)
            GlassRegistry.WriteDword(GeneralRegSub, "DarkMode_BorderColor", dark);
        if (saved.LightModeBorderColor is { } light)
            GlassRegistry.WriteDword(GeneralRegSub, "LightMode_BorderColor", light);
    }
}
