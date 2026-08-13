using GlassSettings.Core.Dwm;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Settings;

namespace GlassSettings.Core.Flyout;

/// <summary>Наличие и удаление theme-параметров flyout (градиент, бордюр).</summary>
public static class FlyoutThemeRegistry
{
    private const string TrayXamlSub = "Tray\\Xaml";
    private const string TrayXamlPrefix = "Tray\\Xaml\\";

    public static bool IsTrayXamlSub(string? regSub) =>
        string.Equals(regSub, TrayXamlSub, StringComparison.OrdinalIgnoreCase);

    /// <summary>Tray\Xaml или Tray\Xaml\{Popup|Preview|Menu|Tooltip}.</summary>
    public static bool IsTrayXamlFamily(string? regSub) =>
        IsTrayXamlSub(regSub)
        || (regSub is not null
            && regSub.StartsWith(TrayXamlPrefix, StringComparison.OrdinalIgnoreCase));

    public static bool IsGradientPresent(string? regSub) =>
        GlassSectionGate.IsAnyPresent(regSub, ThemeParameterNames.GradientNames);

    public static bool IsBorderPresent(string? regSub) =>
        GlassSectionGate.IsAnyPresent(regSub, ThemeParameterNames.BorderNames);

    public static void DeleteGradient(string? regSub) =>
        GlassSectionGate.DeleteAll(regSub, ThemeParameterNames.GradientNames);

    public static void DeleteBorder(string? regSub) =>
        GlassSectionGate.DeleteAll(regSub, ThemeParameterNames.BorderNames);

    public static uint DefaultGradientArgb() => RootSwcaGlassDefaults.DefaultGradientArgb();

    /// <summary>
    /// Fill: устаревший Cached* (только in-memory в TaskbarCompat).
    /// Stroke: CachedSystemStrokeArgb публикует TaskbarCompat для «По умолчанию» — не удалять.
    /// </summary>
    public static void PurgeStaleCachedSystemSamples()
    {
        GlassRegistry.DeleteValue(TrayXamlSub, "CachedSystemFillArgb");
    }

    public static uint ReadGradientArgbForUi(string regSub)
    {
        if (IsTrayXamlFamily(regSub))
        {
            var name = ThemeRegistry.GradientColorName();
            if (GlassRegistry.ReadDword(regSub, name) is { } local)
                return local;
            // Surface → bar → chrome default.
            if (!IsTrayXamlSub(regSub)
                && GlassRegistry.ReadDword(TrayXamlSub, name) is { } bar)
                return bar;
            // Динамический chrome-fallback (как TaskbarCompat без Cached* в реестре).
            return ThemeRegistry.IsDarkMode() ? 0xE42C2C2Cu : 0xE4F3F3F3u;
        }

        return PartRegistryInterop.ReadUiGradientArgb(regSub, ThemeRegistry.IsDarkMode());
    }

    public static uint ReadBorderArgbForUi(string regSub, string colorValueName)
    {
        if (IsTrayXamlFamily(regSub))
        {
            if (GlassRegistry.ReadDword(regSub, colorValueName) is { } local)
                return local;
            if (!IsTrayXamlSub(regSub)
                && GlassRegistry.ReadDword(TrayXamlSub, colorValueName) is { } bar)
                return bar;
            return ResolveDefaultBorderArgb(regSub, colorValueName);
        }

        return PartRegistryInterop.ReadUiBorderArgb(regSub, ThemeRegistry.IsDarkMode());
    }

    public static void EnsureGradientEnabled(string regSub)
    {
        if (GlassRegistry.ReadDword(regSub, ThemeRegistry.GradientColorName()).HasValue)
            return;
        // Tray\Xaml*: отсутствие ключа = fallback/system — не пишем SWCA default.
        if (IsTrayXamlFamily(regSub))
            return;
        GlassRegistry.WriteDword(regSub, ThemeRegistry.GradientColorName(), DefaultGradientArgb());
    }

