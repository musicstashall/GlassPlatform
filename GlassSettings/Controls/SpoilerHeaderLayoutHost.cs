using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

/// <summary>Единая подписка SizeChanged и wiring layout для row/expander шапок.</summary>
public sealed class SpoilerHeaderLayoutHost
{
    private const int MaxLayoutRetryCount = 3;

    private FrameworkElement? _widthSource;
    private SpoilerHeaderGrid? _chrome;
    private ContentControl? _belowHost;
    private SpoilerHeaderLayoutOptions _options = new();
    private bool _stacked;
    private bool _layoutInitialized;
    private bool _layoutRetryPending;
    private int _layoutRetryCount;
    private Func<bool>? _dimBelowWhenDisabled;

    public bool IsStacked => _stacked;

    public void Attach(
        FrameworkElement widthSource,
        SpoilerHeaderGrid chrome,
        ContentControl? belowHost,
        SpoilerHeaderLayoutOptions? options = null,
        Func<bool>? dimBelowWhenEnabled = null)
    {
        Detach();
        _widthSource = widthSource;
        _chrome = chrome;
        _belowHost = belowHost;
        _options = options ?? new SpoilerHeaderLayoutOptions();
        _dimBelowWhenDisabled = dimBelowWhenEnabled;
        _layoutInitialized = false;
        _layoutRetryCount = 0;
        widthSource.SizeChanged += WidthSource_OnSizeChanged;
        UpdateLayout();
    }

    public void Detach()
    {
        if (_widthSource is not null)
            _widthSource.SizeChanged -= WidthSource_OnSizeChanged;
        _widthSource = null;
        _chrome = null;
        _belowHost = null;
        _layoutRetryPending = false;
        _layoutRetryCount = 0;
    }

    public void Invalidate()
    {
        _layoutInitialized = false;
        _layoutRetryCount = 0;
        UpdateLayout();
    }

    public void UpdateLayout()
    {
        if (_widthSource is null || _chrome is null || _belowHost is null)
            return;

        var available = _widthSource.ActualWidth;
        if (available <= 0)
        {
            ScheduleOneShotLayoutRetry();
            return;
        }

        var context = new SpoilerHeaderLayoutContext
        {
            Chrome = _chrome,
            BelowHost = _belowHost
        };

        SpoilerHeaderLayoutEngine.UpdateLayout(
            available,
            ref _stacked,
            ref _layoutInitialized,
            context,
            _options);

        SyncBelowHostDim();
        _layoutRetryCount = 0;
    }

    public void SyncBelowHostDim(bool? enabledOverride = null)
    {
        if (_belowHost is null)
            return;

        if (_belowHost.Visibility != Visibility.Visible)
        {
            _belowHost.ClearValue(UIElement.OpacityProperty);
            return;
        }

        var enabled = enabledOverride ?? _dimBelowWhenDisabled?.Invoke() ?? true;
        if (enabled)
            _belowHost.ClearValue(UIElement.OpacityProperty);
        else
            _belowHost.Opacity = 0.55;
    }

    private void WidthSource_OnSizeChanged(object sender, SizeChangedEventArgs e) => UpdateLayout();

    private void ScheduleOneShotLayoutRetry()
    {
        if (_layoutRetryPending || _layoutRetryCount >= MaxLayoutRetryCount || _widthSource is null)
            return;

        _layoutRetryPending = true;
        _layoutRetryCount++;
        _widthSource.DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, () =>
        {
            _layoutRetryPending = false;
            if (_widthSource is null || _widthSource.ActualWidth > 0)
            {
                UpdateLayout();
                return;
            }

            if (_layoutRetryCount < MaxLayoutRetryCount)
                ScheduleOneShotLayoutRetry();
        });
    }
}
