using System.Collections.Generic;
using GlassSettings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Documents;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;

namespace GlassSettings.Controls;

[ContentProperty(Name = nameof(Trailing))]
public sealed partial class SpoilerHeaderGrid : UserControl
{
    public static readonly DependencyProperty TitleProperty =
        DependencyProperty.Register(
            nameof(Title),
            typeof(string),
            typeof(SpoilerHeaderGrid),
            new PropertyMetadata(string.Empty, OnTitleChanged));

    public static readonly DependencyProperty DescriptionProperty =
        DependencyProperty.Register(
            nameof(Description),
            typeof(string),
            typeof(SpoilerHeaderGrid),
            new PropertyMetadata(string.Empty, OnDescriptionChanged));

    public static readonly DependencyProperty TrailingProperty =
        DependencyProperty.Register(
            nameof(Trailing),
            typeof(object),
            typeof(SpoilerHeaderGrid),
            new PropertyMetadata(null, OnTrailingChanged));

    public static readonly DependencyProperty ShowChevronProperty =
        DependencyProperty.Register(
            nameof(ShowChevron),
            typeof(bool),
            typeof(SpoilerHeaderGrid),
            new PropertyMetadata(true, OnShowChevronChanged));

    public static readonly DependencyProperty IconGlyphProperty =
        DependencyProperty.Register(
            nameof(IconGlyph),
            typeof(string),
            typeof(SpoilerHeaderGrid),
            new PropertyMetadata(string.Empty, OnIconGlyphChanged));

    public static readonly DependencyProperty DimHeaderActionsWithGateProperty =
        DependencyProperty.Register(
            nameof(DimHeaderActionsWithGate),
            typeof(bool),
            typeof(SpoilerHeaderGrid),
            new PropertyMetadata(true, OnDimHeaderActionsWithGateChanged));

    public static readonly DependencyProperty IsUnsavedPendingProperty =
        DependencyProperty.Register(
            nameof(IsUnsavedPending),
            typeof(bool),
            typeof(SpoilerHeaderGrid),
            new PropertyMetadata(false, (d, e) =>
            {
                if (d is not SpoilerHeaderGrid grid)
                    return;

                grid.UnsavedDot.Visibility = (bool)e.NewValue ? Visibility.Visible : Visibility.Collapsed;
                grid.UnsavedDot.Severity = SettingsNoticeSeverity.Warning;
            }));

    private readonly SpoilerHeaderControlsTable _headerControlsTable = new();

