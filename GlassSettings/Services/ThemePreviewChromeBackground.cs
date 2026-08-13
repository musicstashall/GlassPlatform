using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace GlassSettings.Services;

/// <summary>AcrylicBrush для chrome-квадратиков превью тем.</summary>
public static class ThemePreviewChromeBackground
{
    public static Brush CreateBrush(Color tintedColor)
    {
        var tintOpacity = tintedColor.A / 255.0;
        var tint = Color.FromArgb(255, tintedColor.R, tintedColor.G, tintedColor.B);
        return new AcrylicBrush
        {
            TintColor = tint,
            TintOpacity = tintOpacity,
            FallbackColor = tintedColor
        };
    }
}
