using GlassSettings.Core.Registry;
using GlassSettings.Core.Snapshot;
using GlassSettings.Services;

namespace GlassSettings.Core.Dwm;

/// <summary>Цвета DWM colorization — паритет OpenGlassGUI reset.</summary>
public static class DwmColorRegistry
{
    public const uint DefaultGlassColorArgb = 0xFF000000u;
    public const uint DefaultGlassOpacityPercent = 63u;
    public const uint DefaultColorBalancePercent = 10u;
    public const uint DefaultAfterglowBalancePercent = 10u;
    public const uint DefaultBlurBalancePercent = 50u;

    public static uint DefaultArgb(string regName) =>
        DwmDefaults.DefaultWhenKeyAbsent(regName);

    public static bool IsAtDefault(string regName, DwmRegistryHive hive = DwmRegistryHive.Hkcu)
    {
        var current = DwmRegistryStore.ReadDwordForPage(hive, regName);
        return !current.HasValue || current.Value == DefaultArgb(regName);
    }

    /// <summary>Кнопка «По умолчанию» disabled, если ключ есть и значение равно default.</summary>
    public static bool ShouldDisableDefaultButton(string regName, DwmRegistryHive hive = DwmRegistryHive.Hkcu)
    {
        var current = DwmRegistryStore.ReadDwordForPage(hive, regName);
        return current.HasValue && current.Value == DefaultArgb(regName);
    }

    public static uint? ReadGlassColor(DwmRegistryHive hive = DwmRegistryHive.Hkcu) =>
        DwmMirrorRegistry.ReadEffectiveDword(
            hive,
            DwmMirrorRegistry.GlassColorPrimary,
            DwmMirrorRegistry.GlassColorOverride);

    public static uint? ReadAfterglowColor(DwmRegistryHive hive = DwmRegistryHive.Hkcu) =>
        DwmMirrorRegistry.ReadEffectiveDword(
            hive,
            DwmMirrorRegistry.AfterglowPrimary,
            DwmMirrorRegistry.AfterglowOverride);

    public static void WriteGlassColor(DwmRegistryHive hive, uint argb, bool liveDwm = true) =>
        DwmMirrorRegistry.WriteColorPair(
            hive,
            DwmMirrorRegistry.GlassColorPrimary,
            DwmMirrorRegistry.GlassColorOverride,
            argb,
            liveDwm);

    public static void WriteAfterglowColor(DwmRegistryHive hive, uint argb, bool liveDwm = true) =>
        DwmMirrorRegistry.WriteColorPair(
            hive,
            DwmMirrorRegistry.AfterglowPrimary,
            DwmMirrorRegistry.AfterglowOverride,
            argb,
            liveDwm);

    public static void ResetGlassColor(DwmRegistryHive hive) =>
        DwmMirrorRegistry.DeletePair(
            hive,
            DwmMirrorRegistry.GlassColorPrimary,
            DwmMirrorRegistry.GlassColorOverride,
            liveDwm: true);

    public static void ResetAfterglowColor(DwmRegistryHive hive) =>
        DwmMirrorRegistry.DeletePair(
            hive,
            DwmMirrorRegistry.AfterglowPrimary,
            DwmMirrorRegistry.AfterglowOverride,
            liveDwm: true);

    /// <summary>OG reset (кнопка «По умолчанию» при gate ON): чёрный + platform balances + BlurDeviation если уже задан.</summary>
    public static void ApplyActiveColorDefaults(DwmRegistryHive hive, bool win7)
    {
        WriteGlassColor(hive, DefaultGlassColorArgb, liveDwm: true);
        WriteActiveColorBalanceDefaults(hive, win7, usePlatformGlassOpacity: false);
        ApplyBlurDeviationDefaultIfPresent(hive);
    }

    /// <summary>
    /// «По умолчанию» для радиуса: писать OG default (30) только если <c>BlurDeviation</c> уже есть.
    /// Absent не создаём.
    /// </summary>
    public static void ApplyBlurDeviationDefaultIfPresent(DwmRegistryHive hive)
    {
        if (!DwmRegistryStore.ValueExistsForPage(hive, "BlurDeviation"))
            return;

        SettingsSession.Current.WriteDwmDword(
            hive,
            "BlurDeviation",
            DwmDefaults.DefaultWhenKeyAbsent("BlurDeviation"),
            liveDwm: true,
            dwmNotifyImmediate: true);
    }

    /// <summary>Platform policy (включение gate): popup Menu gradient → Override + balances 10/50.</summary>
    public static void ApplyActiveColorPlatformDefaults(DwmRegistryHive hive, bool win7)
    {
        var platformDword = RootSwcaGlassDefaults.ResolvePlatformColorDword();
        DwmMirrorRegistry.WriteGlassColorOverrideDword(hive, platformDword, liveDwm: true);
        WriteActiveColorBalanceDefaults(hive, win7, usePlatformGlassOpacity: true);
    }

