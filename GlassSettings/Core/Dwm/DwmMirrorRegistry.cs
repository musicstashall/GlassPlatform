using GlassSettings.Core.Registry;
using GlassSettings.Core.Snapshot;
using GlassSettings.Services;

namespace GlassSettings.Core.Dwm;

/// <summary>
/// Пары primary/Override DWM — запись в Override только при gate ON (через Write*Pair).
/// Чтение: Override → Primary → null (без миграции).
/// </summary>
public static class DwmMirrorRegistry
{
    public const string GlassColorPrimary = "ColorizationColor";
    public const string GlassColorOverride = "ColorizationColorOverride";
    public const string AfterglowPrimary = "ColorizationAfterglow";
    public const string AfterglowOverride = "ColorizationAfterglowOverride";
    public const string BlurBalancePrimary = "ColorizationBlurBalance";
    public const string BlurBalanceOverride = "ColorizationBlurBalanceOverride";
    public const string AfterglowBalancePrimary = "ColorizationAfterglowBalance";
    public const string AfterglowBalanceOverride = "ColorizationAfterglowBalanceOverride";
    public const string ColorBalancePrimary = "ColorizationColorBalance";
    public const string ColorBalanceOverride = "ColorizationColorBalanceOverride";

    public static string ResolveWriteKey(
        DwmRegistryHive hive,
        string primary,
        string overrideKey) =>
        overrideKey;

    public static uint? ReadEffectiveDword(
        DwmRegistryHive hive,
        string primary,
        string overrideKey) =>
        ReadEffectiveDwordWithinHive(hive, primary, overrideKey);

    /// <summary>Runtime OpenGlass: HKCU pair → HKLM pair.</summary>
    public static uint? ReadRuntimeEffectiveDword(string primary, string overrideKey) =>
        ReadEffectiveDwordWithinHive(DwmRegistryHive.Hkcu, primary, overrideKey)
        ?? ReadEffectiveDwordWithinHive(DwmRegistryHive.Hklm, primary, overrideKey);

    /// <summary>RGB из пикера; альфа сохраняется из текущего значения ключа (OG colorToDwordIgnoreAlpha).</summary>
    public static uint ToDwordPreserveAlpha(DwmRegistryHive hive, string resolvedKey, uint argbFromPicker)
    {
        var alpha = 0u;
        if (DwmRegistry.ReadDword(hive, resolvedKey) is { } current)
            alpha = (current >> 24) & 0xFF;

        return (alpha << 24)
               | ((argbFromPicker >> 16) & 0xFF) << 16
               | ((argbFromPicker >> 8) & 0xFF) << 8
               | (argbFromPicker & 0xFF);
    }

    public static void WriteColorPair(
        DwmRegistryHive hive,
        string primary,
        string overrideKey,
        uint argbFromPicker,
        bool liveDwm)
    {
        var key = ResolveWriteKey(hive, primary, overrideKey);
        PrimeExcludedKeyIfNeeded(hive, key);
        var dword = ToDwordPreserveAlpha(hive, key, argbFromPicker);
        SettingsSession.Current.WriteDwmDword(hive, key, dword, liveDwm: liveDwm, dwmNotifyImmediate: true);
    }

    /// <summary>Platform DWORD в Override без ToDwordPreserveAlpha (полный alpha из SWCA gradient).</summary>
    public static void WriteGlassColorOverrideDword(DwmRegistryHive hive, uint dword, bool liveDwm)
    {
        var key = GlassColorOverride;
        PrimeExcludedKeyIfNeeded(hive, key);
        SettingsSession.Current.WriteDwmDword(hive, key, dword, liveDwm: liveDwm, dwmNotifyImmediate: true);
    }

    public static void WriteBalancePairLivePreview(
        DwmRegistryHive hive,
        string primary,
        string overrideKey,
        uint value)
    {
        var key = ResolveWriteKey(hive, primary, overrideKey);
        PrimeExcludedKeyIfNeeded(hive, key);
        SettingsSession.Current.WriteDwmDwordLivePreview(hive, key, value);
    }

    public static void WriteColorPairLivePreview(
        DwmRegistryHive hive,
        string primary,
        string overrideKey,
        uint argbFromPicker)
    {
        var key = ResolveWriteKey(hive, primary, overrideKey);
        PrimeExcludedKeyIfNeeded(hive, key);
        var dword = ToDwordPreserveAlpha(hive, key, argbFromPicker);
        SettingsSession.Current.WriteDwmDwordLivePreview(hive, key, dword);
    }

    public static void WriteBalancePair(
        DwmRegistryHive hive,
        string primary,
        string overrideKey,
        uint value,
        bool liveDwm)
    {
        var key = ResolveWriteKey(hive, primary, overrideKey);
        PrimeExcludedKeyIfNeeded(hive, key);
        SettingsSession.Current.WriteDwmDword(hive, key, value, liveDwm: liveDwm, dwmNotifyImmediate: true);
    }

    public static void DeletePair(DwmRegistryHive hive, string primary, string overrideKey, bool liveDwm)
    {
        if (DwmRegistry.ValueExists(hive, overrideKey))
            SettingsSession.Current.DeleteDwmValue(hive, overrideKey, liveDwm);

        if (DwmRegistry.ValueExists(hive, primary))
            SettingsSession.Current.DeleteDwmValue(hive, primary, liveDwm);
    }

    private static void PrimeExcludedKeyIfNeeded(DwmRegistryHive hive, string valueName)
    {
        if (!DwmSnapshotExclusions.IsExcludedDwmValue(valueName))
            return;

        SettingsSession.Current.PrimeExcludedDwmKeyForRevert(hive, valueName);
    }

    private static uint? ReadEffectiveDwordWithinHive(
        DwmRegistryHive hive,
        string primary,
        string overrideKey)
    {
        if (DwmRegistry.ValueExists(hive, overrideKey))
            return DwmRegistry.ReadDword(hive, overrideKey);

        return DwmRegistry.ReadDword(hive, primary);
    }
}
