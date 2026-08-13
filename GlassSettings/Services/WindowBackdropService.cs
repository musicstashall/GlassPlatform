using GlassSettings.Core.Registry;
using Microsoft.UI.Composition.SystemBackdrops;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI;
using Windows.UI;

namespace GlassSettings.Services;

/// <summary>Backdrop окна GlassSettings: Mica, Acrylic, системный fallback.</summary>
public static class WindowBackdropService
{
    public static void ApplyFromRegistry(Window window, Border acrylicLayer) =>
        ApplyMode(GlassSettingsRegistry.ReadWindowBackdrop(), window, acrylicLayer);

    /// <summary>Только SystemBackdrop из реестра (splash / окна без acrylic-layer в дереве).</summary>
    public static void ApplySystemBackdropFromRegistry(Window window) =>
        ApplyMode(GlassSettingsRegistry.ReadWindowBackdrop(), window, acrylicLayer: new Border());

    /// <summary>In-app acrylic для overlay UI (pane, диалоги).</summary>
    public static Brush GetAcrylicInAppChromeBrush() =>
        GetAcrylicInAppFillBrush(AppThemeVisualResolver.ResolveIsDarkMode());

    /// <summary>In-app acrylic с учётом AppTheme из реестра GlassSettings.</summary>
    public static void ApplyAcrylicInAppChromeBackground(Border border)
    {
        border.Background = AppThemeVisualResolver.ResolveThemeResourceBrush("AcrylicInAppFillColorDefaultBrush")
            ?? GetAcrylicInAppFillBrush(AppThemeVisualResolver.ResolveIsDarkMode());
    }

    /// <summary>Чуть плотнее in-app acrylic для зеркала notice поверх контента.</summary>
    public static void ApplyMirrorNoticeChromeBackground(Border border)
    {
        var isDark = AppThemeVisualResolver.ResolveIsDarkMode();
        var colors = ThemePreviewChromePalette.Create(isDark, accentColor: null);
        var background = ScaleAlpha(colors.AcrylicBackground, MirrorNoticeAcrylicAlphaScale);
        border.Background = ThemePreviewChromeBackground.CreateBrush(background);
    }

    private const float MirrorNoticeAcrylicAlphaScale = 0.78f;

    /// <summary>
    /// Фон overlay-pane NavigationView (LeftMinimal): in-app acrylic во всех режимах backdrop окна.
    /// WinUI по умолчанию использует AcrylicInAppFillColorDefaultBrush; прозрачный pane даёт «дырку».
    /// </summary>
    public static Brush GetNavigationOverlayPaneBrush() =>
        AppThemeVisualResolver.ResolveThemeResourceBrush("AcrylicInAppFillColorDefaultBrush")
        ?? GetAcrylicInAppFillBrush(AppThemeVisualResolver.ResolveIsDarkMode());

    public static void ApplyMode(AppWindowBackdropMode mode, Window window, Border acrylicLayer)
    {
        // Без process-wide cache: режим backdrop читается из реестра при каждом Apply.
        switch (mode)
        {
            case AppWindowBackdropMode.Acrylic:
                ApplyAcrylic(window, acrylicLayer);
                break;
            case AppWindowBackdropMode.Mica:
                ApplyMica(window, acrylicLayer, MicaKind.Base);
                break;
            case AppWindowBackdropMode.MicaAlt:
                ApplyMica(window, acrylicLayer, MicaKind.BaseAlt);
                break;
            default:
                ApplySystemDefault(window, acrylicLayer);
                break;
        }
    }

    private static void ApplyAcrylic(Window window, Border acrylicLayer)
    {
        ApplyDesktopAcrylicFallback(window);
        HideAcrylicLayer(acrylicLayer);
    }

    private static void ApplyMica(Window window, Border acrylicLayer, MicaKind kind)
    {
        HideAcrylicLayer(acrylicLayer);

        try
        {
            if (MicaController.IsSupported())
            {
                window.SystemBackdrop = new MicaBackdrop { Kind = kind };
                return;
            }
        }
        catch { /* ignore */ }

        ApplyDesktopAcrylicFallback(window);
    }

    private static void ApplySystemDefault(Window window, Border acrylicLayer)
    {
        HideAcrylicLayer(acrylicLayer);

        try
        {
            if (MicaController.IsSupported())
            {
                window.SystemBackdrop = new MicaBackdrop { Kind = MicaKind.Base };
                return;
            }
        }
        catch { /* ignore */ }

        ApplyDesktopAcrylicFallback(window);
    }

    private static void ApplyDesktopAcrylicFallback(Window window)
    {
        try { window.SystemBackdrop = new DesktopAcrylicBackdrop(); }
        catch { /* оставляем фон по умолчанию */ }
    }

    private static void HideAcrylicLayer(Border acrylicLayer) =>
        acrylicLayer.Visibility = Visibility.Collapsed;

    private static Color ScaleAlpha(Color color, float scale)
    {
        var alpha = (byte)Math.Clamp((int)MathF.Round(color.A * scale), 0, 255);
        return Color.FromArgb(alpha, color.R, color.G, color.B);
    }

    private static Brush GetAcrylicInAppFillBrush(bool isDark)
    {
        if (AppThemeVisualResolver.ResolveThemeResourceBrush("AcrylicInAppFillColorDefaultBrush") is Brush themeBrush)
            return themeBrush;

        return new AcrylicBrush
        {
            TintColor = isDark
                ? Color.FromArgb(255, 0x2C, 0x2C, 0x2C)
                : Color.FromArgb(255, 0xFC, 0xFC, 0xFC),
            TintOpacity = isDark ? 0.15 : 0.0,
            FallbackColor = isDark
                ? Color.FromArgb(255, 0x2C, 0x2C, 0x2C)
                : Color.FromArgb(255, 0xF9, 0xF9, 0xF9)
        };
    }
}
