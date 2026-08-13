using GlassSettings.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace GlassSettings.Services;

/// <summary>Базовые alert-цвета Success / Warning / Error для light и dark тем GlassSettings.</summary>
public static class SettingsAlertPalette
{
    private const byte BackgroundAlpha = 0x33;

    private static readonly HashSet<string> AlertResourceKeys = new(StringComparer.Ordinal)
    {
        "SystemFillColorSuccessBrush",
        "SystemFillColorSuccessBackground",
        "SystemFillColorCautionBrush",
        "SystemFillColorCautionBackground",
        "SystemFillColorCriticalBrush",
        "SystemFillColorCriticalBackground",
    };

    public static bool TryResolveBrush(string resourceKey, out Brush brush)
    {
        brush = null!;
        if (!AlertResourceKeys.Contains(resourceKey))
            return false;

        var isDark = AppThemeVisualResolver.ResolveIsDarkMode();
        brush = resourceKey switch
        {
            "SystemFillColorSuccessBrush" => CreateForegroundBrush(isDark, AlertTone.Success),
            "SystemFillColorSuccessBackground" => CreateBackgroundBrush(isDark, AlertTone.Success),
            "SystemFillColorCautionBrush" => CreateForegroundBrush(isDark, AlertTone.Warning),
            "SystemFillColorCautionBackground" => CreateBackgroundBrush(isDark, AlertTone.Warning),
            "SystemFillColorCriticalBrush" => CreateForegroundBrush(isDark, AlertTone.Error),
            "SystemFillColorCriticalBackground" => CreateBackgroundBrush(isDark, AlertTone.Error),
            _ => null!
        };

        return brush is not null;
    }

    public static Brush ResolveForegroundBrush(SettingsNoticeSeverity severity) =>
        CreateForegroundBrush(
            AppThemeVisualResolver.ResolveIsDarkMode(),
            ToTone(severity));

    public static Brush ResolveBackgroundBrush(SettingsNoticeSeverity severity) =>
        CreateBackgroundBrush(
            AppThemeVisualResolver.ResolveIsDarkMode(),
            ToTone(severity));

    private enum AlertTone
    {
        Success,
        Warning,
        Error,
    }

    private static AlertTone ToTone(SettingsNoticeSeverity severity) => severity switch
    {
        SettingsNoticeSeverity.Success => AlertTone.Success,
        SettingsNoticeSeverity.Warning => AlertTone.Warning,
        _ => AlertTone.Error,
    };

    private static SolidColorBrush CreateForegroundBrush(bool isDark, AlertTone tone)
    {
        var (r, g, b) = ResolveRgb(isDark, tone);
        return new SolidColorBrush(Color.FromArgb(255, r, g, b));
    }

    private static SolidColorBrush CreateBackgroundBrush(bool isDark, AlertTone tone)
    {
        var (r, g, b) = ResolveRgb(isDark, tone);
        return new SolidColorBrush(Color.FromArgb(BackgroundAlpha, r, g, b));
    }

    private static (byte R, byte G, byte B) ResolveRgb(bool isDark, AlertTone tone) =>
        (isDark, tone) switch
        {
            (true, AlertTone.Success) => (0x4A, 0x9E, 0x42),
            (false, AlertTone.Success) => (0x14, 0x80, 0x18),
            (true, AlertTone.Warning) => (0xC4, 0x92, 0x2E),
            (false, AlertTone.Warning) => (0xA6, 0x6A, 0x08),
            (true, AlertTone.Error) => (0xFF, 0x99, 0xA4),
            (false, AlertTone.Error) => (0xD6, 0x38, 0x28),
            _ => (0, 0, 0),
        };
}