    public SpoilerHeaderGrid()
    {
        InitializeComponent();
        AttachControlsTable(_headerControlsTable);
        UpdateTrailingRailChrome();
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

    /// <summary>
    /// Подстрока с Inlines (цветные маркеры и т.п.).
    /// Последующая установка <see cref="Description"/> снова переводит в plain-текст.
    /// </summary>
    public void SetDescriptionInlines(IEnumerable<Inline> inlines)
    {
        DescriptionText.Inlines.Clear();
        var any = false;
        foreach (var inline in inlines)
        {
            DescriptionText.Inlines.Add(inline);
            any = true;
        }

        DescriptionText.Visibility = any ? Visibility.Visible : Visibility.Collapsed;
    }

    /// <summary>StaticHost: gate или один статичный контрол.</summary>
    public object? Trailing
    {
        get => GetValue(TrailingProperty);
        set => SetValue(TrailingProperty, value);
    }

    public object? StaticContent
    {
        get => Trailing;
        set => Trailing = value;
    }

    public bool ShowChevron
    {
        get => (bool)GetValue(ShowChevronProperty);
        set => SetValue(ShowChevronProperty, value);
    }

    public string IconGlyph
    {
        get => (string)GetValue(IconGlyphProperty);
        set => SetValue(IconGlyphProperty, value);
    }

    public bool IsUnsavedPending
    {
        get => (bool)GetValue(IsUnsavedPendingProperty);
        set => SetValue(IsUnsavedPendingProperty, value);
    }

    /// <summary>При gate off приглушать таблицу контролов (default true). StaticHost (gate) — всегда яркий.</summary>
    public bool DimHeaderActionsWithGate
    {
        get => (bool)GetValue(DimHeaderActionsWithGateProperty);
        set => SetValue(DimHeaderActionsWithGateProperty, value);
    }

    public SpoilerHeaderControlsTable HeaderControlsTable => _headerControlsTable;

    /// <summary>Ячейка 0 таблицы контролов (legacy API).</summary>
    public object? HeaderActions
    {
        get => GetLegacyCellContent(0);
        set => SetLegacyCellContent(0, value);
    }

    /// <summary>Ячейка 1 таблицы контролов (legacy API).</summary>
    public object? HeaderSlider
    {
        get => GetLegacyCellContent(1);
        set => SetLegacyCellContent(1, value);
    }

    public Grid LayoutRoot => HeaderGrid;

    public StackPanel TitleStack => TitlePanel;

    public TextBlock TitleTextBlock => TitleText;

    public TextBlock DescriptionTextBlock => DescriptionText;

    public ContentPresenter ControlsTablePresenter => ControlsTableHost;

    public ContentPresenter StaticHostPresenter => StaticHost;

    public StackPanel TrailingRail => TrailingRailPanel;

    /// <summary>Legacy alias.</summary>
    public ContentPresenter HeaderActionsPresenter => ControlsTableHost;

    /// <summary>Legacy alias.</summary>
    public ContentPresenter HeaderSliderPresenter => ControlsTableHost;

    /// <summary>Legacy alias.</summary>
    public ContentPresenter TrailingPresenter => StaticHost;

    public FontIcon Chevron => ChevronIcon;

    public FontIcon SectionIconGlyph => SectionIcon;

    public void AttachControlsTable(SpoilerHeaderControlsTable table)
    {
        ControlsTableHost.Content = table;
        ControlsTableHost.Visibility = table.IsEmpty ? Visibility.Collapsed : Visibility.Visible;
    }

    public void DetachControlsTable()
    {
        ControlsTableHost.Content = null;
        ControlsTableHost.Visibility = Visibility.Collapsed;
    }

    public double MeasureStaticHostWidth()
    {
        if (StaticHost.Visibility != Visibility.Visible || StaticHost.Content is null)
            return 0;

        StaticHost.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        return Math.Ceiling(StaticHost.DesiredSize.Width);
    }

    /// <summary>Gate/static + spacing + chevron и margin слева от controls table.</summary>
    public double MeasureTrailingRailWidth()
    {
        if (TrailingRailPanel.Visibility != Visibility.Visible)
            return 0;

        TrailingRailPanel.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        var margin = TrailingRailPanel.Margin;
        return Math.Ceiling(TrailingRailPanel.DesiredSize.Width + margin.Left + margin.Right);
    }

    public double MeasureChevronRailWidth()
    {
        if (ChevronIcon.Visibility != Visibility.Visible)
            return 0;

        ChevronIcon.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        return Math.Ceiling(ChevronIcon.DesiredSize.Width);
    }

    public void ApplyTitleRailWidth(double? width)
    {
        if (width is null or <= 0)
        {
            TitlePanel.ClearValue(FrameworkElement.MaxWidthProperty);
            TitlePanel.ClearValue(FrameworkElement.WidthProperty);
            TitlePanel.ClearValue(FrameworkElement.HorizontalAlignmentProperty);
            return;
        }

        var min = SpoilerHeaderLayoutMetrics.TitleMinWidth;
        var w = Math.Max(min, width.Value);
        TitlePanel.HorizontalAlignment = HorizontalAlignment.Left;
        TitlePanel.MaxWidth = w;
    }

    public double MeasureTitleRailWidth()
    {
        var max = 0d;
        max = Math.Max(max, MeasureTextBlockWidth(TitleText));
        if (DescriptionText.Visibility == Visibility.Visible)
            max = Math.Max(max, MeasureTextBlockWidth(DescriptionText));
        if (ParallelDuplicateText.Visibility == Visibility.Visible)
            max = Math.Max(max, MeasureTextBlockWidth(ParallelDuplicateText));

        max = Math.Max(max, SpoilerHeaderLayoutMetrics.TitleMinWidth);
        return Math.Ceiling(max);
    }

    private static double MeasureTextBlockWidth(TextBlock textBlock)
    {
        textBlock.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        return textBlock.DesiredSize.Width;
    }

    private Action? _updateLineAction;

    public void SetUpdateLine(bool hasUpdate, string? lineText, Action? onLineClick)
    {
        UpdateDot.Visibility = hasUpdate ? Visibility.Visible : Visibility.Collapsed;
        UpdateDot.Severity = SettingsNoticeSeverity.Warning;

        if (string.IsNullOrWhiteSpace(lineText) || onLineClick is null)
        {
            UpdateLineLink.Visibility = Visibility.Collapsed;
            UpdateLineLink.Click -= OnUpdateLineClick;
            _updateLineAction = null;
            return;
        }

        _updateLineAction = onLineClick;
        UpdateLineLink.Content = lineText;
        UpdateLineLink.Foreground = AccentSubtitleBrushService.ResolveAccent();
        UpdateLineLink.Visibility = Visibility.Visible;
        UpdateLineLink.Click -= OnUpdateLineClick;
        UpdateLineLink.Click += OnUpdateLineClick;
    }

    public void RefreshAlertChrome()
    {
        UpdateDot.RefreshChrome();
        UnsavedDot.RefreshChrome();
        if (UpdateLineLink.Visibility == Visibility.Visible)
            UpdateLineLink.Foreground = AccentSubtitleBrushService.ResolveAccent();
    }

    public void SetParallelBranchWarning(
        bool visible,
        string? toolTip,
        string? markerText = null)
    {
        ParallelDuplicateText.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
        ParallelDuplicateText.Text = string.IsNullOrWhiteSpace(markerText)
            ? "есть дубликат"
            : markerText;
        if (visible && !string.IsNullOrWhiteSpace(toolTip))
            ToolTipService.SetToolTip(ParallelDuplicateText, toolTip);
        else
            ToolTipService.SetToolTip(ParallelDuplicateText, null);
    }

    private void OnUpdateLineClick(object sender, RoutedEventArgs e) => _updateLineAction?.Invoke();

    public void ApplyGateVisual(bool gateOn)
    {
        if (gateOn)
        {
            TitlePanel.ClearValue(UIElement.OpacityProperty);
            if (DimHeaderActionsWithGate)
                ControlsTableHost.ClearValue(UIElement.OpacityProperty);
            TitleText.ClearValue(TextBlock.ForegroundProperty);
            DescriptionText.ClearValue(TextBlock.ForegroundProperty);
            if (SectionIcon.Visibility == Visibility.Visible)
                SectionIcon.ClearValue(UIElement.OpacityProperty);
            if (ShowChevron)
                ChevronIcon.ClearValue(UIElement.OpacityProperty);
        }
        else
        {
            TitlePanel.Opacity = 0.55;
            if (DimHeaderActionsWithGate)
                ControlsTableHost.Opacity = 0.55;
            TitleText.Foreground = GetThemeBrush("TextFillColorTertiaryBrush");
            DescriptionText.Foreground = GetThemeBrush("TextFillColorTertiaryBrush");
            if (SectionIcon.Visibility == Visibility.Visible)
                SectionIcon.Opacity = 0.55;
            if (ShowChevron)
                ChevronIcon.Opacity = 0.4;
        }

        if (!DimHeaderActionsWithGate)
            ControlsTableHost.ClearValue(UIElement.OpacityProperty);

        StaticHost.ClearValue(UIElement.OpacityProperty);
    }

    private static Brush GetThemeBrush(string key) =>
        Application.Current.Resources[key] is Brush brush
            ? brush
            : new SolidColorBrush(Microsoft.UI.Colors.Gray);

    private static void OnTitleChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SpoilerHeaderGrid grid)
            grid.TitleText.Text = e.NewValue as string ?? string.Empty;
    }

    private static void OnDescriptionChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not SpoilerHeaderGrid grid)
            return;

        var text = e.NewValue as string ?? string.Empty;
        grid.DescriptionText.Text = text;
        grid.DescriptionText.Visibility = string.IsNullOrEmpty(text)
            ? Visibility.Collapsed
            : Visibility.Visible;
    }

    private static void OnTrailingChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not SpoilerHeaderGrid grid)
            return;

        SpoilerHeaderLayoutMetrics.DetachFromVisualParent(e.NewValue);
        grid.StaticHost.Content = e.NewValue;
        grid.StaticHost.Visibility = e.NewValue is null ? Visibility.Collapsed : Visibility.Visible;
        grid.UpdateTrailingRailChrome();
    }

    private static void OnDimHeaderActionsWithGateChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
    }

    private static void OnShowChevronChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not SpoilerHeaderGrid grid)
            return;

        grid.ChevronIcon.Visibility = (bool)e.NewValue ? Visibility.Visible : Visibility.Collapsed;
        grid.UpdateTrailingRailChrome();
    }

    private static void OnIconGlyphChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not SpoilerHeaderGrid grid)
            return;

        var glyph = e.NewValue as string ?? string.Empty;
        grid.SectionIcon.Glyph = glyph;
        grid.SectionIcon.Visibility = string.IsNullOrEmpty(glyph)
            ? Visibility.Collapsed
            : Visibility.Visible;
    }

    private object? GetLegacyCellContent(int index)
    {
        var cell = _headerControlsTable.GetCell(index);
        return cell?.Control ?? cell;
    }

    private void SetLegacyCellContent(int index, object? content)
    {
        if (content is null)
            _headerControlsTable.ClearCell(index);
        else
            _headerControlsTable.SetCellFromLegacy(index, content);

        AttachControlsTable(_headerControlsTable);
    }

    private void UpdateTrailingRailChrome()
    {
        var hasStatic = StaticHost.Content is not null;
        TrailingRailPanel.Visibility = hasStatic || ShowChevron ? Visibility.Visible : Visibility.Collapsed;
    }
}
