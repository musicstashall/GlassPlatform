using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.UI.ViewManagement;

namespace GlassSettings.Controls;

/// <summary>
/// Горизонтальный slide для контента под Pivot / сегментом DWM|XAML.
/// Смена вкладки — одновременный cross-slide (уходящая + входящая), как у Pivot.
/// </summary>
public static class PivotTabContentTransition
{
    private const double MinSlideOffset = 48;
    private static readonly TimeSpan PhaseDuration = TimeSpan.FromMilliseconds(280);

    /// <summary>Длительность AnimateTabChangeAsync (один параллельный проход).</summary>
    public static TimeSpan TabChangeTotalDuration => PhaseDuration;

    private static bool _running;

    /// <summary>
    /// Вертикальный сдвиг при смене scope Popup: в «Общие» контент поднимается, в «Раздельные» — опускается.
    /// </summary>
    public static async Task AnimateVerticalChangeAsync(
        FrameworkElement? host,
        bool toSeparate,
        Action applyChange)
    {
        ArgumentNullException.ThrowIfNull(applyChange);

        if (host is null || !ShouldAnimate())
        {
            applyChange();
            return;
        }

        if (_running)
        {
            applyChange();
            return;
        }

        _running = true;
        try
        {
            applyChange();

            // «Раздельные»: pivot показан — контент стартует выше и опускается к 0.
            // «Общие»: pivot скрыт — контент стартует ниже и поднимается к 0.
            var enterFrom = toSeparate ? -MinSlideOffset : MinSlideOffset;
            SetVerticalTransform(host, enterFrom, 1);
            await RunVerticalPhaseAsync(host, enterFrom, 0, 1, 1).ConfigureAwait(true);
            ResetVerticalHost(host);
        }
        finally
        {
            _running = false;
        }
    }

    /// <summary>
    /// Одновременное перелистывание ←→: снимок старого контента уезжает, новый въезжает.
    /// Родитель host должен быть <see cref="Grid"/> (клип + overlay).
    /// </summary>
    public static async Task AnimateTabChangeAsync(
        FrameworkElement? host,
        int previousIndex,
        int newIndex,
        Action applyTabChange)
    {
        ArgumentNullException.ThrowIfNull(applyTabChange);

        if (host is null || previousIndex == newIndex || !ShouldAnimate())
        {
            applyTabChange();
            return;
        }

        if (_running)
        {
            applyTabChange();
            return;
        }

        _running = true;
        Image? outgoing = null;
        Grid? root = null;
        try
        {
            var forward = newIndex > previousIndex;
            root = host.Parent as Grid;
            var travel = Math.Max(host.ActualWidth > 1 ? host.ActualWidth : MinSlideOffset * 8, MinSlideOffset);
            var exitTo = forward ? -travel : travel;
            var enterFrom = forward ? travel : -travel;

            if (root is not null && host.ActualWidth > 1 && host.ActualHeight > 1)
            {
                try
                {
                    var bitmap = new RenderTargetBitmap();
                    await bitmap.RenderAsync(host);
                    outgoing = new Image
                    {
                        Source = bitmap,
                        Stretch = Stretch.Fill,
                        Width = host.ActualWidth,
                        Height = host.ActualHeight,
                        HorizontalAlignment = HorizontalAlignment.Left,
                        VerticalAlignment = VerticalAlignment.Top,
                        IsHitTestVisible = false,
                    };
                    root.Children.Add(outgoing);
                    SetTransform(outgoing, 0);
                    // Клип на время слайда (WinUI Grid без ClipToBounds).
                    root.Clip = new RectangleGeometry
                    {
                        Rect = new Windows.Foundation.Rect(0, 0, root.ActualWidth, root.ActualHeight)
                    };
                }
                catch
                {
                    outgoing = null;
                }
            }

            applyTabChange();
            host.UpdateLayout();

            SetTransform(host, enterFrom);

            if (outgoing is not null)
            {
                await RunParallelSlideAsync(
                    outgoing,
                    fromX: 0,
                    toX: exitTo,
                    host,
                    fromX2: enterFrom,
                    toX2: 0).ConfigureAwait(true);
            }
            else
            {
                // Нет снимка — только въезд новой (без «сначала уехать»).
                await RunPhaseAsync(host, enterFrom, 0).ConfigureAwait(true);
            }

            ResetHost(host);
        }
        finally
        {
            if (outgoing is not null && root is not null)
                root.Children.Remove(outgoing);
            if (root is not null)
                root.Clip = null;
            if (host is not null)
                ResetHost(host);
            _running = false;
        }
    }

