using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

/// <summary>Пороги ширины inline-шапки спойлеров.</summary>
internal static class SpoilerHeaderLayoutMetrics
{
    internal const double IconRail = 44;
    internal const double HeaderPadding = 24;
    internal const double ColumnGap = 8;
    private const double DefaultTitleControlsGap = 10;
    private const double DefaultTrailingToggleRail = 96;

    public static double TitleMinWidth
    {
        get
        {
            if (Application.Current.Resources["GlassSettingsSectionTitleMinWidth"] is double value)
                return value;
            return 150;
        }
    }

    public static double SliderMinWidth => GlassSettingsSliderMetrics.TrackMinWidth;

    public static double SliderMaxWidth => GlassSettingsSliderMetrics.TrackMaxWidth;

    public static double SliderSuffixPanelMinWidth => GlassSettingsSliderMetrics.SuffixPanelMinWidth;

    public static double SliderSuffixPanelMaxWidth => GlassSettingsSliderMetrics.SuffixPanelMaxWidth;

    public static double TrailingToggleRail
    {
        get
        {
            if (Application.Current.Resources["GlassSettingsSpoilerTrailingToggleRail"] is double value)
                return value;
            return DefaultTrailingToggleRail;
        }
    }

    /// <summary>Зазор между title/description и таблицей контролов (TitlePanel Margin right).</summary>
    public static double TitleControlsGap =>
        Application.Current.Resources["GlassSettingsTitleControlsGap"] is double value
            ? value
            : DefaultTitleControlsGap;

    /// <summary>Отвязывает элемент от Panel или ContentControl перед повторным назначением parent.</summary>
    public static void DetachFromVisualParent(object? content)
    {
        if (content is not FrameworkElement element)
            return;

        if (element.Parent is Panel panel)
            panel.Children.Remove(element);
        else if (element.Parent is ContentControl host && ReferenceEquals(host.Content, element))
            host.Content = null;
    }
}
