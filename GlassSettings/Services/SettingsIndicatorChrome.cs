using GlassSettings.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace GlassSettings.Services;

/// <summary>Заливка индикаторных точек по severity (как у SettingsUnsavedNoticeStrip).</summary>
public static class SettingsIndicatorChrome
{
    public static Brush ResolveFillBrush(SettingsNoticeSeverity severity) => severity switch
    {
        SettingsNoticeSeverity.Success => AppThemeVisualResolver.ResolveThemeBrush("SystemFillColorSuccessBrush"),
        SettingsNoticeSeverity.Warning => AppThemeVisualResolver.ResolveThemeBrush("SystemFillColorCautionBrush"),
        _ => AppThemeVisualResolver.ResolveThemeBrush("SystemFillColorCriticalBrush")
    };

    /// <summary>Фон подсветки (напр. свёрнутый SettingsDividerExpander с dirty).</summary>
    public static Brush ResolveBackgroundBrush(SettingsNoticeSeverity severity) => severity switch
    {
        SettingsNoticeSeverity.Success => AppThemeVisualResolver.ResolveThemeBrush("SystemFillColorSuccessBackground"),
        SettingsNoticeSeverity.Warning => AppThemeVisualResolver.ResolveThemeBrush("SystemFillColorCautionBackground"),
        _ => AppThemeVisualResolver.ResolveThemeBrush("SystemFillColorCriticalBackground")
    };

    /// <summary>Hover-фон подсветки: для Warning — плотнее тот же caution-цвет.</summary>
    public static Brush ResolveHoverBackgroundBrush(SettingsNoticeSeverity severity)
    {
        var baseBrush = ResolveBackgroundBrush(severity);
        if (baseBrush is SolidColorBrush solid)
        {
            var c = solid.Color;
            var alpha = (byte)Math.Min(255, (int)MathF.Round(c.A * 1.35f));
            return new SolidColorBrush(Color.FromArgb(alpha, c.R, c.G, c.B));
        }

        return baseBrush;
    }
}
