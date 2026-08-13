using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Services;

/// <summary>
/// Chrome слои окна: solid bootstrap (legacy) или transparent под SystemBackdrop.
/// Title bar — только в TitleBarThemeHelper.
/// </summary>
internal static class StartupChromeService
{
    /// <summary>
    /// Непрозрачный фон до применения SystemBackdrop — без «дыры» в desktop на первом кадре.
    /// </summary>
    public static void ApplySolidBootstrapLayers(
        Grid rootGrid,
        Grid blurHost,
        NavigationView? navView = null,
        Frame? navFrame = null)
    {
        var solid = ResolveSolidBaseBrush();
        rootGrid.Background = solid;
        blurHost.Background = solid;

        try
        {
            if (navView is not null)
                navView.Background = solid;
            if (navFrame is not null)
                navFrame.Background = solid;
        }
        catch { /* ignore */ }
    }

    /// <summary>
    /// Transparent под уже применённым SystemBackdrop (single-window splash на Main).
    /// LoadingOverlay — transparent, чтобы acrylic/Mica просвечивал; иконка поверх.
    /// </summary>
    public static void ApplyTransparentLayers(
        Grid rootGrid,
        Grid blurHost,
        Grid? loadingOverlay = null,
        NavigationView? navView = null,
        Frame? navFrame = null)
    {
        var transparent = new SolidColorBrush(Colors.Transparent);
        rootGrid.Background = transparent;
        blurHost.Background = transparent;
        if (loadingOverlay is not null)
            loadingOverlay.Background = transparent;

        try
        {
            if (navView is not null)
                navView.Background = transparent;
            if (navFrame is not null)
                navFrame.Background = transparent;
        }
        catch { /* ignore */ }
    }

    private static Brush ResolveSolidBaseBrush()
    {
        if (Application.Current.Resources.TryGetValue(
                "SolidBackgroundFillColorBaseBrush", out var value)
            && value is Brush brush)
        {
            return brush;
        }

        return new SolidColorBrush(Colors.Black);
    }
}
