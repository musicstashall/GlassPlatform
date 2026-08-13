using GlassSettings.Core.Registry;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Services;

/// <summary>Подсветка подстрок главной: жёлтый SystemFillColorCautionBrush (как точки несохранённых).</summary>
public static class AccentSubtitleBrushService
{
    public static Brush ResolveAccent() =>
        AppThemeVisualResolver.ResolveThemeBrush("SystemFillColorCautionBrush");

    public static Brush ResolveSecondary() =>
        GlassSettingsRegistry.ReadAppTheme() == AppThemeMode.System
            ? (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"]
            : AppThemeVisualResolver.ResolveThemeBrush("TextFillColorSecondaryBrush");
}
