using GlassSettings.Strings;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Markup;

namespace GlassSettings.Controls;

[ContentProperty(Name = nameof(BodyContent))]
public sealed partial class SettingsParameterPanel : UserControl
{
    private const int MaxLayoutRetryCount = 3;

    public static readonly DependencyProperty DescriptionProperty =
        DependencyProperty.Register(
            nameof(Description),
            typeof(string),
            typeof(SettingsParameterPanel),
            new PropertyMetadata(string.Empty, OnDescriptionChanged));

    public static readonly DependencyProperty BodyContentProperty =
        DependencyProperty.Register(
            nameof(BodyContent),
            typeof(object),
            typeof(SettingsParameterPanel),
            new PropertyMetadata(null, OnBodyContentChanged));

    private bool _defaultActionVisible;
    private bool _layoutUpdating;
    private bool _bodyLayoutPassPending;
    private bool _layoutRetryPending;
    private int _layoutRetryCount;
    private double _lastAppliedRightInset = -1;
    private double _lastAppliedTableWidth = -1;
    private bool _lastAppliedWrapActive;

    public SettingsParameterPanel()
    {
        InitializeComponent();
        DefaultButton.Content = GlassSettingsResources.Get("ColorPicker_ResetDefault");
        Loaded += (_, _) =>
        {
            RefreshContentChrome();
            UpdateParameterLayout();
        };
        LayoutRoot.SizeChanged += (_, _) => UpdateParameterLayout();
    }

    public event RoutedEventHandler? DefaultClick;

    public string Description
    {
        get => (string)GetValue(DescriptionProperty);
        set => SetValue(DescriptionProperty, value);
    }

    public object? BodyContent
    {
        get => GetValue(BodyContentProperty);
        set => SetValue(BodyContentProperty, value);
    }

    public void SetDefaultActionState(bool showDefaultAction, bool disableDefaultButton, bool isEnabled = true)
    {
        _defaultActionVisible = showDefaultAction;
        DefaultButton.IsEnabled = showDefaultAction && isEnabled && !disableDefaultButton;
        RefreshContentChrome();
        UpdateParameterLayout();
    }

    /// <summary>Пересчёт body-layout после expand/collapse expander.</summary>
    public void RefreshLayout()
    {
        _lastAppliedRightInset = -1;
        _lastAppliedTableWidth = -1;
        _lastAppliedWrapActive = false;

        if (BodyHost.Content is Grid bodyTable && SettingsParameterBodyLayout.IsBodyTable(bodyTable))
            SettingsParameterBodyLayout.InvalidateParameterBodyLayoutCache(bodyTable);

        if (TryRunLayoutPass())
            _layoutRetryCount = 0;
        else
            ScheduleOneShotLayoutRetry();
    }

    /// <summary>Снять фокус с полей ввода внутри панели (якорь для NumberBox после Enter).</summary>
    public void RelinquishInputFocus() => FocusSink.Focus(FocusState.Programmatic);

