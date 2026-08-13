using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Controls;

public enum SettingsProgressActionMode
{
    Download,
    Cancel,
}

public enum SettingsProgressVisualState
{
    Idle,
    Downloading,
    Success,
    Error,
}

public sealed partial class SettingsProgressActionRow : UserControl
{
    public static readonly DependencyProperty TitleProperty =
        DependencyProperty.Register(
            nameof(Title),
            typeof(string),
            typeof(SettingsProgressActionRow),
            new PropertyMetadata(string.Empty, OnTitleChanged));

    public static readonly DependencyProperty DescriptionProperty =
        DependencyProperty.Register(
            nameof(Description),
            typeof(string),
            typeof(SettingsProgressActionRow),
            new PropertyMetadata(string.Empty, OnDescriptionChanged));

    public static readonly DependencyProperty IconGlyphProperty =
        DependencyProperty.Register(
            nameof(IconGlyph),
            typeof(string),
            typeof(SettingsProgressActionRow),
            new PropertyMetadata(string.Empty, OnIconGlyphChanged));

    private readonly TextBlock _percentText;
    private readonly Button _downloadButton;
    private readonly Button _cancelButton;
    private readonly SpoilerHeaderLayoutHost _layoutHost = new();
    private readonly object _headerStaticContent;
    private object? _headerProgressContent;
    private SettingsProgressVisualState _visualState = SettingsProgressVisualState.Idle;
    private int? _lastPercent;
    private string? _lastPdbFileName;

    public event EventHandler<RoutedEventArgs>? DownloadClick;
    public event EventHandler<RoutedEventArgs>? CancelClick;

    public SettingsProgressActionRow()
    {
        InitializeComponent();

        _percentText = new TextBlock
        {
            VerticalAlignment = VerticalAlignment.Center,
            Style = Application.Current.Resources["GlassSettingsSpoilerSecondaryTextStyle"] as Style,
            Visibility = Visibility.Collapsed,
            TextAlignment = TextAlignment.Right,
            TextTrimming = TextTrimming.CharacterEllipsis
        };

        _downloadButton = new Button();
        _downloadButton.Click += DownloadButton_OnClick;

        _cancelButton = new Button { Visibility = Visibility.Collapsed };
        _cancelButton.Click += CancelButton_OnClick;

        var buttonRail = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 12,
            VerticalAlignment = VerticalAlignment.Center
        };
        buttonRail.Children.Add(_downloadButton);
        buttonRail.Children.Add(_cancelButton);
        _headerStaticContent = buttonRail;

        // Нет expand/navigate — card PointerOver не показываем.
        RootBorder.SizeChanged += (_, _) => ApplyProgressFillWidth();

        Loaded += (_, _) =>
        {
            _layoutHost.Attach(
                RootBorder,
                HeaderChrome,
                ActionsBelowHost,
                SpoilerHeaderRowLayoutBridge.CreateOptions(HeaderChrome, hasGateRail: false));
            ApplyHeaderControls();
            ApplyProgressFillWidth();
        };

