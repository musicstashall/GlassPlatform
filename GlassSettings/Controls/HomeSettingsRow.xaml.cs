using GlassSettings.Core.Snapshot;
using GlassSettings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace GlassSettings.Controls;

public sealed partial class HomeSettingsRow : UserControl, ISettingsUnsavedScopeHost
{
    public static readonly DependencyProperty TitleProperty =
        DependencyProperty.Register(nameof(Title), typeof(string), typeof(HomeSettingsRow),
            new PropertyMetadata(string.Empty, OnTextChanged));

    public static readonly DependencyProperty SubtitleProperty =
        DependencyProperty.Register(nameof(Subtitle), typeof(string), typeof(HomeSettingsRow),
            new PropertyMetadata(string.Empty, OnTextChanged));

    public static readonly DependencyProperty AccentSubtitleProperty =
        DependencyProperty.Register(nameof(AccentSubtitle), typeof(string), typeof(HomeSettingsRow),
            new PropertyMetadata(string.Empty, OnTextChanged));

    public static readonly DependencyProperty IconGlyphCharProperty =
        DependencyProperty.Register(nameof(IconGlyphChar), typeof(string), typeof(HomeSettingsRow),
            new PropertyMetadata(string.Empty, OnIconChanged));

    public static readonly DependencyProperty ShowChevronProperty =
        DependencyProperty.Register(nameof(ShowChevron), typeof(bool), typeof(HomeSettingsRow),
            new PropertyMetadata(true, (d, _) => ((HomeSettingsRow)d).UpdateChrome()));

    public static readonly DependencyProperty ShowDividerProperty =
        DependencyProperty.Register(nameof(ShowDivider), typeof(bool), typeof(HomeSettingsRow),
            new PropertyMetadata(true, (d, _) => ((HomeSettingsRow)d).UpdateChrome()));

    public static readonly DependencyProperty CanNavigateProperty =
        DependencyProperty.Register(nameof(CanNavigate), typeof(bool), typeof(HomeSettingsRow),
            new PropertyMetadata(true, (d, _) => ((HomeSettingsRow)d).UpdateVisualState()));

    public static readonly DependencyProperty IsCustomizationHighlightedProperty =
        DependencyProperty.Register(nameof(IsCustomizationHighlighted), typeof(bool), typeof(HomeSettingsRow),
            new PropertyMetadata(false, (d, _) => ((HomeSettingsRow)d).UpdateText()));

    public static readonly DependencyProperty IsUnsavedPendingProperty =
        DependencyProperty.Register(nameof(IsUnsavedPending), typeof(bool), typeof(HomeSettingsRow),
            new PropertyMetadata(false, (d, _) => ((HomeSettingsRow)d).UpdateUnsavedDot()));

    public static readonly DependencyProperty StatusIndicatorColorProperty =
        DependencyProperty.Register(nameof(StatusIndicatorColor), typeof(Color?), typeof(HomeSettingsRow),
            new PropertyMetadata(null, (d, _) => ((HomeSettingsRow)d).UpdateStatusIndicatorDot()));

    public static readonly DependencyProperty UnsavedScopeProperty =
        DependencyProperty.Register(nameof(UnsavedScope), typeof(SettingsCommitScope?), typeof(HomeSettingsRow),
            new PropertyMetadata(null, (d, _) => ((HomeSettingsRow)d).RefreshUnsavedIndicator()));

    private uint? _activePointerId;

    public event EventHandler? NavigateRequested;

    public HomeSettingsRow()
    {
        InitializeComponent();
        RowBorder.PointerPressed += RowBorder_OnPointerPressed;
        RowBorder.PointerReleased += RowBorder_OnPointerReleased;
        RowBorder.PointerCanceled += RowBorder_OnPointerCanceled;
        RowBorder.PointerEntered += RowBorder_OnPointerEntered;
        RowBorder.PointerExited += RowBorder_OnPointerExited;
        Loaded += (_, _) =>
        {
            UpdateText();
            UpdateChrome();
            UpdateUnsavedDot();
            UpdateStatusIndicatorDot();
            UpdateVisualState();
            SettingsSession.Current.DirtyChanged += Session_OnDirtyChanged;
            RefreshUnsavedIndicator();
        };
        Unloaded += (_, _) => SettingsSession.Current.DirtyChanged -= Session_OnDirtyChanged;
    }

