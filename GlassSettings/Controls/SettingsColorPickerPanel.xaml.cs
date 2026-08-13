using GlassSettings.Strings;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;

namespace GlassSettings.Controls;

[ContentProperty(Name = nameof(PickerContent))]
public sealed partial class SettingsColorPickerPanel : UserControl
{
    public static readonly DependencyProperty DescriptionProperty =
        DependencyProperty.Register(
            nameof(Description),
            typeof(string),
            typeof(SettingsColorPickerPanel),
            new PropertyMetadata(string.Empty, OnDescriptionChanged));

    public static readonly DependencyProperty PickerContentProperty =
        DependencyProperty.Register(
            nameof(PickerContent),
            typeof(object),
            typeof(SettingsColorPickerPanel),
            new PropertyMetadata(null, OnPickerContentChanged));

    public static readonly DependencyProperty FooterContentProperty =
        DependencyProperty.Register(
            nameof(FooterContent),
            typeof(object),
            typeof(SettingsColorPickerPanel),
            new PropertyMetadata(null, OnFooterContentChanged));

    private const double StackUnstackThresholdPx = 1;
    private const double PickerSeparatorWidth = 1;

    private bool _pickerStacked;
    private bool _layoutInitialized;
    private bool _hasDescription;
    private bool _defaultActionVisible;
    private bool _hasFooterContent;
    private object? _rawFooterContent;
    private bool _layoutUpdating;
    private bool _footerLayoutPassPending;
    private double _lastInfoMinHeight = -1;
    private double _cachedMinInfoColumnSlotWidth = -1;
    private double _cachedPickerRailWidth = -1;

    public SettingsColorPickerPanel()
    {
        InitializeComponent();
        DefaultColorButton.Content = GlassSettingsResources.Get("ColorPicker_ResetDefault");
        Loaded += (_, _) =>
        {
            RefreshContentChrome();
            UpdatePickerLayout();
        };
        LayoutRoot.SizeChanged += (_, _) => UpdatePickerLayout();
    }

    public event RoutedEventHandler? DefaultColorClick;

    public string Description
    {
        get => (string)GetValue(DescriptionProperty);
        set => SetValue(DescriptionProperty, value);
    }

    public object? PickerContent
    {
        get => GetValue(PickerContentProperty);
        set => SetValue(PickerContentProperty, value);
    }

    public object? FooterContent
    {
        get => GetValue(FooterContentProperty);
        set => SetValue(FooterContentProperty, value);
    }

    /// <summary>Скрывает footer без удаления контента (напр. blur на Vista).</summary>
    public void SetFooterContentVisible(bool visible)
    {
        if (FooterHost.Content is FrameworkElement footer)
            footer.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;

        InvalidateLayoutThresholds();
        _layoutInitialized = false;
        RefreshContentChrome();
        UpdatePickerLayout();
    }

    public void SetDefaultColorActionState(bool showDefaultAction, bool disableDefaultButton, bool isEnabled = true)
    {
        _defaultActionVisible = showDefaultAction;
        DefaultColorButton.IsEnabled = showDefaultAction && isEnabled && !disableDefaultButton;
        InvalidateLayoutThresholds();
        RefreshContentChrome();
        _layoutInitialized = false;
        UpdatePickerLayout();
    }

    /// <summary>Пересчёт layout после expand/collapse expander (страницы detail).</summary>
    public void RefreshLayout() => UpdatePickerLayout();