        Unloaded += (_, _) => _layoutHost.Detach();
    }

    public string Title
    {
        get => (string)GetValue(TitleProperty);
        set => SetValue(TitleProperty, value);
    }

    public string Description
    {
        get => (string)GetValue(DescriptionProperty);
        set => SetValue(DescriptionProperty, value);
    }

    public string IconGlyph
    {
        get => (string)GetValue(IconGlyphProperty);
        set => SetValue(IconGlyphProperty, value);
    }

    public string DownloadLabel
    {
        get => _downloadButton.Content as string ?? string.Empty;
        set => _downloadButton.Content = value;
    }

    public string CancelLabel
    {
        get => _cancelButton.Content as string ?? string.Empty;
        set => _cancelButton.Content = value;
    }

    public void SetActionMode(SettingsProgressActionMode mode)
    {
        _downloadButton.Visibility = mode == SettingsProgressActionMode.Download
            ? Visibility.Visible
            : Visibility.Collapsed;
        _cancelButton.Visibility = mode == SettingsProgressActionMode.Cancel
            ? Visibility.Visible
            : Visibility.Collapsed;
    }

    public void SetActionButtonsEnabled(bool enabled)
    {
        _downloadButton.IsEnabled = enabled;
        _cancelButton.IsEnabled = enabled;
    }

    public void SetProgress(int? percent, SettingsProgressVisualState state, string? pdbFileName = null)
    {
        var previousProgressContent = _headerProgressContent;

        _visualState = state;
        _lastPercent = percent;
        if (pdbFileName is not null)
            _lastPdbFileName = pdbFileName;

        switch (state)
        {
            case SettingsProgressVisualState.Idle:
                ProgressFill.Visibility = Visibility.Collapsed;
                _percentText.Visibility = Visibility.Collapsed;
                _percentText.Text = string.Empty;
                _lastPdbFileName = null;
                _headerProgressContent = null;
                break;

            case SettingsProgressVisualState.Downloading:
                ProgressFill.Visibility = Visibility.Visible;
                ProgressFill.Background = AppThemeVisualResolver.ResolveThemeBrush("SystemFillColorSuccessBackground");
                _percentText.Visibility = Visibility.Visible;
                _percentText.Text = FormatProgressText(percent, pdbFileName ?? _lastPdbFileName, includeFileName: true);
                _headerProgressContent = _percentText;
                break;

            case SettingsProgressVisualState.Success:
                ProgressFill.Visibility = Visibility.Visible;
                ProgressFill.Background = AppThemeVisualResolver.ResolveThemeBrush("SystemFillColorSuccessBackground");
                _percentText.Visibility = Visibility.Visible;
                _percentText.Text = FormatProgressText(100, null, includeFileName: false);
                _headerProgressContent = _percentText;
                break;

            case SettingsProgressVisualState.Error:
                ProgressFill.Visibility = Visibility.Visible;
                ProgressFill.Background = AppThemeVisualResolver.ResolveThemeBrush("SystemFillColorCriticalBackground");
                _percentText.Visibility = Visibility.Visible;
                _percentText.Text = FormatProgressText(percent, null, includeFileName: false);
                _headerProgressContent = _percentText;
                break;
        }

        if (!ReferenceEquals(previousProgressContent, _headerProgressContent))
            ApplyHeaderControls();
        else if (_headerProgressContent is not null)
            _layoutHost.Invalidate();

        ApplyProgressFillWidth();
    }

    public void RefreshLayout() => _layoutHost.Invalidate();

    private static string FormatProgressText(int? percent, string? pdbFileName, bool includeFileName)
    {
        if (percent is >= 0 and <= 100
            && includeFileName
            && !string.IsNullOrWhiteSpace(pdbFileName))
        {
            return GlassSettingsResources.Format("Dwm_System_SymbolsDownloading", pdbFileName, percent.Value);
        }

        return percent is >= 0 and <= 100
            ? $"{percent.Value}%"
            : "…";
    }

    private void ApplyProgressFillWidth()
    {
        if (_visualState == SettingsProgressVisualState.Idle)
        {
            ProgressFill.Width = 0;
            return;
        }

        var hostWidth = RootBorder.ActualWidth;
        if (hostWidth <= 0)
            return;

        var percent = _lastPercent is >= 0 and <= 100 ? _lastPercent.Value : 0;
        if (_visualState == SettingsProgressVisualState.Success)
            percent = 100;

        ProgressFill.Width = hostWidth * percent / 100.0;
    }

    private void ApplyHeaderControls()
    {
        if (!IsLoaded)
            return;

        SpoilerHeaderRowLayoutBridge.ApplyHeaderControlsWithoutGate(
            HeaderChrome,
            _layoutHost,
            _headerStaticContent,
            _headerProgressContent);
    }

    private static void OnTitleChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsProgressActionRow row)
            row.HeaderChrome.Title = e.NewValue as string ?? string.Empty;
    }

    private static void OnDescriptionChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsProgressActionRow row)
            row.HeaderChrome.Description = e.NewValue as string ?? string.Empty;
    }

    private static void OnIconGlyphChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsProgressActionRow row)
            row.HeaderChrome.IconGlyph = e.NewValue as string ?? string.Empty;
    }

    private void DownloadButton_OnClick(object sender, RoutedEventArgs e) =>
        DownloadClick?.Invoke(this, e);

    private void CancelButton_OnClick(object sender, RoutedEventArgs e) =>
        CancelClick?.Invoke(this, e);
}
