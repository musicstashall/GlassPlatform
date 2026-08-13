using GlassSettings.Strings;
using Microsoft.UI.Xaml.Media;
using Windows.UI.Text;

namespace GlassSettings.Core.Text;

/// <summary>Коллекция экранных шрифтов и начертаний (аналог CF_SCREENFONTS + ChooseFont).</summary>
public static class ScreenFontCatalog
{
    private static readonly byte[] CommonCharsetBytes = [0, 1, 2, 128, 129, 134, 136, 204, 162, 186];

    private static IReadOnlyList<ScreenFontFamilyEntry>? _cachedFamilies;

    public sealed class ScreenFontFamilyEntry
    {
        public required string FaceName { get; init; }

        public FontFamily PreviewFamily { get; init; } = new("Segoe UI");
    }

    public sealed class ScreenFontStyleEntry
    {
        public required string DisplayName { get; init; }

        public int Weight { get; init; }

        public bool Italic { get; init; }

        public FontFamily PreviewFamily { get; init; } = new("Segoe UI");

        public FontWeight PreviewWeight { get; init; } = new() { Weight = 400 };

        public FontStyle PreviewStyle { get; init; } = FontStyle.Normal;
    }

    public sealed class ScreenFontCharsetEntry
    {
        public required string DisplayName { get; init; }

        public byte CharSet { get; init; }
    }

    public static ScreenFontFamilyEntry CreateFamilyEntry(string faceName) =>
        new()
        {
            FaceName = faceName,
            PreviewFamily = CreatePreviewFamily(faceName)
        };

    public static IReadOnlyList<ScreenFontFamilyEntry> GetScreenFamilies()
    {
        if (_cachedFamilies is not null)
            return _cachedFamilies;

        var names = new SortedSet<string>(StringComparer.OrdinalIgnoreCase);
        var hdc = SystemFontsNative.CreateDCW("DISPLAY", null, null, 0);
        if (hdc == 0)
        {
            _cachedFamilies = [new ScreenFontFamilyEntry { FaceName = "Segoe UI" }];
            return _cachedFamilies;
        }

        try
        {
            var probe = SystemFontsLogFont.Default;
            probe.lfCharSet = 1; // DEFAULT_CHARSET
            SystemFontsNative.EnumFontFamiliesExW(
                hdc,
                ref probe,
                (ref SystemFontsNative.EnumLogFontExW elfe, ref SystemFontsNative.TextMetricW metric, uint fontType, nint _) =>
                {
                    if ((fontType & 0x0004) == 0) // TRUETYPE
                        return SystemFontsNative.FontenumProcContinue;
                    if ((metric.tmPitchAndFamily & 0x80) != 0) // TMPF_VERTICAL
                        return SystemFontsNative.FontenumProcContinue;
                    if (string.IsNullOrWhiteSpace(elfe.elfLogFont.lfFaceName))
                        return SystemFontsNative.FontenumProcContinue;

                    names.Add(elfe.elfLogFont.lfFaceName.Trim());
                    return SystemFontsNative.FontenumProcContinue;
                },
                0,
                0);
        }
        finally
        {
            SystemFontsNative.DeleteDC(hdc);
        }

        _cachedFamilies = names
            .Select(static name => new ScreenFontFamilyEntry
            {
                FaceName = name,
                PreviewFamily = CreatePreviewFamily(name)
            })
            .ToList();

        return _cachedFamilies;
    }

    public static string GetLocalizedStyleName(in SystemFontsLogFont font) =>
        FormatLocalizedStyleName(font.lfWeight, font.lfItalic != 0);

    public static IReadOnlyList<ScreenFontStyleEntry> GetStylesForFace(string faceName, byte charSet = 1)
    {
        var styles = new List<ScreenFontStyleEntry>();
        var hdc = SystemFontsNative.CreateDCW("DISPLAY", null, null, 0);
        if (hdc == 0)
        {
            styles.Add(CreateStyleEntry(faceName, 400, false));
            return styles;
        }

        try
        {
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var probeCharSet in new[] { (byte)1, (byte)0, charSet }.Distinct())
                EnumStylesForFace(hdc, styles, seen, faceName, probeCharSet);
        }
        finally
        {
            SystemFontsNative.DeleteDC(hdc);
        }

        if (styles.Count == 0)
        {
            styles.Add(CreateStyleEntry(faceName, 400, false));
            return styles;
        }