    private static void WriteActiveColorBalanceDefaults(DwmRegistryHive hive, bool win7, bool usePlatformGlassOpacity)
    {
        if (win7)
        {
            DwmMirrorRegistry.WriteBalancePair(
                hive,
                DwmMirrorRegistry.ColorBalancePrimary,
                DwmMirrorRegistry.ColorBalanceOverride,
                DefaultColorBalancePercent,
                liveDwm: true);
            DwmMirrorRegistry.WriteBalancePair(
                hive,
                DwmMirrorRegistry.BlurBalancePrimary,
                DwmMirrorRegistry.BlurBalanceOverride,
                DefaultBlurBalancePercent,
                liveDwm: true);
            return;
        }

        var opacity = usePlatformGlassOpacity
            ? RootSwcaGlassDefaults.GradientArgbToOpacityPercent(RootSwcaGlassDefaults.ResolvePlatformGradientArgb())
            : DefaultGlassOpacityPercent;
        SettingsSession.Current.WriteDwmDword(
            hive,
            "GlassOpacity",
            opacity,
            liveDwm: true,
            dwmNotifyImmediate: true);
    }

    public static bool IsActiveColorAtDefaults(DwmRegistryHive hive, bool win7)
    {
        var color = ReadGlassColor(hive);
        if (color is null || (color.Value & 0x00FFFFFFu) != (DefaultGlassColorArgb & 0x00FFFFFFu))
            return false;

        if (win7)
        {
            var balance = DwmMirrorRegistry.ReadEffectiveDword(
                hive,
                DwmMirrorRegistry.ColorBalancePrimary,
                DwmMirrorRegistry.ColorBalanceOverride);
            if (balance != DefaultColorBalancePercent)
                return false;

            var blur = DwmMirrorRegistry.ReadEffectiveDword(
                hive,
                DwmMirrorRegistry.BlurBalancePrimary,
                DwmMirrorRegistry.BlurBalanceOverride);
            return blur == DefaultBlurBalancePercent;
        }

        if (!VistaGlassColorPolicy.HasGlassOpacityKey(hive))
            return false;

        var glass = DwmRegistryStore.ReadDwordForPage(hive, "GlassOpacity") ?? DefaultGlassOpacityPercent;
        return glass == DefaultGlassOpacityPercent;
    }

    public static void ApplyInactiveSecondaryDefaults(DwmRegistryHive hive)
    {
        var dword = DwmMirrorRegistry.ToDwordPreserveAlpha(
            hive,
            "ColorizationColorInactive",
            DefaultGlassColorArgb);
        SettingsSession.Current.WriteDwmDword(
            hive,
            "ColorizationColorInactive",
            dword,
            liveDwm: true,
            dwmNotifyImmediate: true);
        SettingsSession.Current.WriteDwmDword(
            hive,
            "GlassOpacityInactive",
            DefaultGlassOpacityPercent,
            liveDwm: true,
            dwmNotifyImmediate: true);
    }

    public static bool IsInactiveSecondaryAtDefaults(DwmRegistryHive hive)
    {
        var color = DwmRegistryStore.ReadDwordForPage(hive, "ColorizationColorInactive");
        if (color is null)
            return false;

        var expected = DwmMirrorRegistry.ToDwordPreserveAlpha(
            hive,
            "ColorizationColorInactive",
            DefaultGlassColorArgb);
        if (color.Value != expected)
            return false;

        var opacity = DwmRegistryStore.ReadDwordForPage(hive, "GlassOpacityInactive") ?? DefaultGlassOpacityPercent;
        return opacity == DefaultGlassOpacityPercent;
    }

    public static void ApplyAfterglowSecondaryDefaults(DwmRegistryHive hive)
    {
        var accentBgr = DwmCaptionColorHelper.TryReadSystemAccentBgr(hive, out var bgr)
            ? bgr
            : 0u;
        var argb = DwmCaptionColorHelper.DwordBgrToPickerArgb(accentBgr);
        WriteAfterglowColor(hive, argb, liveDwm: true);
        DwmMirrorRegistry.WriteBalancePair(
            hive,
            DwmMirrorRegistry.AfterglowBalancePrimary,
            DwmMirrorRegistry.AfterglowBalanceOverride,
            DefaultAfterglowBalancePercent,
            liveDwm: true);
    }

    public static bool IsAfterglowSecondaryAtDefaults(DwmRegistryHive hive)
    {
        if (!DwmCaptionColorHelper.TryReadSystemAccentBgr(hive, out var accentBgr))
            return false;

        var color = ReadAfterglowColor(hive);
        if (color is null || (color.Value & 0x00FFFFFFu) != (accentBgr & 0x00FFFFFFu))
            return false;

        var balance = DwmMirrorRegistry.ReadEffectiveDword(
            hive,
            DwmMirrorRegistry.AfterglowBalancePrimary,
            DwmMirrorRegistry.AfterglowBalanceOverride);
        return balance == DefaultAfterglowBalancePercent;
    }

    public static void ResetToDefault(DwmRegistryHive hive, string regName)
    {
        if (string.Equals(regName, DwmMirrorRegistry.GlassColorPrimary, StringComparison.OrdinalIgnoreCase)
            || string.Equals(regName, DwmMirrorRegistry.GlassColorOverride, StringComparison.OrdinalIgnoreCase))
        {
            ResetGlassColor(hive);
            return;
        }

        if (string.Equals(regName, DwmMirrorRegistry.AfterglowPrimary, StringComparison.OrdinalIgnoreCase)
            || string.Equals(regName, DwmMirrorRegistry.AfterglowOverride, StringComparison.OrdinalIgnoreCase))
        {
            ResetAfterglowColor(hive);
            return;
        }

        SettingsSession.Current.DeleteDwmValue(hive, regName, liveDwm: true);
    }
}
