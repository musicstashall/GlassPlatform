using Windows.UI;

namespace GlassSettings.Services;

/// <summary>
/// Форматы цвета акцента (паритет живого HKCU + DwmGetColorizationColor):
/// <list type="bullet">
/// <item><b>.theme</b> / DWM ColorizationColor / Afterglow — 0xAARRGGBB</item>
/// <item><b>DWM AccentColor</b> / Explorer AccentColorMenu — COLORREF 0xAABBGGRR</item>
/// </list>
/// </summary>
public static class ThemeAccentColorHelper
{
    private const string DesktopSection = @"Control Panel\Desktop";
    private const string VisualStylesSection = "VisualStyles";

    public static Color? TryParseColorizationColor(string themeFilePath)
    {
        if (string.IsNullOrWhiteSpace(themeFilePath) || !File.Exists(themeFilePath))
            return null;

        try
        {
            Color? desktopColor = null;
            Color? visualStylesColor = null;
            var inDesktop = false;
            var inVisualStyles = false;

            foreach (var rawLine in ThemeFileLineReader.ReadLines(themeFilePath))
            {
                var line = rawLine.Trim();
                if (line.Length == 0 || line[0] == ';')
                    continue;

                if (line.StartsWith('[') && line.EndsWith(']'))
                {
                    var section = line[1..^1].Trim();
                    inDesktop = string.Equals(section, DesktopSection, StringComparison.OrdinalIgnoreCase);
                    inVisualStyles = string.Equals(section, VisualStylesSection, StringComparison.OrdinalIgnoreCase);
                    continue;
                }

                if (!inDesktop && !inVisualStyles)
                    continue;

                var separator = line.IndexOf('=');
                if (separator <= 0)
                    continue;

                var key = line[..separator].Trim();
                if (!string.Equals(key, "ColorizationColor", StringComparison.OrdinalIgnoreCase))
                    continue;

                var value = line[(separator + 1)..].Trim();
                var parsed = TryParseThemeFileDword(value);
                if (parsed is null)
                    continue;

                if (inDesktop && desktopColor is null)
                    desktopColor = parsed;
                else if (inVisualStyles && visualStylesColor is null)
                    visualStylesColor = parsed;

                if (desktopColor is not null && visualStylesColor is not null)
                    break;
            }

            return desktopColor ?? visualStylesColor;
        }
        catch
        {
            return null;
        }
    }

    /// <summary>0xAARRGGBB → WinUI Color (.theme / DWM ColorizationColor).</summary>
    public static Color FromArgbDword(uint dword) =>
        Color.FromArgb(
            255,
            (byte)((dword >> 16) & 0xFF),
            (byte)((dword >> 8) & 0xFF),
            (byte)(dword & 0xFF));

    /// <summary>0xAABBGGRR (COLORREF) → WinUI Color (DWM AccentColor / AccentColorMenu).</summary>
    public static Color FromColorRefDword(uint dword) =>
        Color.FromArgb(
            255,
            (byte)(dword & 0xFF),
            (byte)((dword >> 8) & 0xFF),
            (byte)((dword >> 16) & 0xFF));

    /// <summary>WinUI Color → 0xAARRGGBB (.theme / Colorization).</summary>
    public static uint ToArgbDword(Color color, byte alpha = 0xFF) =>
        ((uint)alpha << 24) | ((uint)color.R << 16) | ((uint)color.G << 8) | color.B;

    /// <summary>WinUI Color → 0xAABBGGRR (COLORREF для Accent*).</summary>
    public static uint ToColorRefDword(Color color, byte alpha = 0xFF) =>
        ((uint)alpha << 24) | ((uint)color.B << 16) | ((uint)color.G << 8) | color.R;

    /// <summary>Алиас COLORREF (Accent).</summary>
    public static Color DwordToColor(uint dword) => FromColorRefDword(dword);

    public static Color ToOpaqueForeground(Color color) =>
        Color.FromArgb(255, color.R, color.G, color.B);

    public static bool RgbEquals(Color a, Color b) =>
        a.R == b.R && a.G == b.G && a.B == b.B;

    private static Color? TryParseThemeFileDword(string raw)
    {
        if (string.IsNullOrWhiteSpace(raw))
            return null;

        var text = raw.Trim();
        if (text.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            text = text[2..];

        if (!uint.TryParse(text, System.Globalization.NumberStyles.HexNumber, null, out var dword))
            return null;

        // .theme: 0xAARRGGBB (aero 0xC40078D4 → #0078D4).
        return FromArgbDword(dword);
    }
}