    public string Title
    {
        get => (string)GetValue(TitleProperty);
        set => SetValue(TitleProperty, value);
    }

    public string Subtitle
    {
        get => (string)GetValue(SubtitleProperty);
        set => SetValue(SubtitleProperty, value);
    }

    public string AccentSubtitle
    {
        get => (string)GetValue(AccentSubtitleProperty);
        set => SetValue(AccentSubtitleProperty, value);
    }

    public string IconGlyphChar
    {
        get => (string)GetValue(IconGlyphCharProperty);
        set => SetValue(IconGlyphCharProperty, value);
    }

    public bool ShowChevron
    {
        get => (bool)GetValue(ShowChevronProperty);
        set => SetValue(ShowChevronProperty, value);
    }

    public bool ShowDivider
    {
        get => (bool)GetValue(ShowDividerProperty);
        set => SetValue(ShowDividerProperty, value);
    }

    public bool CanNavigate
    {
        get => (bool)GetValue(CanNavigateProperty);
        set => SetValue(CanNavigateProperty, value);
    }

    public bool IsCustomizationHighlighted
    {
        get => (bool)GetValue(IsCustomizationHighlightedProperty);
        set => SetValue(IsCustomizationHighlightedProperty, value);
    }

    public SettingsCommitScope? UnsavedScope
    {
        get => (SettingsCommitScope?)GetValue(UnsavedScopeProperty);
        set => SetValue(UnsavedScopeProperty, value);
    }

    public bool IsUnsavedPending
    {
        get => (bool)GetValue(IsUnsavedPendingProperty);
        set => SetValue(IsUnsavedPendingProperty, value);
    }

    public Color? StatusIndicatorColor
    {
        get => (Color?)GetValue(StatusIndicatorColorProperty);
        set => SetValue(StatusIndicatorColorProperty, value);
    }

    public UIElement? TrailingContent
    {
        get => TrailingHost.Content as UIElement;
        set
        {
            TrailingHost.Content = value;
            TrailingHost.Visibility = value is null ? Visibility.Collapsed : Visibility.Visible;
        }
    }

    public void ApplyStatus(string subtitle, string? accentValue, bool highlighted)
    {
        Subtitle = subtitle;
        AccentSubtitle = accentValue ?? string.Empty;
        IsCustomizationHighlighted = highlighted;
        UpdateText();
    }

    /// <summary>Перечитывает accent-brush после смены accent/AppTheme извне.</summary>
    public void RefreshAccentAppearance()
    {
        UpdateText();
        UpdateUnsavedDot();
    }

    public void RefreshUnsavedIndicator()
    {
        IsUnsavedPending = UnsavedScope is { } scope && SettingsSession.Current.HasUnsavedInScope(scope);
    }

    private void Session_OnDirtyChanged(object? sender, EventArgs e) =>
        RefreshUnsavedIndicator();

