using System.Runtime.InteropServices;

namespace GlassSettings.Core.Text;

/// <summary>Эталонные шрифты текущей визуальной темы (GetThemeSysFont).</summary>
public static class SystemFontsThemeRef
{
    public static SystemFontsLogFont[] LoadFromTheme(SystemFontsLogFont[] fallbackWhenEmpty)
    {
        var result = new SystemFontsLogFont[SystemFontsCatalog.SlotCount];
        nint theme = SystemFontsNative.OpenThemeData(IntPtr.Zero, "sysmetrics");
        if (theme == 0)
            theme = SystemFontsNative.OpenThemeData(SystemFontsNative.GetDesktopWindow(), "sysmetrics");

        try
        {
            for (var i = 0; i < SystemFontsCatalog.SlotCount; i++)
            {
                if (theme == 0
                    || SystemFontsNative.GetThemeSysFont(theme, SystemFontsCatalog.ThemeMetricIds[i], out var lf) != 0
                    || string.IsNullOrEmpty(lf.lfFaceName))
                {
                    result[i] = fallbackWhenEmpty[i];
                }
                else
                {
                    result[i] = lf;
                }
            }
        }
        finally
        {
            if (theme != 0)
                SystemFontsNative.CloseThemeData(theme);
        }

        return result;
    }
}
