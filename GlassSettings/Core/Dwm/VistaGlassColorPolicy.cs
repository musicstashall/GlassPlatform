using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Snapshot;
using GlassSettings.Services;

namespace GlassSettings.Core.Dwm;

/// <summary>
/// Vista Aero: OG factual absent (UI/превью без записи) vs gate ON (Override / GlassOpacity).
/// </summary>
public static class VistaGlassColorPolicy
{
    public const uint FactualAbsentColorRgb = 0;
    public const uint FactualAbsentGlassOpacityPercent = 0;

    /// <summary>OG runtime: GetDwordFromRegistry(ColorizationColor, 0xFFFFFFFF) → white BGR.</summary>
    public const uint OgAbsentActiveColorBgr = 0x00FFFFFFu;

    public const uint OgAbsentGlassOpacityPercent = 63u;

    public static bool HasActiveOverrideKey(DwmRegistryHive hive) =>
        DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.GlassColorOverride)
        || DwmRegistryStore.ValueExistsForPage(hive, "GlassOpacity");

    public static bool HasActiveColorKey(DwmRegistryHive hive) =>
        DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.GlassColorOverride)
        || DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.GlassColorPrimary);

    public static bool HasGlassOpacityKey(DwmRegistryHive hive) =>
        DwmRegistryStore.ValueExistsForPage(hive, "GlassOpacity");

    public static uint? TryReadActiveColorRgb(DwmRegistryHive hive) =>
        DwmMirrorRegistry.ReadEffectiveDword(
            hive,
            DwmMirrorRegistry.GlassColorPrimary,
            DwmMirrorRegistry.GlassColorOverride);

    /// <summary>UI/превью: absent → 0, не OG gate-off дефолты (паритет Win7GlassColorPolicy).</summary>
    public static uint ReadFactualActiveColorRgb(DwmRegistryHive hive) =>
        TryReadActiveColorRgb(hive) ?? FactualAbsentColorRgb;

    /// <summary>UI/превью: absent → 0 (паритет Win7 ReadFactualActiveBalancePercent).</summary>
    public static uint ReadFactualGlassOpacityPercent(DwmRegistryHive hive)
    {
        if (!HasGlassOpacityKey(hive))
            return FactualAbsentGlassOpacityPercent;

        var value = DwmRegistryStore.ReadDwordForPage(hive, "GlassOpacity") ?? FactualAbsentGlassOpacityPercent;
        return Math.Min(value, 100u);
    }

    public static bool HasRuntimeGlassOpacityKey() =>
        DwmRegistryStore.ExistsInAnyHive("GlassOpacity");

    public static uint ReadRuntimeFactualActiveColorRgb() =>
        DwmMirrorRegistry.ReadRuntimeEffectiveDword(
            DwmMirrorRegistry.GlassColorPrimary,
            DwmMirrorRegistry.GlassColorOverride) ?? FactualAbsentColorRgb;

    public static uint ReadRuntimeFactualGlassOpacityPercent()
    {
        if (!HasRuntimeGlassOpacityKey())
            return FactualAbsentGlassOpacityPercent;

        var value = DwmRegistryStore.ReadEffectiveDword("GlassOpacity") ?? FactualAbsentGlassOpacityPercent;
        return Math.Min(value, 100u);
    }

    public static bool HasRuntimeInactiveOverrideKey() =>
        DwmRegistryStore.ExistsInAnyHive("ColorizationColorInactive")
        || DwmRegistryStore.ExistsInAnyHive("GlassOpacityInactive");

    /// <summary>UI пикера α: OG default если ключ absent (паритет Win7 ReadUiActiveBalancePercent).</summary>
    public static uint ReadUiGlassOpacityPercent(DwmRegistryHive hive) =>
        HasGlassOpacityKey(hive)
            ? ReadFactualGlassOpacityPercent(hive)
            : DwmColorRegistry.DefaultGlassOpacityPercent;

    /// <summary>OG compositor: TryRead → OG absent default (без записи в реестр).</summary>
    public static uint ReadOgFactualActiveColorRgb(DwmRegistryHive hive) =>
        TryReadActiveColorRgb(hive) ?? OgAbsentActiveColorBgr;

    public static uint ReadOgFactualGlassOpacityPercent(DwmRegistryHive hive)
    {
        if (!HasGlassOpacityKey(hive))
            return OgAbsentGlassOpacityPercent;

        var value = DwmRegistryStore.ReadDwordForPage(hive, "GlassOpacity") ?? OgAbsentGlassOpacityPercent;
        return Math.Min(value, 100u);
    }

    public static (uint BgrRgb, uint OpacityPercent) ResolveGateOffValues() =>
        RootSwcaGlassDefaults.ResolveActiveGateOffValues();

    public static bool IsActiveAtGateOffPreset(DwmRegistryHive hive) =>
        !HasActiveOverrideKey(hive);

    public static bool IsActiveGateOn(DwmRegistryHive hive) => HasActiveOverrideKey(hive);

    public static bool HasInactiveColorKey(DwmRegistryHive hive) =>
        DwmRegistryStore.ValueExistsForPage(hive, "ColorizationColorInactive");

    public static bool HasInactiveOpacityKey(DwmRegistryHive hive) =>
        DwmRegistryStore.ValueExistsForPage(hive, "GlassOpacityInactive");

    /// <summary>Gate ON: inactive color и/или opacity override (паритет active Override / GlassOpacity).</summary>
    public static bool HasInactiveOverrideKey(DwmRegistryHive hive) =>
        HasInactiveColorKey(hive) || HasInactiveOpacityKey(hive);

    /// <summary>Gate OFF: удалить inactive color и opacity.</summary>
    public static void ClearInactiveOverrides(DwmRegistryHive hive)
    {
        var changed = false;
        if (DwmRegistryStore.ValueExistsForPage(hive, "ColorizationColorInactive"))
        {
            SettingsSession.Current.DeleteDwmValue(hive, "ColorizationColorInactive", liveDwm: true);
            changed = true;
        }

        if (DwmRegistryStore.ValueExistsForPage(hive, "GlassOpacityInactive"))
        {
            SettingsSession.Current.DeleteDwmValue(hive, "GlassOpacityInactive", liveDwm: true);
            changed = true;
        }

        if (changed)
            LiveReloadScheduler.NotifyDwmColorizationNow();
    }

    /// <summary>Gate OFF: удалить Override color, GlassOpacity и BlurDeviation.</summary>
    public static void ClearActiveOverrides(DwmRegistryHive hive)
    {
        var changed = false;
        if (DwmRegistryStore.ValueExistsForPage(hive, DwmMirrorRegistry.GlassColorOverride))
        {
            SettingsSession.Current.DeleteDwmValue(hive, DwmMirrorRegistry.GlassColorOverride, liveDwm: false);
            changed = true;
        }

        if (DwmRegistryStore.ValueExistsForPage(hive, "GlassOpacity"))
        {
            SettingsSession.Current.DeleteDwmValue(hive, "GlassOpacity", liveDwm: false);
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

    [Obsolete("Gate OFF удаляет Override; не записывает SWCA preset.")]
    public static void ApplyActiveGateOffSwcaDefault(DwmRegistryHive hive)
    {
        var (bgr, opacityPercent) = ResolveGateOffValues();
        WriteGateOffColorPair(
            hive,
            DwmMirrorRegistry.GlassColorPrimary,
            DwmMirrorRegistry.GlassColorOverride,
            bgr);
        SettingsSession.Current.WriteDwmDword(
            hive,
            "GlassOpacity",
            opacityPercent,
            liveDwm: true,
            dwmNotifyImmediate: true);
    }

    [Obsolete("Gate OFF удаляет Override; тема не перезаписывает SWCA preset.")]
    public static bool ShouldRefreshActiveGateOffPreset(DwmRegistryHive hive, bool uiGateOn, bool armed) =>
        false;

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
}
