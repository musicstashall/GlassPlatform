using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

public enum SwcaColorSectionKind { Gradient, Border }

public sealed partial class SwcaColorSection : UserControl
{
    public static readonly DependencyProperty SectionTitleProperty =
        DependencyProperty.Register(
            nameof(SectionTitle),
            typeof(string),
            typeof(SwcaColorSection),
            new PropertyMetadata(string.Empty, OnSectionTitleChanged));

    public static readonly DependencyProperty IconGlyphProperty =
        DependencyProperty.Register(
            nameof(IconGlyph),
            typeof(string),
            typeof(SwcaColorSection),
            new PropertyMetadata(string.Empty, OnIconGlyphChanged));

    private bool _loading;

    public event EventHandler<uint>? ColorChanged;
    public event EventHandler<bool>? EnableChanged;

    public SwcaColorSection()
    {
        InitializeComponent();
        ColorPicker.ColorChanged += (_, argb) =>
        {
            if (!_loading) ColorChanged?.Invoke(this, argb);
        };
        Loaded += (_, _) => ApplyKindIcon();
    }

    public string SectionTitle
    {
        get => (string)GetValue(SectionTitleProperty);
        set => SetValue(SectionTitleProperty, value);
    }

    public string IconGlyph
    {
        get => (string)GetValue(IconGlyphProperty);
        set => SetValue(IconGlyphProperty, value);
    }

    public SwcaColorSectionKind Kind { get; set; }

    public void ShowExpandedForDrill() => RootExpander.IsExpanded = true;

    public void Load(bool enabled, uint argb)
    {
        _loading = true;
        EnableSwitch.IsOn = enabled;
        ColorPicker.IsEnabled = enabled;
        ColorPicker.SetColor(argb);
        _loading = false;
    }

    public new bool IsEnabled => EnableSwitch.IsOn;

    private static void OnSectionTitleChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SwcaColorSection section)
            section.HeaderChrome.Title = e.NewValue as string ?? string.Empty;
    }

    private static void OnIconGlyphChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SwcaColorSection section)
            section.HeaderChrome.IconGlyph = e.NewValue as string ?? string.Empty;
    }

    private void ApplyKindIcon()
    {
        if (!string.IsNullOrEmpty(IconGlyph))
            return;

        IconGlyph = Kind switch
        {
            SwcaColorSectionKind.Gradient => SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.Gradient),
            SwcaColorSectionKind.Border => SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.Border),
            _ => string.Empty
        };
    }

    private void EnableSwitch_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        ColorPicker.IsEnabled = EnableSwitch.IsOn;
        EnableChanged?.Invoke(this, EnableSwitch.IsOn);
    }
}