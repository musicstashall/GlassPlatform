using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;

namespace GlassSettings.Services;

/// <summary>Slide/fade анимация для composite host (акрил + notice) зеркала.</summary>
internal static class NoticeHostSlideAnimator
{
    private static double SlideOffsetY => -GlassSettingsNoticeMetrics.StripMinHeight;
    private const int SlideMs = 1000;

    private static Storyboard? _activeStoryboard;
    private static TaskCompletionSource<bool>? _activeTcs;

    public static void PrepareSlideInStart(UIElement host, TranslateTransform transform)
    {
        host.Opacity = 0;
        transform.Y = SlideOffsetY;
    }

    public static void ResetState(UIElement host, TranslateTransform transform)
    {
        host.Opacity = 1;
        transform.Y = 0;
    }

    /// <summary>Остановить текущий slide/fade; разблокировать await (Stop() часто не шлёт Completed).</summary>
    public static void CancelActive(UIElement host, TranslateTransform transform)
    {
        if (_activeStoryboard is not null)
        {
            _activeStoryboard.Stop();
            _activeStoryboard = null;
        }

        var tcs = _activeTcs;
        _activeTcs = null;
        tcs?.TrySetResult(true);

        ResetState(host, transform);
    }

    public static Task SlideInAsync(UIElement host, TranslateTransform transform) =>
        RunSlideAsync(host, transform, fromY: SlideOffsetY, toY: 0, fromOpacity: 0, toOpacity: 1, EasingMode.EaseOut);

    public static Task SlideUpOutAsync(UIElement host, TranslateTransform transform)
    {
        var fromY = transform.Y;
        var fromOpacity = host.Opacity;
        return RunSlideAsync(host, transform, fromY, SlideOffsetY, fromOpacity, 0, EasingMode.EaseIn);
    }

    private static async Task RunSlideAsync(
        UIElement host,
        TranslateTransform transform,
        double fromY,
        double toY,
        double fromOpacity,
        double toOpacity,
        EasingMode easing)
    {
        CancelActive(host, transform);

        var storyboard = new Storyboard();
        var tcs = new TaskCompletionSource<bool>();
        _activeStoryboard = storyboard;
        _activeTcs = tcs;

        var fade = new DoubleAnimation
        {
            From = fromOpacity,
            To = toOpacity,
            Duration = TimeSpan.FromMilliseconds(SlideMs),
            EasingFunction = new CubicEase { EasingMode = easing }
        };
        Storyboard.SetTarget(fade, host);
        Storyboard.SetTargetProperty(fade, nameof(UIElement.Opacity));
        storyboard.Children.Add(fade);

        var slide = new DoubleAnimation
        {
            From = fromY,
            To = toY,
            Duration = TimeSpan.FromMilliseconds(SlideMs),
            EasingFunction = new CubicEase { EasingMode = easing }
        };
        Storyboard.SetTarget(slide, host);
        Storyboard.SetTargetProperty(slide, "(UIElement.RenderTransform).(TranslateTransform.Y)");
        storyboard.Children.Add(slide);

        try
        {
            await RunStoryboardAsync(storyboard, tcs).ConfigureAwait(true);
        }
        finally
        {
            if (ReferenceEquals(_activeStoryboard, storyboard))
                _activeStoryboard = null;
            if (ReferenceEquals(_activeTcs, tcs))
                _activeTcs = null;
        }

        ResetState(host, transform);
    }

    private static Task RunStoryboardAsync(Storyboard storyboard, TaskCompletionSource<bool> tcs)
    {
        void OnCompleted(object? sender, object e)
        {
            storyboard.Completed -= OnCompleted;
            tcs.TrySetResult(true);
        }

        storyboard.Completed += OnCompleted;
        storyboard.Begin();
        return tcs.Task;
    }
}
