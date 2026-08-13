using GlassSettings.Core.Dwm;
using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Snapshot;

namespace GlassSettings.Services;

/// <summary>
/// Секция [Glass.OpenGlass] в .theme — параметры DWM glass для OpenGlass.
/// Значения — DWORD реестра в hex (0x........).
/// </summary>
public static class OpenGlassThemeSection
{
    public const string SectionName = "Glass.OpenGlass";

    public const string KeyGlassType = "GlassType";
    public const string KeyColorizationColor = "ColorizationColor";
    public const string KeyColorizationColorInactive = "ColorizationColorInactive";
    public const string KeyGlassOpacity = "GlassOpacity";
    public const string KeyGlassOpacityInactive = "GlassOpacityInactive";
    public const string KeyColorizationAfterglow = "ColorizationAfterglow";
    public const string KeyColorizationAfterglowBalance = "ColorizationAfterglowBalance";
    public const string KeyColorizationColorBalance = "ColorizationColorBalance";
    public const string KeyColorizationBlurBalance = "ColorizationBlurBalance";

    public static Dictionary<string, uint>? TryRead(string themeFilePath)
    {
        if (string.IsNullOrWhiteSpace(themeFilePath) || !File.Exists(themeFilePath))
            return null;

        try
        {
            var inSection = false;
            Dictionary<string, uint>? values = null;
            foreach (var raw in ThemeFileLineReader.ReadLines(themeFilePath))
            {
                var line = raw.Trim();
                if (line.Length == 0 || line[0] == ';')
                    continue;

                if (line.StartsWith('[') && line.EndsWith(']'))
                {
                    inSection = string.Equals(
                        line[1..^1].Trim(),
                        SectionName,
                        StringComparison.OrdinalIgnoreCase);
                    continue;
                }

                if (!inSection)
                    continue;

                var separator = line.IndexOf('=');
                if (separator <= 0)
                    continue;

                var key = line[..separator].Trim();
                var valueText = line[(separator + 1)..].Trim();
                if (!TryParseDword(valueText, out var dword))
                    continue;

                values ??= new Dictionary<string, uint>(StringComparer.OrdinalIgnoreCase);
                values[key] = dword;
            }

            return values is { Count: > 0 } ? values : null;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings OpenGlassThemeSection.TryRead: {ex}");
            return null;
        }
    }

    public static bool HasSection(string themeFilePath) =>
        ThemeFileSectionWriter.SectionExists(themeFilePath, SectionName);

    /// <summary>Снимок текущих OG-параметров HKCU для записи в .theme.</summary>
    public static IReadOnlyDictionary<string, string> CaptureCurrentValues()
    {
        const DwmRegistryHive hive = DwmRegistryHive.Hkcu;
        var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

        var glassType = DwmRegistryStore.ReadDwordForPage(hive, KeyGlassType) ?? 0u;
        map[KeyGlassType] = FormatDword(glassType);
        var win7 = glassType == 1;

        if (DwmColorRegistry.ReadGlassColor(hive) is { } color)
            map[KeyColorizationColor] = FormatDword(color);

        if (win7)
        {
            if (DwmColorRegistry.ReadAfterglowColor(hive) is { } afterglow)
                map[KeyColorizationAfterglow] = FormatDword(afterglow);

            if (DwmMirrorRegistry.ReadEffectiveDword(
                    hive,
                    DwmMirrorRegistry.AfterglowBalancePrimary,
                    DwmMirrorRegistry.AfterglowBalanceOverride) is { } afterglowBalance)
            {
                map[KeyColorizationAfterglowBalance] = FormatDword(afterglowBalance);
            }

            if (DwmMirrorRegistry.ReadEffectiveDword(
                    hive,
                    DwmMirrorRegistry.ColorBalancePrimary,
                    DwmMirrorRegistry.ColorBalanceOverride) is { } colorBalance)
            {
                map[KeyColorizationColorBalance] = FormatDword(colorBalance);
            }

            if (DwmMirrorRegistry.ReadEffectiveDword(
                    hive,
                    DwmMirrorRegistry.BlurBalancePrimary,
                    DwmMirrorRegistry.BlurBalanceOverride) is { } blurBalance)
            {
                map[KeyColorizationBlurBalance] = FormatDword(blurBalance);
            }
        }
        else
        {
            if (VistaGlassColorPolicy.HasGlassOpacityKey(hive))
            {
                map[KeyGlassOpacity] = FormatDword(
                    VistaGlassColorPolicy.ReadFactualGlassOpacityPercent(hive));
            }

            if (DwmRegistryStore.ReadDwordForPage(hive, KeyColorizationColorInactive) is { } inactive)
                map[KeyColorizationColorInactive] = FormatDword(inactive);

            if (DwmRegistryStore.ReadDwordForPage(hive, KeyGlassOpacityInactive) is { } inactiveOpacity)
                map[KeyGlassOpacityInactive] = FormatDword(inactiveOpacity);
        }

        return map;
    }