    private static void OnTextChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is HomeSettingsRow row)
            row.UpdateText();
    }

    private static void OnIconChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is HomeSettingsRow row)
            row.UpdateIcon();
    }

    private void UpdateIcon()
    {
        var g = IconGlyphChar;
        if (string.IsNullOrEmpty(g))
        {
            IconGlyph.Visibility = Visibility.Collapsed;
            return;
        }

        IconGlyph.Glyph = g;
        IconGlyph.Visibility = Visibility.Visible;
    }

    private void UpdateText()
    {
        TitleText.Text = Title;
        var accent = AccentSubtitle;
        if (!string.IsNullOrEmpty(accent))
        {
            SubtitlePrefix.Text = Subtitle;
            SubtitlePrefix.Visibility = string.IsNullOrEmpty(Subtitle) ? Visibility.Collapsed : Visibility.Visible;
            SubtitleAccent.Text = accent;
            SubtitleAccent.Foreground = AccentSubtitleBrushService.ResolveAccent();
            SubtitleAccent.Visibility = Visibility.Visible;
            SubtitlePlain.Visibility = Visibility.Collapsed;
            return;
        }

        SubtitlePrefix.Visibility = Visibility.Collapsed;
        SubtitleAccent.Visibility = Visibility.Collapsed;
        SubtitlePlain.Text = Subtitle;
        SubtitlePlain.Visibility = string.IsNullOrEmpty(Subtitle) ? Visibility.Collapsed : Visibility.Visible;
        SubtitlePlain.Foreground = IsCustomizationHighlighted && !string.IsNullOrEmpty(Subtitle)
            ? AccentSubtitleBrushService.ResolveAccent()
            : AccentSubtitleBrushService.ResolveSecondary();
    }

    private void UpdateChrome()
    {
        ChevronIcon.Visibility = ShowChevron ? Visibility.Visible : Visibility.Collapsed;
        DividerHost.Visibility = ShowDivider ? Visibility.Visible : Visibility.Collapsed;
    }

    private void UpdateUnsavedDot()
    {
        UnsavedDot.Visibility = IsUnsavedPending ? Visibility.Visible : Visibility.Collapsed;
        if (IsUnsavedPending)
            UnsavedDot.Fill = SettingsIndicatorChrome.ResolveFillBrush(SettingsNoticeSeverity.Warning);
    }

    private void UpdateStatusIndicatorDot()
    {
        if (StatusIndicatorColor is not { } color)
        {
            StatusIndicatorDot.Visibility = Visibility.Collapsed;
            return;
        }

        StatusIndicatorDot.Fill = new SolidColorBrush(color);
        StatusIndicatorDot.Visibility = Visibility.Visible;
    }

    private void UpdateVisualState()
    {
        var navigable = CanNavigate && ShowChevron;
        ChevronIcon.Opacity = navigable ? 1.0 : 0.4;
        RowBorder.Opacity = navigable || !ShowChevron ? 1.0 : 0.55;
        if (!navigable)
            RowBorder.Background = null;
    }

    private void RowBorder_OnPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        if (!CanNavigate || !ShowChevron || IsInteractiveSource(e.OriginalSource as DependencyObject))
            return;
        _activePointerId = e.Pointer.PointerId;
        RowBorder.Background = Application.Current.Resources["SubtleFillColorSecondaryBrush"] as Brush
            ?? RowBorder.Background;
    }

    private void RowBorder_OnPointerReleased(object sender, PointerRoutedEventArgs e)
    {
        if (_activePointerId != e.Pointer.PointerId)
            return;
        _activePointerId = null;
        UpdateVisualState();

        if (!CanNavigate || !ShowChevron || IsInteractiveSource(e.OriginalSource as DependencyObject))
            return;

        var pt = e.GetCurrentPoint(RowBorder);
        if (pt.Position.X >= 0 && pt.Position.Y >= 0 &&
            pt.Position.X <= RowBorder.ActualWidth &&
            pt.Position.Y <= RowBorder.ActualHeight)
            NavigateRequested?.Invoke(this, EventArgs.Empty);
    }

    private void RowBorder_OnPointerEntered(object sender, PointerRoutedEventArgs e)
    {
        if (CanNavigate && ShowChevron && !_activePointerId.HasValue)
            RowBorder.Background = Application.Current.Resources["SubtleFillColorSecondaryBrush"] as Brush
                ?? RowBorder.Background;
    }

    private void RowBorder_OnPointerCanceled(object sender, PointerRoutedEventArgs e)
    {
        if (_activePointerId == e.Pointer.PointerId)
            _activePointerId = null;
        UpdateVisualState();
    }

    private void RowBorder_OnPointerExited(object sender, PointerRoutedEventArgs e)
    {
        if (_activePointerId == e.Pointer.PointerId)
            return;
        if (CanNavigate && ShowChevron)
            RowBorder.Background = null;
    }

    private bool IsInteractiveSource(DependencyObject? src)
    {
        while (src is not null)
        {
            if (ReferenceEquals(src, TrailingHost))
                return true;
            if (src is ToggleSwitch or Button or ComboBox or Slider or GlassSettingsLabeledToggleSwitch)
                return true;
            src = VisualTreeHelper.GetParent(src);
        }
        return false;
    }
}