    private static void OnDescriptionChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsColorPickerPanel panel)
        {
            var text = e.NewValue as string ?? string.Empty;
            panel._hasDescription = !string.IsNullOrWhiteSpace(text);
            panel.DescriptionText.Text = text;
            panel.InvalidateLayoutThresholds();
            panel.RefreshContentChrome();
            panel._layoutInitialized = false;
            panel.UpdatePickerLayout();
        }
    }

    private static void OnPickerContentChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsColorPickerPanel panel)
        {
            panel.PickerHost.Content = e.NewValue;
            panel.InvalidateLayoutThresholds();
            panel._layoutInitialized = false;
            panel.UpdatePickerLayout();
        }
    }

    private static void OnFooterContentChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsColorPickerPanel panel)
        {
            panel._rawFooterContent = e.NewValue;
            panel.ApplyFooterContent();
        }
    }

    private void ApplyFooterContent()
    {
        var normalized = SettingsPickerBodyLayout.NormalizeFooterContent(
            _rawFooterContent as FrameworkElement);
        FooterHost.Content = normalized;
        _hasFooterContent = normalized is not null;
        FooterHost.HorizontalAlignment = HorizontalAlignment.Stretch;
        FooterHost.HorizontalContentAlignment = HorizontalAlignment.Stretch;
        FooterHost.VerticalAlignment = VerticalAlignment.Stretch;
        FooterHost.VerticalContentAlignment = VerticalAlignment.Stretch;
        InvalidateLayoutThresholds();
        RefreshContentChrome();
        _layoutInitialized = false;
        UpdatePickerLayout();
    }

    private void DefaultColorButton_OnClick(object sender, RoutedEventArgs e) =>
        DefaultColorClick?.Invoke(this, e);

    private void RefreshContentChrome()
    {
        var showInfo = HasVisibleInfoContent();
        InfoColumn.Visibility = showInfo ? Visibility.Visible : Visibility.Collapsed;
        InfoColumnDefinition.Width = showInfo
            ? new GridLength(1, GridUnitType.Star)
            : new GridLength(0);

        DescriptionText.Visibility = _hasDescription ? Visibility.Visible : Visibility.Collapsed;
        DescriptionBullet.Visibility = _hasDescription ? Visibility.Visible : Visibility.Collapsed;
        DescriptionDivider.Visibility = _hasDescription || HasEffectiveFooter() ? Visibility.Visible : Visibility.Collapsed;

        DefaultActionPanel.Visibility = _defaultActionVisible ? Visibility.Visible : Visibility.Collapsed;
        FooterHost.Visibility = _hasFooterContent ? Visibility.Visible : Visibility.Collapsed;
    }

    private bool HasVisibleInfoContent() =>
        _hasDescription || _defaultActionVisible || HasEffectiveFooter();

    private bool HasEffectiveFooter() =>
        _hasFooterContent && SettingsPickerBodyLayout.IsFooterEffectivelyVisible(FooterHost.Content);

    private void InvalidateLayoutThresholds()
    {
        _cachedMinInfoColumnSlotWidth = -1;
        _cachedPickerRailWidth = -1;
    }

    private bool EnsureLayoutThresholdsCached()
    {
        if (_cachedPickerRailWidth <= 0)
        {
            var rail = MeasurePickerRailWidth();
            if (rail > 0)
                _cachedPickerRailWidth = rail;
        }

        if (HasVisibleInfoContent() && _cachedMinInfoColumnSlotWidth <= 0)
        {
            var minInner = SettingsPickerBodyLayout.EstimateMinimumInfoContentWidth(
                _hasDescription,
                _defaultActionVisible,
                FooterHost.Content as FrameworkElement);
            _cachedMinInfoColumnSlotWidth =
                SettingsPickerBodyLayout.InfoColumnHorizontalPadding + minInner;
        }

        if (!HasVisibleInfoContent())
            return _cachedPickerRailWidth > 0;

        return _cachedMinInfoColumnSlotWidth > 0 && _cachedPickerRailWidth > 0;
    }

    private double MeasurePickerRailWidth()
    {
        if (!_pickerStacked && PickerZone.ActualWidth > 0)
            return Math.Ceiling(PickerZone.ActualWidth);

        PickerZone.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        var measured = Math.Ceiling(PickerZone.DesiredSize.Width);
        if (measured > 0)
            return measured;

        return 0;
    }

    private double MeasureInfoToPickerSlotWidth(double hostWidth)
    {
        if (_pickerStacked)
        {
            if (_cachedPickerRailWidth <= 0)
                return 0;

            return Math.Max(0, hostWidth - _cachedPickerRailWidth - PickerSeparatorWidth);
        }

        try
        {
            var transform = PickerZone.TransformToVisual(LayoutRoot);
            var point = transform.TransformPoint(new Point(0, 0));
            if (point.X > 0)
                return point.X;
        }
        catch (InvalidOperationException)
        {
            // Visual tree ещё не готов — fallback ниже.
        }

        if (InfoColumn.ActualWidth > 0)
        {
            var sep = PickerSeparator.ActualWidth > 0 ? PickerSeparator.ActualWidth : PickerSeparatorWidth;
            return InfoColumn.ActualWidth + sep;
        }

        return 0;
    }

    private bool ResolvePickerStackState(double slotWidth)
    {
        if (_layoutInitialized && _pickerStacked)
            return slotWidth < _cachedMinInfoColumnSlotWidth + StackUnstackThresholdPx;

        return slotWidth < _cachedMinInfoColumnSlotWidth;
    }

    private bool ResolvePickerOnlyStackState(double hostWidth)
    {
        if (_layoutInitialized && _pickerStacked)
            return hostWidth < _cachedPickerRailWidth + StackUnstackThresholdPx;

        return hostWidth < _cachedPickerRailWidth;
    }

    private double GetLayoutHostWidth() =>
        LayoutRoot.ActualWidth > 0 ? LayoutRoot.ActualWidth : ActualWidth;

    private double ResolveInlinePickerRailWidth()
    {
        if (_cachedPickerRailWidth > 0)
            return _cachedPickerRailWidth;

        if (!_pickerStacked && PickerZone.ActualWidth > 0)
            return PickerZone.ActualWidth;

        return 0;
    }

    /// <summary>Полная inner-ширина info-контента: LayoutRoot − picker − separator − padding.</summary>
    private double ResolveInfoContentWidth()
    {
        var hostWidth = GetLayoutHostWidth();
        if (hostWidth <= 0)
            return 0;

        double layoutWidth;
        if (_pickerStacked || !HasVisibleInfoContent())
        {
            layoutWidth = Math.Max(0, hostWidth - SettingsPickerBodyLayout.InfoColumnHorizontalPadding);
        }
        else
        {
            var pickerRail = ResolveInlinePickerRailWidth();
            if (pickerRail <= 0)
                return 0;

            layoutWidth = Math.Max(
                0,
                hostWidth
                - pickerRail
                - PickerSeparatorWidth
                - SettingsPickerBodyLayout.InfoColumnHorizontalPadding);
        }

        var f = SettingsPickerBodyLayoutTrace.Format;
        SettingsPickerBodyLayoutTrace.WriteIfChanged(
            $"{GetHashCode()}:ResolveInfo",
            GetTraceTag(),
            $"ResolveInfoContentWidth hostW={f(hostWidth)} rail={f(ResolveInlinePickerRailWidth())} stacked={_pickerStacked} layoutW={f(layoutWidth)}");

        return layoutWidth;
    }

    private void Root_OnSizeChanged(object sender, SizeChangedEventArgs e) => UpdatePickerLayout();

    private void UpdatePickerLayout()
    {
        if (_layoutUpdating)
            return;

        var available = GetLayoutHostWidth();
        if (available <= 0)
            return;

        try
        {
            _layoutUpdating = true;

            var showInfo = HasVisibleInfoContent();
            var thresholdsReady = EnsureLayoutThresholdsCached();
            var slotWidth = 0d;
            var stacked = _pickerStacked;

            if (thresholdsReady)
            {
                if (showInfo)
                {
                    slotWidth = MeasureInfoToPickerSlotWidth(available);
                    if (slotWidth > 0)
                        stacked = ResolvePickerStackState(slotWidth);
                }
                else
                {
                    stacked = ResolvePickerOnlyStackState(available);
                }
            }

            var stackChanged = stacked != _pickerStacked || !_layoutInitialized;
            _pickerStacked = stacked;
            _layoutInitialized = true;

            if (stackChanged)
                ApplyPickerStackLayout(stacked, showInfo);

            RefreshFooterBodyLayout();
            ScheduleFooterBodyLayoutPass();

            var f = SettingsPickerBodyLayoutTrace.Format;
            SettingsPickerBodyLayoutTrace.WriteIfChanged(
                $"{GetHashCode()}:UpdatePickerLayout",
                GetTraceTag(),
                $"UpdatePickerLayout host={f(available)} slotW={f(slotWidth)} cachedMin={f(_cachedMinInfoColumnSlotWidth)} cachedRail={f(_cachedPickerRailWidth)} unstackAt={f(_cachedMinInfoColumnSlotWidth + StackUnstackThresholdPx)} stacked={stacked} stackChanged={stackChanged} thresholdsReady={thresholdsReady} infoCol={f(InfoColumn.ActualWidth)} footerHost={f(FooterHost.ActualWidth)}");
        }
        finally
        {
            _layoutUpdating = false;
        }
    }

    private void ApplyPickerStackLayout(bool stacked, bool showInfo)
    {
        if (stacked)
        {
            if (showInfo)
            {
                Grid.SetRow(InfoColumn, 0);
                Grid.SetColumn(InfoColumn, 0);
                Grid.SetColumnSpan(InfoColumn, 3);

                Grid.SetRow(PickerSeparator, 1);
                Grid.SetColumn(PickerSeparator, 0);
                Grid.SetColumnSpan(PickerSeparator, 3);
                PickerSeparator.Width = double.NaN;
                PickerSeparator.Height = 1;
                PickerSeparator.Margin = new Thickness(0);
                PickerSeparator.HorizontalAlignment = HorizontalAlignment.Stretch;
                PickerSeparator.VerticalAlignment = VerticalAlignment.Top;

                Grid.SetRow(PickerZone, 2);
                Grid.SetColumn(PickerZone, 0);
                Grid.SetColumnSpan(PickerZone, 3);
            }
            else
            {
                PickerSeparator.Visibility = Visibility.Collapsed;

                Grid.SetRow(PickerZone, 0);
                Grid.SetColumn(PickerZone, 0);
                Grid.SetColumnSpan(PickerZone, 3);
            }

            PickerZone.HorizontalAlignment = HorizontalAlignment.Stretch;
            PickerSeparator.Visibility = showInfo ? Visibility.Visible : Visibility.Collapsed;
            PickerHost.HorizontalAlignment = HorizontalAlignment.Right;
            InfoColumn.ClearValue(FrameworkElement.MinHeightProperty);
            _lastInfoMinHeight = -1;
            return;
        }

        PickerSeparator.Visibility = showInfo ? Visibility.Visible : Visibility.Collapsed;
        PickerSeparator.Width = 1;
        PickerSeparator.Height = double.NaN;
        PickerSeparator.Margin = new Thickness(0);
        PickerSeparator.HorizontalAlignment = HorizontalAlignment.Stretch;
        PickerSeparator.VerticalAlignment = VerticalAlignment.Stretch;
        PickerZone.BorderThickness = new Thickness(0);

        if (showInfo)
        {
            Grid.SetRow(InfoColumn, 0);
            Grid.SetColumn(InfoColumn, 0);
            Grid.SetColumnSpan(InfoColumn, 1);
            Grid.SetRowSpan(InfoColumn, 1);

            Grid.SetRow(PickerSeparator, 0);
            Grid.SetColumn(PickerSeparator, 1);
            Grid.SetColumnSpan(PickerSeparator, 1);
            Grid.SetRowSpan(PickerSeparator, 1);

            Grid.SetRow(PickerZone, 0);
            Grid.SetColumn(PickerZone, 2);
            Grid.SetColumnSpan(PickerZone, 1);
            Grid.SetRowSpan(PickerZone, 1);
        }
        else
        {
            Grid.SetRow(PickerZone, 0);
            Grid.SetColumn(PickerZone, 0);
            Grid.SetColumnSpan(PickerZone, 3);
        }

        PickerZone.HorizontalAlignment = HorizontalAlignment.Right;
        PickerHost.HorizontalAlignment = HorizontalAlignment.Right;

        if (PickerZone.ActualHeight > 0)
        {
            var h = PickerZone.ActualHeight;
            if (Math.Abs(_lastInfoMinHeight - h) > 1)
            {
                InfoColumn.MinHeight = h;
                _lastInfoMinHeight = h;
            }
        }
    }

    private void RefreshFooterBodyLayout()
    {
        if (FooterHost.Content is not Grid bodyTable || !SettingsPickerBodyLayout.IsBodyTable(bodyTable))
            return;

        var layoutWidth = ResolveInfoContentWidth();
        if (layoutWidth <= 0)
            return;

        var metrics = SettingsPickerBodyLayout.UpdateBodyTableLayout(bodyTable, layoutWidth);
        ApplyFooterHostInset(metrics.RightInset);

        var f = SettingsPickerBodyLayoutTrace.Format;
        SettingsPickerBodyLayoutTrace.WriteIfChanged(
            $"{GetHashCode()}:RefreshFooter",
            GetTraceTag(),
            $"RefreshFooter layoutW={f(layoutWidth)} labelW={f(metrics.LabelWidth)} trackW={f(metrics.TrackWidth)} tableW={f(metrics.TableWidth)} inset={f(metrics.RightInset)} footerHost={f(FooterHost.ActualWidth)}");
    }

    private void ApplyFooterHostInset(double rightInset)
    {
        if (Math.Abs(FooterHost.Padding.Right - rightInset) > 0.5)
        {
            FooterHost.Padding = new Thickness(0, 0, rightInset, 0);
            SettingsPickerBodyLayoutTrace.WriteIfChanged(
                $"{GetHashCode()}:Inset",
                GetTraceTag(),
                $"ApplyFooterHostInset inset={SettingsPickerBodyLayoutTrace.Format(rightInset)} footerHost={SettingsPickerBodyLayoutTrace.Format(FooterHost.ActualWidth)}");
        }

        FooterHost.HorizontalContentAlignment = HorizontalAlignment.Left;
    }

    private void ScheduleFooterBodyLayoutPass()
    {
        if (_footerLayoutPassPending)
            return;

        _footerLayoutPassPending = true;
        DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, () =>
        {
            _footerLayoutPassPending = false;
            if (_layoutUpdating)
                return;

            RefreshFooterBodyLayout();
        });
    }

    private string GetTraceTag()
    {
        var desc = Description;
        if (!string.IsNullOrWhiteSpace(desc))
        {
            desc = desc.Trim();
            return desc.Length > 24 ? desc[..24] : desc;
        }

        return $"Panel#{GetHashCode()}";
    }
}