    public static bool TryWriteToThemeFile(string themeFilePath)
    {
        var values = CaptureCurrentValues();
        return ThemeFileSectionWriter.TryUpsertSection(themeFilePath, SectionName, values);
    }

    /// <summary>Применить секцию в DWM + reload OG. Возвращает false, если секции нет.</summary>
    public static bool TryApplyFromThemeFile(string themeFilePath)
    {
        var values = TryRead(themeFilePath);
        if (values is null)
            return false;

        const DwmRegistryHive hive = DwmRegistryHive.Hkcu;
        var win7 = values.TryGetValue(KeyGlassType, out var glassType) && glassType == 1;

        if (values.TryGetValue(KeyGlassType, out var gt))
        {
            SettingsSession.Current.WriteDwmDword(
                hive,
                KeyGlassType,
                gt,
                liveDwm: true,
                dwmNotifyImmediate: false);
        }

        if (values.TryGetValue(KeyColorizationColor, out var colorDword))
        {
            var argb = DwmCaptionColorHelper.DwordBgrToPickerArgb(colorDword);
            DwmColorRegistry.WriteGlassColor(hive, argb, liveDwm: false);
        }

        if (win7)
        {
            if (values.TryGetValue(KeyColorizationAfterglow, out var afterglowDword))
            {
                var argb = DwmCaptionColorHelper.DwordBgrToPickerArgb(afterglowDword);
                DwmColorRegistry.WriteAfterglowColor(hive, argb, liveDwm: false);
            }

            if (values.TryGetValue(KeyColorizationAfterglowBalance, out var afterglowBalance))
            {
                DwmMirrorRegistry.WriteBalancePair(
                    hive,
                    DwmMirrorRegistry.AfterglowBalancePrimary,
                    DwmMirrorRegistry.AfterglowBalanceOverride,
                    afterglowBalance,
                    liveDwm: false);
            }

            if (values.TryGetValue(KeyColorizationColorBalance, out var colorBalance))
            {
                DwmMirrorRegistry.WriteBalancePair(
                    hive,
                    DwmMirrorRegistry.ColorBalancePrimary,
                    DwmMirrorRegistry.ColorBalanceOverride,
                    colorBalance,
                    liveDwm: false);
            }

            if (values.TryGetValue(KeyColorizationBlurBalance, out var blurBalance))
            {
                DwmMirrorRegistry.WriteBalancePair(
                    hive,
                    DwmMirrorRegistry.BlurBalancePrimary,
                    DwmMirrorRegistry.BlurBalanceOverride,
                    blurBalance,
                    liveDwm: false);
            }
        }
        else
        {
            if (values.TryGetValue(KeyGlassOpacity, out var opacity))
            {
                SettingsSession.Current.WriteDwmDword(
                    hive,
                    KeyGlassOpacity,
                    opacity,
                    liveDwm: true,
                    dwmNotifyImmediate: false);
            }

            if (values.TryGetValue(KeyColorizationColorInactive, out var inactive))
            {
                SettingsSession.Current.WriteDwmDword(
                    hive,
                    KeyColorizationColorInactive,
                    inactive,
                    liveDwm: true,
                    dwmNotifyImmediate: false);
            }

            if (values.TryGetValue(KeyGlassOpacityInactive, out var inactiveOpacity))
            {
                SettingsSession.Current.WriteDwmDword(
                    hive,
                    KeyGlassOpacityInactive,
                    inactiveOpacity,
                    liveDwm: true,
                    dwmNotifyImmediate: false);
            }
        }

        Core.Platform.LiveReloadScheduler.NotifyDwmColorizationNow();
        return true;
    }

    /// <summary>
    /// Dirty для «Сохранить»: OG exposed и live DWM (GlassType/Override) отличается от
    /// baseline после последнего apply темы (паритет Default на Colors).
    /// Без baseline — абсолютный gate-off dirty (холодный старт).
    /// </summary>
    public static bool HasNonDefaultControlledSettings()
    {
        if (!OpenGlassUiPolicy.IsFeatureExposedInUi())
            return false;

        var live = ComputeSaveDirtyFingerprint();
        var baseline = GlassSettingsRegistry.ReadLastAppliedThemeDwmFingerprint();
        // Старый формат без v2 (Afterglow value) — мигрируем, иначе ложный dirty после смены темы.
        if (!string.IsNullOrWhiteSpace(baseline)
            && !baseline.StartsWith("v2|", StringComparison.Ordinal))
        {
            CaptureSaveBaselineFromLive();
            return false;
        }

        if (!string.IsNullOrWhiteSpace(baseline))
            return !string.Equals(live, baseline, StringComparison.Ordinal);

        return IsAbsolutePolicyDirty();
    }