    public static void EnsureBorderEnabled(string regSub, string colorValueName)
    {
        if (GlassRegistry.ReadDword(regSub, colorValueName).HasValue)
            return;
        if (IsTrayXamlFamily(regSub))
            return;
        var fallback = ReadBorderArgbForUi(regSub, colorValueName);
        GlassRegistry.WriteDword(regSub, colorValueName, fallback);
    }

    /// <summary>Сброс градиента к цвету по умолчанию — только если ключ уже есть (запись ARGB).</summary>
    public static uint ResetGradientToDefault(string regSub)
    {
        var name = ThemeRegistry.GradientColorName();
        if (!GlassRegistry.ReadDword(regSub, name).HasValue)
            return ReadGradientArgbForUi(regSub);

        var def = ResolveDefaultGradientArgb(regSub);
        GlassRegistry.WriteDword(regSub, name, def);
        return def;
    }

    /// <summary>Сброс бордюра к цвету по умолчанию — только если ключ уже есть.</summary>
    public static uint ResetBorderToDefault(string regSub, string colorValueName)
    {
        if (!GlassRegistry.ReadDword(regSub, colorValueName).HasValue)
            return ReadBorderArgbForUi(regSub, colorValueName);

        var def = ResolveDefaultBorderArgb(regSub, colorValueName);
        GlassRegistry.WriteDword(regSub, colorValueName, def);
        return def;
    }

    public static bool IsGradientAtDefault(string regSub)
    {
        var current = GlassRegistry.ReadDword(regSub, ThemeRegistry.GradientColorName());
        if (!current.HasValue)
            return true;
        return current.Value == ResolveDefaultGradientArgb(regSub);
    }

    /// <summary>
    /// «По умолчанию» недоступна: ключа нет, либо значение уже равно дефолту.
    /// </summary>
    public static bool ShouldDisableGradientDefaultButton(string regSub)
    {
        var current = GlassRegistry.ReadDword(regSub, ThemeRegistry.GradientColorName());
        if (!current.HasValue)
            return true;
        return current.Value == ResolveDefaultGradientArgb(regSub);
    }

    public static bool IsBorderAtDefault(string regSub, string colorValueName)
    {
        var current = GlassRegistry.ReadDword(regSub, colorValueName);
        if (!current.HasValue)
            return true;
        return current.Value == ResolveDefaultBorderArgb(regSub, colorValueName);
    }

    /// <summary>
    /// «По умолчанию» недоступна: ключа нет, либо значение уже равно дефолту.
    /// </summary>
    public static bool ShouldDisableBorderDefaultButton(string regSub, string colorValueName)
    {
        var current = GlassRegistry.ReadDword(regSub, colorValueName);
        if (!current.HasValue)
            return true;
        return current.Value == ResolveDefaultBorderArgb(regSub, colorValueName);
    }

    public static uint ResolveDefaultGradientArgb(string regSub)
    {
        if (IsTrayXamlFamily(regSub))
            return ThemeRegistry.IsDarkMode() ? 0xE42C2C2Cu : 0xE4F3F3F3u;

        return DefaultGradientArgb();
    }

    /// <summary>
    /// Дефолт бордюра без мутации реестра.
    /// Tray\Xaml*: system stroke до патча (CachedSystemStrokeArgb), иначе ChartView.
    /// </summary>
    public static uint ResolveDefaultBorderArgb(string regSub, string colorValueName)
    {
        if (IsTrayXamlFamily(regSub))
        {
            if (GlassRegistry.ReadDword(TrayXamlSub, "CachedSystemStrokeArgb") is { } cached
                && cached != 0)
                return cached;
            // ChartView bar-sample (surfaces делят тот же system stroke).
            return PartRegistryInterop.ReadUiBorderArgb(TrayXamlSub, ThemeRegistry.IsDarkMode());
        }

        return PartRegistryInterop.ReadUiBorderArgb(regSub, ThemeRegistry.IsDarkMode());
    }
}
