using GlassSettings.Core.Snapshot;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Controls;

/// <summary>
/// Сегмент DWM/XAML (или Общие/Раздельные): одна unsaved-точка перед группой —
/// только при изменении BorderDrawMode / scope-ключа, не параметров внутри секции.
/// </summary>
public sealed partial class PopupScopeSegmentBar : UserControl
{
    private bool _suppress;

    public static readonly DependencyProperty IsSeparateScopeProperty =
        DependencyProperty.Register(
            nameof(IsSeparateScope),
            typeof(bool),
            typeof(PopupScopeSegmentBar),
            new PropertyMetadata(false, OnIsSeparateScopeChanged));

    public static readonly DependencyProperty SharedLabelResourceKeyProperty =
        DependencyProperty.Register(
            nameof(SharedLabelResourceKey),
            typeof(string),
            typeof(PopupScopeSegmentBar),
            new PropertyMetadata("Popup_ScopeShared", OnLabelResourceKeysChanged));

    public static readonly DependencyProperty SeparateLabelResourceKeyProperty =
        DependencyProperty.Register(
            nameof(SeparateLabelResourceKey),
            typeof(string),
            typeof(PopupScopeSegmentBar),
            new PropertyMetadata("Popup_ScopeSeparate", OnLabelResourceKeysChanged));

    public static readonly DependencyProperty PathUnsavedProperty =
        DependencyProperty.Register(
            nameof(PathUnsaved),
            typeof(bool),
            typeof(PopupScopeSegmentBar),
            new PropertyMetadata(false, OnPathUnsavedChanged));

    public event EventHandler<RoutedEventArgs>? ScopeChanged;

    public PopupScopeSegmentBar()
    {
        InitializeComponent();
        ApplyLocalizedStrings();
        UpdateVisualState();
        ApplyPathUnsavedDot();
    }

    public bool IsSeparateScope
    {
        get => (bool)GetValue(IsSeparateScopeProperty);
        set => SetValue(IsSeparateScopeProperty, value);
    }

    public string SharedLabelResourceKey
    {
        get => (string)GetValue(SharedLabelResourceKeyProperty);
        set => SetValue(SharedLabelResourceKeyProperty, value);
    }

    public string SeparateLabelResourceKey
    {
        get => (string)GetValue(SeparateLabelResourceKeyProperty);
        set => SetValue(SeparateLabelResourceKeyProperty, value);
    }

    /// <summary>Несохранённое изменение режима (BorderDrawMode) — точка перед капсулой.</summary>
    public bool PathUnsaved
    {
        get => (bool)GetValue(PathUnsavedProperty);
        set => SetValue(PathUnsavedProperty, value);
    }

    public void ApplyLocalizedStrings()
    {
        SharedLabel.Text = GlassSettingsResources.Get(SharedLabelResourceKey);
        SeparateLabel.Text = GlassSettingsResources.Get(SeparateLabelResourceKey);
    }

    public void SetPathUnsaved(bool unsaved) => PathUnsaved = unsaved;

    private static void OnLabelResourceKeysChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is PopupScopeSegmentBar bar)
            bar.ApplyLocalizedStrings();
    }

    private static void OnIsSeparateScopeChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is PopupScopeSegmentBar bar)
            bar.UpdateVisualState();
    }

    private static void OnPathUnsavedChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is PopupScopeSegmentBar bar)
            bar.ApplyPathUnsavedDot();
    }

    private void ApplyPathUnsavedDot()
    {
        PathUnsavedDot.Visibility = PathUnsaved ? Visibility.Visible : Visibility.Collapsed;
        if (PathUnsaved)
        {
            PathUnsavedDot.Severity = SettingsNoticeSeverity.Warning;
            PathUnsavedDot.RefreshChrome();
        }
    }

    private void SharedButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (_suppress)
            return;

        SetScope(separate: false, e);
    }

    private void SeparateButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (_suppress)
            return;

        SetScope(separate: true, e);
    }

    private void SetScope(bool separate, RoutedEventArgs e)
    {
        if (IsSeparateScope == separate)
            return;

        IsSeparateScope = separate;
        ScopeChanged?.Invoke(this, e);
    }

    private void UpdateVisualState()
    {
        _suppress = true;
        try
        {
            SharedButton.IsChecked = !IsSeparateScope;
            SeparateButton.IsChecked = IsSeparateScope;
            ApplySegmentStyle(SharedButton, SharedLabel, !IsSeparateScope);
            ApplySegmentStyle(SeparateButton, SeparateLabel, IsSeparateScope);
        }
        finally
        {
            _suppress = false;
        }
    }

    private static void ApplySegmentStyle(ToggleButton button, TextBlock label, bool selected)
    {
        if (selected)
        {
            button.Background = (Brush)Application.Current.Resources["AccentFillColorDefaultBrush"];
            var onAccent = (Brush)Application.Current.Resources["TextOnAccentFillColorPrimaryBrush"];
            button.Foreground = onAccent;
            label.Foreground = onAccent;
            return;
        }

        button.Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        var secondary = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"];
        button.Foreground = secondary;
        label.Foreground = secondary;
    }
}