        SortStyles(styles);
        return styles;
    }

    private static void EnumStylesForFace(
        nint hdc,
        List<ScreenFontStyleEntry> styles,
        HashSet<string> seen,
        string faceName,
        byte charSet)
    {
        var probe = SystemFontsLogFont.Default;
        probe.lfFaceName = faceName;
        probe.lfCharSet = charSet;

        SystemFontsNative.EnumFontFamiliesExW(
            hdc,
            ref probe,
            (ref SystemFontsNative.EnumLogFontExW elfe, ref SystemFontsNative.TextMetricW _, uint _, nint _) =>
            {
                if (!string.Equals(elfe.elfLogFont.lfFaceName, faceName, StringComparison.OrdinalIgnoreCase))
                    return SystemFontsNative.FontenumProcContinue;

                var weight = elfe.elfLogFont.lfWeight;
                var italic = elfe.elfLogFont.lfItalic != 0;
                var styleKey = BuildStyleKey(elfe.elfStyle, weight, italic);
                if (!seen.Add(styleKey))
                    return SystemFontsNative.FontenumProcContinue;

                styles.Add(CreateStyleEntry(faceName, weight, italic));
                return SystemFontsNative.FontenumProcContinue;
            },
            0,
            0);
    }

    public static void EnsureStylePresent(List<ScreenFontStyleEntry> styles, string faceName, int weight, bool italic)
    {
        foreach (var style in styles)
        {
            if (style.Weight == weight && style.Italic == italic)
                return;
        }

        styles.Add(CreateStyleEntry(faceName, weight, italic));
        SortStyles(styles);
    }

    public static IReadOnlyList<ScreenFontCharsetEntry> GetCommonCharsets() =>
        CommonCharsetBytes
            .Select(static charSet => new ScreenFontCharsetEntry
            {
                CharSet = charSet,
                DisplayName = GlassSettingsResources.Get($"Text_FontPickerCharset_{charSet}")
            })
            .ToList();

    public static SystemFontsLogFont BuildLogFont(
        string faceName,
        ScreenFontStyleEntry style,
        byte charSet,
        int pointSize,
        in SystemFontsLogFont template)
    {
        var lf = template;
        lf.lfFaceName = faceName;
        lf.lfWeight = style.Weight;
        lf.lfItalic = (byte)(style.Italic ? 1 : 0);
        lf.lfCharSet = charSet;
        lf.lfHeight = SystemFontsModel.PointSizeToLfHeight(pointSize);
        lf.lfWidth = 0;
        lf.lfUnderline = 0;
        lf.lfStrikeOut = 0;
        return lf;
    }

    public static ScreenFontStyleEntry MatchStyle(in SystemFontsLogFont font, IReadOnlyList<ScreenFontStyleEntry> styles)
    {
        foreach (var style in styles)
        {
            if (style.Weight == font.lfWeight && style.Italic == (font.lfItalic != 0))
                return style;
        }

        return styles[0];
    }

    private static string BuildStyleKey(string elfStyle, int weight, bool italic) =>
        string.IsNullOrWhiteSpace(elfStyle)
            ? $"w{weight}:i{(italic ? 1 : 0)}"
            : elfStyle.Trim();

    private static void SortStyles(List<ScreenFontStyleEntry> styles) =>
        styles.Sort(static (a, b) =>
        {
            var weightCompare = a.Weight.CompareTo(b.Weight);
            if (weightCompare != 0)
                return weightCompare;

            return a.Italic.CompareTo(b.Italic);
        });

    private static ScreenFontStyleEntry CreateStyleEntry(string faceName, int weight, bool italic) =>
        new()
        {
            DisplayName = FormatLocalizedStyleName(weight, italic),
            Weight = weight,
            Italic = italic,
            PreviewFamily = CreatePreviewFamily(faceName),
            PreviewWeight = ToPreviewWeight(weight),
            PreviewStyle = italic ? FontStyle.Italic : FontStyle.Normal
        };

    private static FontFamily CreatePreviewFamily(string faceName)
    {
        try
        {
            return new FontFamily(faceName);
        }
        catch
        {
            return new FontFamily("Segoe UI");
        }
    }

    private static FontWeight ToPreviewWeight(int weight) =>
        new() { Weight = (ushort)Math.Clamp(weight, 1, 999) };

    private static string FormatLocalizedStyleName(int weight, bool italic)
    {
        var baseKey = weight switch
        {
            >= 700 => "Text_FontPickerStyle_Bold",
            >= 600 => "Text_FontPickerStyle_Semibold",
            >= 500 => "Text_FontPickerStyle_Medium",
            >= 300 => "Text_FontPickerStyle_Light",
            _ => "Text_FontPickerStyle_Regular"
        };

        if (!italic)
            return GlassSettingsResources.Get(baseKey);

        if (baseKey == "Text_FontPickerStyle_Regular")
            return GlassSettingsResources.Get("Text_FontPickerStyle_Italic");

        var italicKey = baseKey switch
        {
            "Text_FontPickerStyle_Bold" => "Text_FontPickerStyle_BoldItalic",
            "Text_FontPickerStyle_Semibold" => "Text_FontPickerStyle_SemiboldItalic",
            "Text_FontPickerStyle_Medium" => "Text_FontPickerStyle_MediumItalic",
            "Text_FontPickerStyle_Light" => "Text_FontPickerStyle_LightItalic",
            _ => "Text_FontPickerStyle_Italic"
        };

        return GlassSettingsResources.Get(italicKey);
    }
}