    private static void OnDescriptionChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsParameterPanel panel)
        {
            panel.DescriptionText.Text = e.NewValue as string ?? string.Empty;
            panel.RefreshContentChrome();
        }
    }

    private static void OnBodyContentChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsParameterPanel panel)
        {
            panel.BodyHost.Content = e.NewValue;
            panel._lastAppliedRightInset = -1;
            panel._lastAppliedTableWidth = -1;
            panel._lastAppliedWrapActive = false;
            panel.UpdateParameterLayout();
        }
    }

    private void DefaultButton_OnClick(object sender, RoutedEventArgs e) =>
        DefaultClick?.Invoke(this, e);

    private void RefreshContentChrome()
    {
        var hasDescription = !string.IsNullOrWhiteSpace(DescriptionText.Text);
        DescriptionHeader.Visibility = hasDescription ? Visibility.Visible : Visibility.Collapsed;
        DescriptionText.Visibility = hasDescription ? Visibility.Visible : Visibility.Collapsed;
        DescriptionBullet.Visibility = hasDescription ? Visibility.Visible : Visibility.Collapsed;
        TopDivider.Visibility = hasDescription ? Visibility.Visible : Visibility.Collapsed;
        DefaultActionPanel.Visibility = _defaultActionVisible ? Visibility.Visible : Visibility.Collapsed;

        // Без призыва не оставляем Padding описания (~22) + лишний top у BodyHost.
        BodyHost.Margin = hasDescription
            ? new Thickness(28, 12, 16, 0)
            : new Thickness(28, 0, 16, 0);
    }

    private double GetLayoutHostWidth() =>
        LayoutRoot.ActualWidth > 0 ? LayoutRoot.ActualWidth : ActualWidth;

    private double ResolveBodyContentWidth()
    {
        var hostWidth = GetLayoutHostWidth();
        if (hostWidth <= 0)
            return 0;

        return Math.Max(0, hostWidth - SettingsParameterBodyLayout.InfoColumnHorizontalPadding);
    }

    private bool TryRunLayoutPass()
    {
        if (ResolveBodyContentWidth() <= 0)
            return false;

        UpdateParameterLayout();
        return ResolveBodyContentWidth() > 0;
    }

    private void ScheduleOneShotLayoutRetry()
    {
        if (_layoutRetryPending || _layoutRetryCount >= MaxLayoutRetryCount)
            return;

        _layoutRetryPending = true;
        _layoutRetryCount++;
        DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Normal, () =>
        {
            _layoutRetryPending = false;
            if (TryRunLayoutPass())
                _layoutRetryCount = 0;
            else
                ScheduleOneShotLayoutRetry();
        });
    }

    private void UpdateParameterLayout()
    {
        if (_layoutUpdating)
            return;

        if (BodyHost.Content is not Grid bodyTable || !SettingsParameterBodyLayout.IsBodyTable(bodyTable))
            return;

        var layoutWidth = ResolveBodyContentWidth();
        if (layoutWidth <= 0)
            return;

        try
        {
            _layoutUpdating = true;

            var metrics = SettingsParameterBodyLayout.UpdateParameterBodyLayout(bodyTable, layoutWidth);
            ApplyBodyHostInset(metrics);

            if (LayoutMetricsChanged(metrics))
                ScheduleBodyLayoutPass();

            _layoutRetryCount = 0;

            var f = SettingsParameterBodyLayoutTrace.Format;
            SettingsParameterBodyLayoutTrace.WriteIfChanged(
                $"{GetHashCode()}:UpdateParameterLayout",
                $"Panel#{GetHashCode()}",
                $"UpdateParameterLayout host={f(GetLayoutHostWidth())} layoutW={f(layoutWidth)} tableW={f(metrics.TableWidth)} inset={f(metrics.RightInset)} wrap={metrics.RowWrapActive}");
        }
        finally
        {
            _layoutUpdating = false;
        }
    }

    private bool LayoutMetricsChanged(SettingsParameterBodyLayout.ParameterBodyLayoutMetrics metrics)
    {
        if (Math.Abs(_lastAppliedRightInset - metrics.RightInset) >= 0.5
            || Math.Abs(_lastAppliedTableWidth - metrics.TableWidth) >= 0.5
            || _lastAppliedWrapActive != metrics.RowWrapActive)
        {
            _lastAppliedRightInset = metrics.RightInset;
            _lastAppliedTableWidth = metrics.TableWidth;
            _lastAppliedWrapActive = metrics.RowWrapActive;
            return true;
        }

        return false;
    }

    private void ApplyBodyHostInset(SettingsParameterBodyLayout.ParameterBodyLayoutMetrics metrics)
    {
        if (metrics.TableWidth <= 0)
        {
            if (BodyHost.Padding.Right > 0.5)
                BodyHost.Padding = new Thickness(0);
            return;
        }

        var rightInset = metrics.RightInset;
        if (Math.Abs(BodyHost.Padding.Right - rightInset) > 0.5)
            BodyHost.Padding = new Thickness(0, 0, rightInset, 0);
    }

    private void ScheduleBodyLayoutPass()
    {
        if (_bodyLayoutPassPending)
            return;

        _bodyLayoutPassPending = true;
        DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, () =>
        {
            _bodyLayoutPassPending = false;
            if (_layoutUpdating)
                return;

            if (BodyHost.Content is not Grid bodyTable || !SettingsParameterBodyLayout.IsBodyTable(bodyTable))
                return;

            var layoutWidth = ResolveBodyContentWidth();
            if (layoutWidth <= 0)
                return;

            try
            {
                _layoutUpdating = true;

                var metrics = SettingsParameterBodyLayout.UpdateParameterBodyLayout(bodyTable, layoutWidth);
                ApplyBodyHostInset(metrics);
                LayoutMetricsChanged(metrics);
            }
            finally
            {
                _layoutUpdating = false;
            }
        });
    }
}
