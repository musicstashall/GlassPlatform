using GlassSettings.Core.Registry;
using Microsoft.Win32;

namespace GlassSettings.Services;

/// <summary>Light/Dark для chrome-превью: активная .theme → entry → Personalize → GetSysColor.</summary>
public static class ThemePreviewChromeModeResolver
{
    public static bool ResolveIsDarkMode(VisualStyleThemeEntry? entry = null)
    {
        if (TryReadActiveThemeIsDark(out var isDark))
            return isDark;

        if (entry is not null && !string.IsNullOrWhiteSpace(entry.ThemeFilePath))
        {
            if (ThemeAppModeHelper.TryReadIsDarkMode(entry.ThemeFilePath, out isDark))
                return isDark;

            return entry.IsDarkMode;
        }

        if (TryReadAppsUseLightThemeIsDark(out isDark))
            return isDark;

        return ThemeRegistry.IsDarkMode();
    }

    public static Windows.UI.Color? ResolveAccentColor(VisualStyleThemeEntry? entry = null)
    {
        if (entry?.AccentColor is { } accent)
            return accent;

        if (VisualStyleThemeManager.TryGetCurrentThemeFilePath(out var rawPath))
        {
            var path = ThemePathNormalizer.Normalize(rawPath);
            if (!string.IsNullOrWhiteSpace(path))
                return ThemeAccentColorHelper.TryParseColorizationColor(path);
        }

        if (entry is not null && !string.IsNullOrWhiteSpace(entry.ThemeFilePath))
            return ThemeAccentColorHelper.TryParseColorizationColor(entry.ThemeFilePath);

        return null;
    }

    /// <summary>AppMode конкретной .theme (для плиток сетки, не активной темы).</summary>
    public static bool ResolveIsDarkModeForEntry(VisualStyleThemeEntry? entry)
    {
        if (entry is not null && !string.IsNullOrWhiteSpace(entry.ThemeFilePath))
        {
            if (ThemeAppModeHelper.TryReadIsDarkMode(entry.ThemeFilePath, out var isDark))
                return isDark;

            return entry.IsDarkMode;
        }

        return false;
    }

    public static Windows.UI.Color? ResolveAccentColorForEntry(VisualStyleThemeEntry? entry)
    {
        if (entry?.AccentColor is { } accent)
            return accent;

        if (entry is not null && !string.IsNullOrWhiteSpace(entry.ThemeFilePath))
            return ThemeAccentColorHelper.TryParseColorizationColor(entry.ThemeFilePath);

        return null;
    }

    private static bool TryReadActiveThemeIsDark(out bool isDark)
    {
        isDark = false;
        if (!VisualStyleThemeManager.TryGetCurrentThemeFilePath(out var rawPath))
            return false;

        var path = ThemePathNormalizer.Normalize(rawPath);
        return !string.IsNullOrWhiteSpace(path) &&
               ThemeAppModeHelper.TryReadIsDarkMode(path, out isDark);
    }

    private static bool TryReadAppsUseLightThemeIsDark(out bool isDark)
    {
        isDark = false;
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(
                @"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
            if (key?.GetValue("AppsUseLightTheme") is not int value)
                return false;

            isDark = value == 0;
            return true;
        }
        catch
        {
            return false;
        }
    }
}
