using Microsoft.UI.Xaml;

namespace GlassSettings.Controls;

/// <summary>Единые min/max ширины трека слайдера (120–200) для шапки и body спойлеров.</summary>
public static class GlassSettingsSliderMetrics
{
    private const double DefaultTrackMinWidth = 120;
    private const double DefaultTrackMaxWidth = 200;
    private const double DefaultSuffixReserve = 48;

    public static double TrackMinWidth =>
        ReadResource("GlassSettingsSliderMinWidth", DefaultTrackMinWidth);

    public static double TrackMaxWidth =>
        ReadResource("GlassSettingsSliderMaxWidth", DefaultTrackMaxWidth);

    public static double SuffixReserve =>
        ReadResource("GlassSettingsSliderSuffixReserve", DefaultSuffixReserve);

    /// <summary>Панель slider+suffix в шапке / body: min = track min + suffix.</summary>
    public static double SuffixPanelMinWidth =>
        ReadResource("GlassSettingsSliderHeaderPanelMinWidth", TrackMinWidth + SuffixReserve);

    public static double SuffixPanelMaxWidth =>
        ReadResource("GlassSettingsSliderHeaderPanelMaxWidth", TrackMaxWidth + SuffixReserve);

    public static double ClampTrack(double value) =>
        Math.Clamp(value, TrackMinWidth, TrackMaxWidth);

    private static double ReadResource(string key, double fallback) =>
        Application.Current.Resources[key] is double value ? value : fallback;
}