    /// <summary>Снять baseline «Сохранить» с текущего HKCU DWM (после settle темы).</summary>
    public static void CaptureSaveBaselineFromLive()
    {
        if (!OpenGlassUiPolicy.IsFeatureExposedInUi())
            return;

        var fp = ComputeSaveDirtyFingerprint();
        GlassSettingsRegistry.WriteLastAppliedThemeDwmFingerprint(fp);
    }

    /// <summary>Абсолютный dirty относительно policy gate-off (без theme baseline).</summary>
    public static bool IsAbsolutePolicyDirty()
    {
        const DwmRegistryHive hive = DwmRegistryHive.Hkcu;

        var glassType = DwmRegistryStore.ReadDwordForPage(hive, KeyGlassType) ?? 0u;
        if (glassType != 0)
            return true;

        if (VistaGlassColorPolicy.HasActiveOverrideKey(hive)
            || Win7GlassColorPolicy.HasActiveOverrideKey(hive))
        {
            return true;
        }

        if (VistaGlassColorPolicy.HasInactiveOverrideKey(hive))
            return true;

        if (Win7GlassColorPolicy.HasAfterglowOverrideKey(hive))
            return true;

        return false;
    }

    /// <summary>
    /// Fingerprint подконтрольных DWM-ключей для Save (GlassType + Override/GlassOpacity).
    /// AfterglowOverride — только presence: значение переписывается темой вместе с акцентом.
    /// </summary>
    public static string ComputeSaveDirtyFingerprint()
    {
        const DwmRegistryHive hive = DwmRegistryHive.Hkcu;
        static string Slot(string name, uint? value) =>
            value.HasValue ? $"{name}={value.Value:X8}" : $"{name}=-";

        static string Presence(string name, uint? value) =>
            value.HasValue ? $"{name}=1" : $"{name}=-";

        uint? Read(string key) => DwmRegistryStore.ReadDwordForPage(hive, key);

        // v2: AfterglowOverride без значения (тема меняет цвет → ложный dirty).
        return string.Join('|',
            "v2",
            Slot(KeyGlassType, Read(KeyGlassType)),
            Slot(DwmMirrorRegistry.GlassColorOverride, Read(DwmMirrorRegistry.GlassColorOverride)),
            Slot("GlassOpacity", Read("GlassOpacity")),
            Slot("ColorizationColorInactive", Read("ColorizationColorInactive")),
            Slot(DwmMirrorRegistry.ColorBalanceOverride, Read(DwmMirrorRegistry.ColorBalanceOverride)),
            Slot(DwmMirrorRegistry.BlurBalanceOverride, Read(DwmMirrorRegistry.BlurBalanceOverride)),
            Presence(DwmMirrorRegistry.AfterglowOverride, Read(DwmMirrorRegistry.AfterglowOverride)),
            Slot(DwmMirrorRegistry.AfterglowBalanceOverride, Read(DwmMirrorRegistry.AfterglowBalanceOverride)));
    }

    /// <summary>Нет baseline или устаревший формат → снять с live (холодный старт / миграция).</summary>
    public static bool EnsureSaveBaselineSeeded()
    {
        if (!OpenGlassUiPolicy.IsFeatureExposedInUi())
            return false;

        var baseline = GlassSettingsRegistry.ReadLastAppliedThemeDwmFingerprint();
        if (!string.IsNullOrWhiteSpace(baseline)
            && baseline.StartsWith("v2|", StringComparison.Ordinal))
        {
            return false;
        }

        CaptureSaveBaselineFromLive();
        return true;
    }

    private static string FormatDword(uint value) => "0x" + value.ToString("X8");

    private static bool TryParseDword(string raw, out uint dword)
    {
        dword = 0;
        if (string.IsNullOrWhiteSpace(raw))
            return false;

        var text = raw.Trim();
        if (text.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            text = text[2..];

        return uint.TryParse(text, System.Globalization.NumberStyles.HexNumber, null, out dword)
               || uint.TryParse(text, System.Globalization.NumberStyles.Integer, null, out dword);
    }
}
