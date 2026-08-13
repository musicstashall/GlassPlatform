using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Snapshot;
using GlassSettings.Services;

namespace GlassSettings.Core.Dwm;

/// <summary>
/// Win7 Aero: factual absent (UI/превью) vs OG gate-off preview (Override absent).
/// </summary>
public static class Win7GlassColorPolicy
{
    public const uint FactualAbsentColorRgb = 0;
    public const uint FactualAbsentBalancePercent = 0;

    /// <summary>Gate-off запись: color balance (только при gate OFF, не для UI absent).</summary>
    public const uint GateOffActiveColorBalancePercent = 35;

    /// <summary>Gate-off запись: afterglow balance (только при gate OFF, не для UI absent).</summary>
    public const uint GateOffAfterglowBalancePercent = 20;

    /// <summary>Gate-off запись: blur balance active (только при gate OFF, не для UI absent).</summary>
    public const uint GateOffBlurBalancePercent = 50;

    public static bool HasBlurBalanceKey(DwmRegistryHive hive) =>
        DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.BlurBalanceOverride)
        || DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.BlurBalancePrimary);

    public static bool HasActiveOverrideKey(DwmRegistryHive hive) =>
        DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.GlassColorOverride)
        || DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.ColorBalanceOverride)
        || DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.BlurBalanceOverride);

    public static bool HasActiveColorKey(DwmRegistryHive hive) =>
        DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.GlassColorOverride)
        || DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.GlassColorPrimary);

    public static bool HasAfterglowOverrideKey(DwmRegistryHive hive) =>
        DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.AfterglowOverride)
        || DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.AfterglowBalanceOverride);

    public static bool HasAfterglowColorKey(DwmRegistryHive hive) =>
        DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.AfterglowOverride)
        || DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.AfterglowPrimary);

    public static bool HasColorBalanceKey(DwmRegistryHive hive) =>
        DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.ColorBalanceOverride)
        || DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.ColorBalancePrimary);

    public static bool HasAfterglowBalanceKey(DwmRegistryHive hive) =>
        DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.AfterglowBalanceOverride)
        || DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.AfterglowBalancePrimary);

    public static uint? TryReadActiveColorRgb(DwmRegistryHive hive) =>
        DwmMirrorRegistry.ReadEffectiveDword(
            hive,
            DwmMirrorRegistry.GlassColorPrimary,
            DwmMirrorRegistry.GlassColorOverride);

    public static uint? TryReadAfterglowColorRgb(DwmRegistryHive hive) =>
        DwmMirrorRegistry.ReadEffectiveDword(
            hive,
            DwmMirrorRegistry.AfterglowPrimary,
            DwmMirrorRegistry.AfterglowOverride);

    /// <summary>UI/превью: absent → 0, не gate-off дефолты.</summary>
    public static uint ReadFactualActiveColorRgb(DwmRegistryHive hive) =>
        TryReadActiveColorRgb(hive) ?? FactualAbsentColorRgb;

    public static uint ReadFactualAfterglowColorRgb(DwmRegistryHive hive)
    {
        var raw = TryReadAfterglowColorRgb(hive) ?? FactualAbsentColorRgb;
        return NormalizeAfterglowBgrIfStoredAsColorRef(hive, raw);
    }

    /// <summary>
    /// Heal: native policy мог записать AccentColor (COLORREF) в BGR-ключ без swap.
    /// Не трогаем уже нормализованный BGR (защита от двойной конвертации).
    /// </summary>
    private static uint NormalizeAfterglowBgrIfStoredAsColorRef(DwmRegistryHive hive, uint bgrRgb)
    {
        uint? accentRaw = DwmRegistry.ReadDword(hive, "AccentColor");
        if (accentRaw is null && hive == DwmRegistryHive.Hkcu)
            accentRaw = DwmRegistry.ReadDword(DwmRegistryHive.Hklm, "AccentColor");
        if (accentRaw is not { } accent)
            return bgrRgb;

        var storedRgb = bgrRgb & 0x00FFFFFFu;
        var accentRgb = accent & 0x00FFFFFFu;
        if (storedRgb != accentRgb)
            return bgrRgb;

        var normalized = DwmCaptionColorHelper.ColorRefDwordToDwmBgr(accent) & 0x00FFFFFFu;
        return normalized != storedRgb ? normalized : bgrRgb;
    }

    public static uint ReadFactualActiveBalancePercent(DwmRegistryHive hive)
    {
        if (!HasColorBalanceKey(hive))
            return FactualAbsentBalancePercent;

        var value = DwmMirrorRegistry.ReadEffectiveDword(
            hive,
            DwmMirrorRegistry.ColorBalancePrimary,
            DwmMirrorRegistry.ColorBalanceOverride) ?? FactualAbsentBalancePercent;
        return Math.Min(value, 100u);
    }

    public static uint ReadFactualAfterglowBalancePercent(DwmRegistryHive hive)
    {
        if (!HasAfterglowBalanceKey(hive))
            return FactualAbsentBalancePercent;

        var value = DwmMirrorRegistry.ReadEffectiveDword(
            hive,
            DwmMirrorRegistry.AfterglowBalancePrimary,
            DwmMirrorRegistry.AfterglowBalanceOverride) ?? FactualAbsentBalancePercent;
        return Math.Min(value, 100u);
    }

    public static uint ReadFactualBlurBalancePercent(DwmRegistryHive hive)
    {
        if (!HasBlurBalanceKey(hive))
            return FactualAbsentBalancePercent;

        var value = DwmMirrorRegistry.ReadEffectiveDword(
            hive,
            DwmMirrorRegistry.BlurBalancePrimary,
            DwmMirrorRegistry.BlurBalanceOverride) ?? FactualAbsentBalancePercent;
        return Math.Min(value, 100u);
    }

    private static bool HasRuntimeMirrorPair(string primary, string overrideKey) =>
        DwmRegistryStore.ExistsInAnyHive(overrideKey) || DwmRegistryStore.ExistsInAnyHive(primary);

    public static bool HasRuntimeBlurBalanceKey() =>
        HasRuntimeMirrorPair(DwmMirrorRegistry.BlurBalancePrimary, DwmMirrorRegistry.BlurBalanceOverride);

    public static bool HasRuntimeColorBalanceKey() =>
        HasRuntimeMirrorPair(DwmMirrorRegistry.ColorBalancePrimary, DwmMirrorRegistry.ColorBalanceOverride);

    public static bool HasRuntimeAfterglowBalanceKey() =>
        HasRuntimeMirrorPair(
            DwmMirrorRegistry.AfterglowBalancePrimary,
            DwmMirrorRegistry.AfterglowBalanceOverride);

    public static bool HasRuntimeAfterglowOverrideKey() =>
        DwmRegistryStore.ExistsInAnyHive(DwmMirrorRegistry.AfterglowOverride)
        || DwmRegistryStore.ExistsInAnyHive(DwmMirrorRegistry.AfterglowBalanceOverride);

    public static uint? TryReadRuntimeActiveColorRgb() =>
        DwmMirrorRegistry.ReadRuntimeEffectiveDword(
            DwmMirrorRegistry.GlassColorPrimary,
            DwmMirrorRegistry.GlassColorOverride);

    public static uint? TryReadRuntimeAfterglowColorRgb() =>
        DwmMirrorRegistry.ReadRuntimeEffectiveDword(
            DwmMirrorRegistry.AfterglowPrimary,
            DwmMirrorRegistry.AfterglowOverride);

    public static uint ReadRuntimeFactualActiveColorRgb() =>
        TryReadRuntimeActiveColorRgb() ?? FactualAbsentColorRgb;

    public static uint ReadRuntimeFactualAfterglowColorRgb()
    {
        var raw = TryReadRuntimeAfterglowColorRgb() ?? FactualAbsentColorRgb;
        return NormalizeAfterglowBgrIfStoredAsColorRefRuntime(raw);
    }

    public static uint ReadRuntimeFactualActiveBalancePercent()
    {
        if (!HasRuntimeColorBalanceKey())
            return FactualAbsentBalancePercent;

        var value = DwmMirrorRegistry.ReadRuntimeEffectiveDword(
            DwmMirrorRegistry.ColorBalancePrimary,
            DwmMirrorRegistry.ColorBalanceOverride) ?? FactualAbsentBalancePercent;
        return Math.Min(value, 100u);
    }

    public static uint ReadRuntimeFactualAfterglowBalancePercent()
    {
        if (!HasRuntimeAfterglowBalanceKey())
            return FactualAbsentBalancePercent;

        var value = DwmMirrorRegistry.ReadRuntimeEffectiveDword(
            DwmMirrorRegistry.AfterglowBalancePrimary,
            DwmMirrorRegistry.AfterglowBalanceOverride) ?? FactualAbsentBalancePercent;
        return Math.Min(value, 100u);
    }

    public static uint ReadRuntimeFactualBlurBalancePercent()
    {
        if (!HasRuntimeBlurBalanceKey())
            return FactualAbsentBalancePercent;

        var value = DwmMirrorRegistry.ReadRuntimeEffectiveDword(
            DwmMirrorRegistry.BlurBalancePrimary,
            DwmMirrorRegistry.BlurBalanceOverride) ?? FactualAbsentBalancePercent;
        return Math.Min(value, 100u);
    }

    /// <summary>Эффективное свечение для превью OpenGlass: HKCU → HKLM.</summary>
    public static (uint BgrRgb, uint BalancePercent) ResolveRuntimePreviewAfterglow()
    {
        if (HasRuntimeAfterglowOverrideKey())
        {
            return (
                ReadRuntimeFactualAfterglowColorRgb(),
                Math.Min(ReadRuntimeFactualAfterglowBalancePercent(), 100u));
        }

        return ResolveOgAfterglowPreview(DwmRegistryHive.Hkcu);
    }

    private static uint NormalizeAfterglowBgrIfStoredAsColorRefRuntime(uint bgrRgb)
    {
        if (DwmRegistryStore.ReadEffectiveDword("AccentColor") is not { } accent)
            return bgrRgb;

        var storedRgb = bgrRgb & 0x00FFFFFFu;
        var accentRgb = accent & 0x00FFFFFFu;
        if (storedRgb != accentRgb)
            return bgrRgb;

        var normalized = DwmCaptionColorHelper.ColorRefDwordToDwmBgr(accent) & 0x00FFFFFFu;
        return normalized != storedRgb ? normalized : bgrRgb;
    }

    /// <summary>UI слайдер: OG default если ключ absent (GetDword key, default).</summary>
    public static uint ReadUiBlurBalancePercent(DwmRegistryHive hive) =>
        HasBlurBalanceKey(hive)
            ? ReadFactualBlurBalancePercent(hive)
            : DwmColorRegistry.DefaultBlurBalancePercent;

    /// <summary>UI picker α: OG default если ключ absent.</summary>
    public static uint ReadUiActiveBalancePercent(DwmRegistryHive hive) =>
        HasColorBalanceKey(hive)
            ? ReadFactualActiveBalancePercent(hive)
            : DwmColorRegistry.DefaultColorBalancePercent;

    /// <summary>UI picker α afterglow: OG default если ключ absent.</summary>
    public static uint ReadUiAfterglowBalancePercent(DwmRegistryHive hive) =>
        HasAfterglowBalanceKey(hive)
            ? ReadFactualAfterglowBalancePercent(hive)
            : DwmColorRegistry.DefaultAfterglowBalancePercent;

    public static uint ResolveGateOffActiveColorRgb() =>
        RootSwcaGlassDefaults.GradientArgbToBgrRgb(RootSwcaGlassDefaults.ResolvePlatformGradientArgb());

    public static uint ResolveGateOffAfterglowColorRgb(DwmRegistryHive hive) =>
        DwmCaptionColorHelper.TryReadSystemAccentBgr(hive, out var bgr)
            ? bgr
            : FactualAbsentColorRgb;

    /// <summary>OG-эффективное свечение для превью при gate OFF (Override absent).</summary>
    public static (uint BgrRgb, uint BalancePercent) ResolveOgAfterglowPreview(DwmRegistryHive hive) =>
        (ResolveGateOffAfterglowColorRgb(hive), GateOffAfterglowBalancePercent);

    /// <summary>Эффективное свечение для превью: Override → factual, иначе OG gate-off.</summary>
    public static (uint BgrRgb, uint BalancePercent) ResolvePreviewAfterglow(DwmRegistryHive hive)
    {
        if (HasAfterglowOverrideKey(hive))
        {
            return (
                ReadFactualAfterglowColorRgb(hive),
                Math.Min(ReadFactualAfterglowBalancePercent(hive), 100u));
        }

        return ResolveOgAfterglowPreview(hive);
    }

    /// <summary>Gate OFF: удалить Override active color + BlurDeviation (OG рендерит без кастома).</summary>
    public static void ClearActiveOverrides(DwmRegistryHive hive)
    {
        var changed = false;
        if (DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.GlassColorOverride))
        {
            SettingsSession.Current.DeleteDwmValue(hive, DwmMirrorRegistry.GlassColorOverride, liveDwm: false);
            changed = true;
        }

        if (DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.ColorBalanceOverride))
        {
            SettingsSession.Current.DeleteDwmValue(hive, DwmMirrorRegistry.ColorBalanceOverride, liveDwm: false);
            changed = true;
        }

        if (DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.BlurBalanceOverride))
        {
            SettingsSession.Current.DeleteDwmValue(hive, DwmMirrorRegistry.BlurBalanceOverride, liveDwm: false);
            changed = true;
        }

        if (DwmRegistryStore.ValueExistsForPage(hive, "BlurDeviation"))
        {
            SettingsSession.Current.DeleteDwmValue(hive, "BlurDeviation", liveDwm: false);
            changed = true;
        }

        if (changed)
            LiveReloadScheduler.NotifyDwmColorizationNow();
    }

    /// <summary>Gate OFF: удалить Override afterglow (OG рендерит accent/balance 20 без кастома).</summary>
    public static void ClearAfterglowOverrides(DwmRegistryHive hive)
    {
        var changed = false;
        if (DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.AfterglowOverride))
        {
            SettingsSession.Current.DeleteDwmValue(hive, DwmMirrorRegistry.AfterglowOverride, liveDwm: false);
            changed = true;
        }

        if (DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.AfterglowBalanceOverride))
        {
            SettingsSession.Current.DeleteDwmValue(hive, DwmMirrorRegistry.AfterglowBalanceOverride, liveDwm: false);
            changed = true;
        }

        if (changed)
            LiveReloadScheduler.NotifyDwmColorizationNow();
    }

    public static uint ToPickerArgbFromBgr(uint bgrRgb, uint balancePercent)
    {
        var argb = DwmCaptionColorHelper.DwordBgrToPickerArgb(bgrRgb);
        var alpha = PercentToAlpha(balancePercent);
        return ((uint)alpha << 24) | (argb & 0x00FFFFFFu);
    }

    public static bool IsActiveAtGateOffPreset(DwmRegistryHive hive)
    {
        if (!HasActiveColorKey(hive) || !HasColorBalanceKey(hive) || !HasBlurBalanceKey(hive))
            return false;

        var rgb = TryReadActiveColorRgb(hive);
        if (!rgb.HasValue
            || (rgb.Value & 0x00FFFFFFu) != (ResolveGateOffActiveColorRgb() & 0x00FFFFFFu))
            return false;

        return ReadFactualActiveBalancePercent(hive) == GateOffActiveColorBalancePercent
            && ReadFactualBlurBalancePercent(hive) == GateOffBlurBalancePercent;
    }

    public static bool IsAfterglowAtGateOffPreset(DwmRegistryHive hive) =>
        !HasAfterglowOverrideKey(hive);

    public static bool IsActiveGateOn(DwmRegistryHive hive) => HasActiveOverrideKey(hive);

    public static bool IsAfterglowGateOn(DwmRegistryHive hive) => HasAfterglowOverrideKey(hive);

    /// <summary>Устаревший gate-off пресет (SWCA) — не вызывать из UI; оставлен для совместимости миграции.</summary>
    [Obsolete("Gate OFF удаляет Override; не записывает SWCA preset.")]
    public static void ApplyActiveGateOffSystemDefault(DwmRegistryHive hive)
    {
        var bgr = ResolveGateOffActiveColorRgb();
        WriteGateOffColorPair(
            hive,
            DwmMirrorRegistry.GlassColorPrimary,
            DwmMirrorRegistry.GlassColorOverride,
            bgr);
        DwmMirrorRegistry.WriteBalancePair(
            hive,
            DwmMirrorRegistry.ColorBalancePrimary,
            DwmMirrorRegistry.ColorBalanceOverride,
            GateOffActiveColorBalancePercent,
            liveDwm: true);
        DwmMirrorRegistry.WriteBalancePair(
            hive,
            DwmMirrorRegistry.BlurBalancePrimary,
            DwmMirrorRegistry.BlurBalanceOverride,
            GateOffBlurBalancePercent,
            liveDwm: true);
    }

    [Obsolete("Gate OFF удаляет Override; тема не перезаписывает SWCA preset.")]
    public static bool ShouldRefreshActiveGateOffSystemDefault(DwmRegistryHive hive, bool uiGateOn, bool armed) =>
        false;

    private static byte PercentToAlpha(uint percent) => (byte)((percent * 255u + 50u) / 100u);

    /// <summary>DWORD для gate-off: alpha 0xFF при absent/нулевой альфе (ToDwordPreserveAlpha даёт 0 → чёрное стекло).</summary>
    private static uint ComposeGateOffColorDword(
        uint bgrRgb,
        DwmRegistryHive hive,
        string primary,
        string overrideKey)
    {
        var alpha = 0xFFu;
        var key = DwmMirrorRegistry.ResolveWriteKey(hive, primary, overrideKey);
        if (DwmRegistryStore.ReadDwordForPage(hive, key) is { } current)
            alpha = (current >> 24) & 0xFF;
        if (alpha == 0)
            alpha = 0xFFu;

        return (alpha << 24) | (bgrRgb & 0x00FFFFFFu);
    }

    private static void WriteGateOffColorPair(
        DwmRegistryHive hive,
        string primary,
        string overrideKey,
        uint bgrRgb)
    {
        var key = DwmMirrorRegistry.ResolveWriteKey(hive, primary, overrideKey);
        if (DwmSnapshotExclusions.IsExcludedDwmValue(key))
            SettingsSession.Current.PrimeExcludedDwmKeyForRevert(hive, key);

        var dword = ComposeGateOffColorDword(bgrRgb, hive, primary, overrideKey);
        SettingsSession.Current.WriteDwmDword(hive, key, dword, liveDwm: true, dwmNotifyImmediate: true);
    }
}
