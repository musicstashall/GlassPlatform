using GlassSettings.Core.Registry;
using GlassSettings.Services;
using Windows.UI;

namespace GlassSettings.Core.Dwm;

/// <summary>Режимы и BGR-формат ColorizationColorCaption* (паритет OpenGlassGUI).</summary>
public static class DwmCaptionColorHelper
{
    public const uint AutoSentinel = 0xFFFFFFFDu;
    public const uint ThemeSentinel = 0xFFFFFFFEu;
    public const uint SystemSentinel = 0xFFFFFFFFu;

    public const int ModeAuto = 0;
    public const int ModeFromTheme = 1;
    public const int ModeCustom = 2;
    public const int ModeSystem = 3;

    public static int ModeToComboIndex(uint value) => value switch
    {
        SystemSentinel => ModeSystem,
        ThemeSentinel => ModeFromTheme,
        AutoSentinel => ModeAuto,
        _ => IsCustomBgr(value) ? ModeCustom : ModeAuto
    };

    public static uint ComboIndexToDword(int index, uint customBgr) => index switch
    {
        ModeAuto => AutoSentinel,
        ModeFromTheme => ThemeSentinel,
        ModeSystem => SystemSentinel,
        ModeCustom => customBgr,
        _ => AutoSentinel
    };

    public static bool IsCustomMode(int comboIndex) => comboIndex == ModeCustom;

    public static bool IsCustomBgr(uint value) =>
        value != AutoSentinel && value != ThemeSentinel && value != SystemSentinel;

    /// <summary>BGR DWORD → ARGB для GlassAlphaColorPicker (alpha = 255).</summary>
    public static uint DwordBgrToPickerArgb(uint bgr)
    {
        var b = (byte)(bgr & 0xFF);
        var g = (byte)((bgr >> 8) & 0xFF);
        var r = (byte)((bgr >> 16) & 0xFF);
        return 0xFF000000u | ((uint)r << 16) | ((uint)g << 8) | b;
    }

    /// <summary>ARGB пикера → BGR DWORD (альфа игнорируется).</summary>
    public static uint PickerArgbToDwordBgr(uint argb)
    {
        var r = (byte)((argb >> 16) & 0xFF);
        var g = (byte)((argb >> 8) & 0xFF);
        var b = (byte)(argb & 0xFF);
        return (uint)r << 16 | (uint)g << 8 | b;
    }

    /// <summary>WinUI Color → DWM BGR (младшие 24 бита: B, G, R).</summary>
    public static uint WinUiColorToDwmBgrRgb(Color color) =>
        (uint)color.B | ((uint)color.G << 8) | ((uint)color.R << 16);

    /// <summary>COLORREF DWORD (R,G,B в младших байтах) → DWM BGR.</summary>
    public static uint ColorRefDwordToDwmBgr(uint colorRef)
    {
        var r = (byte)(colorRef & 0xFF);
        var g = (byte)((colorRef >> 8) & 0xFF);
        var b = (byte)((colorRef >> 16) & 0xFF);
        return (uint)b | ((uint)g << 8) | ((uint)r << 16);
    }

    /// <summary>DWM BGR + balance% → ARGB пикера.</summary>
    public static uint ComposePickerArgbFromDwmBgr(uint bgrRgb, uint percent)
    {
        var alpha = PercentToAlpha(percent);
        var rgb = DwordBgrToPickerArgb(bgrRgb) & 0x00FFFFFFu;
        return ((uint)alpha << 24) | rgb;
    }

    private static byte PercentToAlpha(uint percent) => (byte)((percent * 255u + 50u) / 100u);

    /// <summary>
    /// Системный accent для afterglow: DWM AccentColor (HKCU→HKLM), иначе ColorizationColor из .theme.
    /// AccentColor в реестре — COLORREF; colorization keys — BGR.
    /// </summary>
    public static bool TryReadSystemAccentBgr(DwmRegistryHive hive, out uint bgrRgb)
    {
        bgrRgb = 0;
        if (DwmRegistry.ReadDword(hive, "AccentColor") is { } accent)
        {
            bgrRgb = ColorRefDwordToDwmBgr(accent);
            return true;
        }

        if (hive == DwmRegistryHive.Hkcu
            && DwmRegistry.ReadDword(DwmRegistryHive.Hklm, "AccentColor") is { } machineAccent)
        {
            bgrRgb = ColorRefDwordToDwmBgr(machineAccent);
            return true;
        }

        var themeAccent = ThemePreviewChromeModeResolver.ResolveAccentColor();
        if (themeAccent is null)
            return false;

        bgrRgb = WinUiColorToDwmBgrRgb(themeAccent.Value);
        return true;
    }

    /// <summary>Эффективное значение с наследованием OpenGlass (absent → fallback).</summary>
    public static uint ResolveEffectiveValue(
        DwmRegistryHive hive,
        string key,
        uint inheritFrom)
    {
        if (DwmRegistryStore.ValueExistsForPage(hive, key))
            return DwmRegistryStore.ReadDwordForPage(hive, key) ?? inheritFrom;

        return inheritFrom;
    }

    public static uint ResolveCaptionActive(DwmRegistryHive hive) =>
        ResolveEffectiveValue(hive, "ColorizationColorCaption", AutoSentinel);

    public static uint ResolveCaptionInactive(DwmRegistryHive hive)
    {
        var active = ResolveCaptionActive(hive);
        return ResolveEffectiveValue(hive, "ColorizationColorCaptionInactive", active);
    }

    public static uint ResolveCaptionMaximized(DwmRegistryHive hive)
    {
        var active = ResolveCaptionActive(hive);
        return ResolveEffectiveValue(hive, "ColorizationColorCaptionMaximized", active);
    }

    public static uint ResolveCaptionInactiveMaximized(DwmRegistryHive hive)
    {
        var inactive = ResolveCaptionInactive(hive);
        return ResolveEffectiveValue(hive, "ColorizationColorCaptionInactiveMaximized", inactive);
    }

    /// <summary>Запись caption color: Auto → delete, иначе write.</summary>
    public static void CommitMode(
        DwmRegistryHive hive,
        string key,
        int comboIndex,
        uint customBgr)
    {
        if (comboIndex == ModeAuto)
        {
            SettingsSession.Current.DeleteDwmValue(hive, key, liveDwm: true);
            return;
        }

        var dword = ComboIndexToDword(comboIndex, customBgr);
        SettingsSession.Current.WriteDwmDword(hive, key, dword, liveDwm: true, dwmNotifyImmediate: true);
    }
}
