using Windows.UI;

namespace GlassSettings.Services;

public readonly record struct ThemePreviewChromeColors(
    Color AcrylicBackground,
    Color LineStrong,
    Color LineSoft,
    Color AccentStripe);

/// <summary>Акриловое мини-превью hero-плитки (светлое / тёмное + полоска accent).</summary>
public static class ThemePreviewChromePalette
{
    private static readonly Color DefaultLightAccent = Color.FromArgb(255, 0, 120, 212);
    private static readonly Color DefaultDarkAccent = Color.FromArgb(255, 117, 117, 117);

    public static ThemePreviewChromeColors Create(bool isDarkMode, Color? accentColor)
    {
        var accent = accentColor ?? (isDarkMode ? DefaultDarkAccent : DefaultLightAccent);
        return isDarkMode ? CreateDark(accent) : CreateLight(accent);
    }

    private static ThemePreviewChromeColors CreateLight(Color accent) =>
        new(
            AcrylicBackground: Color.FromArgb(175, 252, 252, 252),
            LineStrong: Color.FromArgb(204, 0, 0, 0),
            LineSoft: Color.FromArgb(153, 0, 0, 0),
            AccentStripe: accent);

    private static ThemePreviewChromeColors CreateDark(Color accent) =>
        new(
            AcrylicBackground: Color.FromArgb(140, 56, 56, 56),
            LineStrong: Color.FromArgb(204, 255, 255, 255),
            LineSoft: Color.FromArgb(153, 255, 255, 255),
            AccentStripe: accent);
}
