using Microsoft.UI.Xaml;

namespace GlassSettings.Services;

/// <summary>Метрики notice-полосы — синхрон с <c>GlassSettingsNoticeStripMinHeight</c> в App.xaml.</summary>
internal static class GlassSettingsNoticeMetrics
{
    private const string MinHeightResourceKey = "GlassSettingsNoticeStripMinHeight";
    private const double FallbackMinHeight = 52;

    public static double StripMinHeight =>
        Application.Current?.Resources[MinHeightResourceKey] is double value
            ? value
            : FallbackMinHeight;
}
