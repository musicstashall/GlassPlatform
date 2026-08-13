using GlassSettings.Core.Registry;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace GlassSettings.Services;

/// <summary>
/// Визуальная light/dark для overlay и in-app acrylic по реестру AppTheme.
/// Application.RequestedTheme после старта окна не меняется — ориентируемся на GlassSettingsRegistry.
/// </summary>
public static class AppThemeVisualResolver
{
    public static bool ResolveIsDarkMode() =>
        GlassSettingsRegistry.ReadAppTheme() switch
        {
            AppThemeMode.Light => false,
            AppThemeMode.Dark => true,
            _ => ResolveSystemIsDark()
        };

    public static ElementTheme ResolveElementTheme() =>
        GlassSettingsRegistry.ReadAppTheme() switch
        {
            AppThemeMode.Light => ElementTheme.Light,
            AppThemeMode.Dark => ElementTheme.Dark,
            _ => WindowThemeBootstrap.ResolveElementTheme()
        };

    public static Brush? ResolveThemeResourceBrush(string resourceKey) =>
        CoerceBrush(TryResolveThemedResource(resourceKey));

    /// <summary>Кисть по AppTheme для императивного кода (footer, overlay).</summary>
    public static Brush ResolveThemeBrush(string resourceKey)
    {
        if (SettingsAlertPalette.TryResolveBrush(resourceKey, out var alert))
            return alert;

        if (ResolveThemeResourceBrush(resourceKey) is { } themed)
            return themed;

        if (ResolveSemanticFallbackBrush(resourceKey) is { } semantic)
            return semantic;

        if (Application.Current.Resources.TryGetValue(resourceKey, out var raw)
            && CoerceBrush(raw) is Brush fromApp)
            return fromApp;

        var colorKey = resourceKey.EndsWith("Brush", StringComparison.Ordinal)
            ? resourceKey[..^"Brush".Length]
            : resourceKey + "Brush";
        if (Application.Current.Resources.TryGetValue(colorKey, out var altRaw)
            && CoerceBrush(altRaw) is Brush fromAlt)
            return fromAlt;

        return new SolidColorBrush(Color.FromArgb(0, 0, 0, 0));
    }

    private static bool ResolveSystemIsDark()
    {
        var theme = WindowThemeBootstrap.ResolveElementTheme();
        if (theme == ElementTheme.Light)
            return false;
        if (theme == ElementTheme.Dark)
            return true;

        return ThemePreviewChromeModeResolver.ResolveIsDarkMode();
    }

    private static object? TryResolveThemedResource(string resourceKey)
    {
        var isDark = ResolveIsDarkMode();

        if (TryFromThemeDictionaries(Application.Current.Resources, resourceKey, isDark, out var value))
            return value;

        foreach (var merged in Application.Current.Resources.MergedDictionaries)
        {
            if (TryFromThemeDictionaries(merged, resourceKey, isDark, out value))
                return value;

            foreach (var nested in merged.MergedDictionaries)
            {
                if (TryFromThemeDictionaries(nested, resourceKey, isDark, out value))
                    return value;
            }
        }

        var colorKey = resourceKey.EndsWith("Brush", StringComparison.Ordinal)
            ? resourceKey[..^"Brush".Length]
            : null;
        if (colorKey is null)
            return null;

        if (TryFromThemeDictionaries(Application.Current.Resources, colorKey, isDark, out value))
            return value;

        foreach (var merged in Application.Current.Resources.MergedDictionaries)
        {
            if (TryFromThemeDictionaries(merged, colorKey, isDark, out value))
                return value;

            foreach (var nested in merged.MergedDictionaries)
            {
                if (TryFromThemeDictionaries(nested, colorKey, isDark, out value))
                    return value;
            }
        }

        return null;
    }

    private static bool TryFromThemeDictionaries(
        ResourceDictionary resources,
        string resourceKey,
        bool isDark,
        out object? value)
    {
        value = null;
        if (resources.ThemeDictionaries.Count == 0)
            return false;

        foreach (var themeKey in ThemeDictionaryKeys(isDark))
        {
            if (!resources.ThemeDictionaries.TryGetValue(themeKey, out var dictObj)
                || dictObj is not ResourceDictionary themeResources
                || !themeResources.TryGetValue(resourceKey, out value))
            {
                continue;
            }

            return CoerceBrush(value) is not null;
        }

        return false;
    }

    private static Brush? CoerceBrush(object? value) =>
        value switch
        {
            Brush brush => brush,
            Color color => new SolidColorBrush(color),
            _ => null
        };

    private static Brush? ResolveSemanticFallbackBrush(string resourceKey)
    {
        var isDark = ResolveIsDarkMode();
        return resourceKey switch
        {
            "TextFillColorSecondaryBrush" => new SolidColorBrush(isDark
                ? Color.FromArgb(255, 0xC8, 0xC6, 0xC4)
                : Color.FromArgb(255, 0x60, 0x5E, 0x5C)),
            "AccentTextFillColorPrimaryBrush" => new SolidColorBrush(isDark
                ? Color.FromArgb(255, 0x99, 0xE6, 0xFF)
                : Color.FromArgb(255, 0x00, 0x78, 0xD4)),
            "SolidBackgroundFillColorBaseBrush" => new SolidColorBrush(isDark
                ? Color.FromArgb(255, 0x20, 0x20, 0x20)
                : Color.FromArgb(255, 0xF3, 0xF3, 0xF3)),
            _ => null
        };
    }

    private static IEnumerable<object> ThemeDictionaryKeys(bool isDark)
    {
        if (isDark)
        {
            yield return "Dark";
            yield return ElementTheme.Dark;
        }
        else
        {
            yield return "Light";
            yield return ElementTheme.Light;
        }
    }
}
