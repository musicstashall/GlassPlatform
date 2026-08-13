using GlassSettings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Controls;

public sealed partial class UnsavedIndicatorDot : UserControl
{
    public static readonly DependencyProperty SeverityProperty =
        DependencyProperty.Register(
            nameof(Severity),
            typeof(SettingsNoticeSeverity),
            typeof(UnsavedIndicatorDot),
            new PropertyMetadata(SettingsNoticeSeverity.Warning, (d, _) => ((UnsavedIndicatorDot)d).ApplyChrome()));

    public UnsavedIndicatorDot()
    {
        InitializeComponent();
        Loaded += (_, _) => ApplyChrome();
        ActualThemeChanged += (_, _) => ApplyChrome();
    }

    public SettingsNoticeSeverity Severity
    {
        get => (SettingsNoticeSeverity)GetValue(SeverityProperty);
        set => SetValue(SeverityProperty, value);
    }

    private void ApplyChrome() =>
        DotShape.Fill = SettingsIndicatorChrome.ResolveFillBrush(Severity);

    /// <summary>Перечитывает заливку после смены темы.</summary>
    public void RefreshChrome() => ApplyChrome();
}
