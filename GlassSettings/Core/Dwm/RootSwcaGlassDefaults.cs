using GlassSettings.Core.Flyout;
using GlassSettings.Core.Registry;
using GlassSettings.Navigation;

namespace GlassSettings.Core.Dwm;

/// <summary>
/// SWCA gradient (Root/Popup BWM) — platform preset и gate-off запись в DWM.
/// </summary>
public static class RootSwcaGlassDefaults
{
    public const uint LightGradientArgb = 0x9EDDDDDDu;
    public const uint DarkGradientArgb = 0x412B2B2Bu;

    /// <summary>Канонический popup для platform DWM color (Menu fallthrough BWM).</summary>
    public const string CanonicalPopupRegSub = "Menu";

    public static uint DefaultGradientArgb() =>
        ThemeRegistry.IsDarkMode() ? DarkGradientArgb : LightGradientArgb;

    /// <summary>Effective SWCA gradient Popup — единый источник для DWM platform color и gate-off.</summary>
    public static uint ResolvePlatformGradientArgb() =>
        FlyoutThemeRegistry.ReadGradientArgbForUi(CanonicalPopupRegSub);

    /// <summary>ARGB gradient → BGR DWORD (паритет DwmSwcaColorBridge::SyncDwmFromSwcaGradient).</summary>
    public static uint GradientArgbToBgrRgb(uint argb)
    {
        var r = (argb >> 16) & 0xFF;
        var g = (argb >> 8) & 0xFF;
        var b = argb & 0xFF;
        return (uint)(b | (g << 8) | (r << 16));
    }

    public static uint GradientArgbToOpacityPercent(uint argb) =>
        ((argb >> 24) & 0xFF) * 100u / 255u;

    public static uint ComposePlatformColorDword(uint gradientArgb)
    {
        var alpha = (gradientArgb >> 24) & 0xFF;
        var bgr = GradientArgbToBgrRgb(gradientArgb);
        return (alpha << 24) | bgr;
    }

    public static uint ResolvePlatformColorDword() =>
        ComposePlatformColorDword(ResolvePlatformGradientArgb());

    public static bool MatchesAnyPlatformGradientBgr(uint bgrRgb)
    {
        var bgr = bgrRgb & 0x00FFFFFFu;
        var current = GradientArgbToBgrRgb(ResolvePlatformGradientArgb()) & 0x00FFFFFFu;
        // Другая ветка Menu из реестра (паритет OpenGlassDwmPolicy), плюс OG defaults.
        var otherBranchArgb = PartRegistryInterop.ReadUiGradientArgb(
            CanonicalPopupRegSub,
            !ThemeRegistry.IsDarkMode());
        var otherBranch = GradientArgbToBgrRgb(otherBranchArgb) & 0x00FFFFFFu;
        var light = GradientArgbToBgrRgb(LightGradientArgb) & 0x00FFFFFFu;
        var dark = GradientArgbToBgrRgb(DarkGradientArgb) & 0x00FFFFFFu;

        return bgr == current
               || bgr == otherBranch
               || bgr == light
               || bgr == dark;
    }

    public static (uint BgrRgb, uint OpacityPercent) ResolveActiveGateOffValues()
    {
        var gradient = ResolvePlatformGradientArgb();
        return (GradientArgbToBgrRgb(gradient), GradientArgbToOpacityPercent(gradient));
    }
}