    private static bool ShouldAnimate()
    {
        try
        {
            return new UISettings().AnimationsEnabled;
        }
        catch
        {
            return true;
        }
    }

    private static CompositeTransform EnsureTransform(FrameworkElement element)
    {
        if (element.RenderTransform is CompositeTransform existing)
            return existing;

        element.RenderTransformOrigin = new Windows.Foundation.Point(0.5, 0.5);
        var transform = new CompositeTransform();
        element.RenderTransform = transform;
        return transform;
    }

    private static void SetTransform(FrameworkElement element, double translateX)
    {
        EnsureTransform(element).TranslateX = translateX;
        element.Opacity = 1;
    }

    private static void ResetHost(FrameworkElement element)
    {
        if (element.RenderTransform is CompositeTransform transform)
            transform.TranslateX = 0;
        element.Opacity = 1;
    }

    private static void SetVerticalTransform(FrameworkElement element, double translateY, double opacity)
    {
        EnsureTransform(element).TranslateY = translateY;
        element.Opacity = opacity;
    }

    private static void ResetVerticalHost(FrameworkElement element)
    {
        if (element.RenderTransform is CompositeTransform transform)
            transform.TranslateY = 0;
        element.Opacity = 1;
    }

    private static Task RunVerticalPhaseAsync(
        FrameworkElement host,
        double fromY,
        double toY,
        double fromOpacity,
        double toOpacity)
    {
        var transform = EnsureTransform(host);
        transform.TranslateY = fromY;
        host.Opacity = fromOpacity;

        var storyboard = new Storyboard();
        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };

        var slide = new DoubleAnimation
        {
            From = fromY,
            To = toY,
            Duration = PhaseDuration,
            EasingFunction = ease
        };
        Storyboard.SetTarget(slide, transform);
        Storyboard.SetTargetProperty(slide, nameof(CompositeTransform.TranslateY));
        storyboard.Children.Add(slide);

        var fade = new DoubleAnimation
        {
            From = fromOpacity,
            To = toOpacity,
            Duration = PhaseDuration,
            EasingFunction = ease
        };
        Storyboard.SetTarget(fade, host);
        Storyboard.SetTargetProperty(fade, nameof(UIElement.Opacity));
        storyboard.Children.Add(fade);

        var tcs = new TaskCompletionSource<bool>();
        storyboard.Completed += (_, _) => tcs.TrySetResult(true);
        storyboard.Begin();
        return tcs.Task;
    }

    private static Task RunPhaseAsync(FrameworkElement host, double fromX, double toX)
    {
        var transform = EnsureTransform(host);
        transform.TranslateX = fromX;

        var storyboard = new Storyboard();
        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };

        var slide = new DoubleAnimation
        {
            From = fromX,
            To = toX,
            Duration = PhaseDuration,
            EasingFunction = ease
        };
        Storyboard.SetTarget(slide, transform);
        Storyboard.SetTargetProperty(slide, nameof(CompositeTransform.TranslateX));
        storyboard.Children.Add(slide);

        var tcs = new TaskCompletionSource<bool>();
        storyboard.Completed += (_, _) => tcs.TrySetResult(true);
        storyboard.Begin();
        return tcs.Task;
    }

    private static Task RunParallelSlideAsync(
        FrameworkElement outgoing,
        double fromX,
        double toX,
        FrameworkElement incoming,
        double fromX2,
        double toX2)
    {
        var outTransform = EnsureTransform(outgoing);
        var inTransform = EnsureTransform(incoming);
        outTransform.TranslateX = fromX;
        inTransform.TranslateX = fromX2;

        var storyboard = new Storyboard();
        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };

        var outSlide = new DoubleAnimation
        {
            From = fromX,
            To = toX,
            Duration = PhaseDuration,
            EasingFunction = ease
        };
        Storyboard.SetTarget(outSlide, outTransform);
        Storyboard.SetTargetProperty(outSlide, nameof(CompositeTransform.TranslateX));
        storyboard.Children.Add(outSlide);

        var inSlide = new DoubleAnimation
        {
            From = fromX2,
            To = toX2,
            Duration = PhaseDuration,
            EasingFunction = ease
        };
        Storyboard.SetTarget(inSlide, inTransform);
        Storyboard.SetTargetProperty(inSlide, nameof(CompositeTransform.TranslateX));
        storyboard.Children.Add(inSlide);

        var tcs = new TaskCompletionSource<bool>();
        storyboard.Completed += (_, _) => tcs.TrySetResult(true);
        storyboard.Begin();
        return tcs.Task;
    }
}
