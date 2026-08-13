using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;
using Windows.UI.ViewManagement;

namespace GlassSettings.Controls;

/// <summary>
/// Анимация тела спойлера: разворот и сворот покадровым MaxHeight + ScaleY + Opacity.
/// </summary>
internal static class ExpanderBodyTransition
{
    private static readonly TimeSpan Duration = TimeSpan.FromMilliseconds(250);
    private const int FrameMilliseconds = 16;

    public static bool ShouldAnimate()
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

    public static Task RunAsync(
        Border body,
        FrameworkElement revealHost,
        FrameworkElement layoutRoot,
        bool expand,
        CancellationToken cancellationToken) =>
        expand
            ? ExpandTopDownAsync(body, revealHost, layoutRoot, cancellationToken)
            : CollapseTopDownAsync(body, revealHost, cancellationToken);

    /// <summary>
    /// Разворот только через MaxHeight + Clip — без ScaleY, контент не сжимается.
    /// </summary>
    public static Task RunSlideAsync(
        Border body,
        FrameworkElement revealHost,
        FrameworkElement layoutRoot,
        bool expand,
        CancellationToken cancellationToken) =>
        expand
            ? ExpandSlideAsync(body, revealHost, layoutRoot, cancellationToken)
            : CollapseSlideAsync(body, revealHost, cancellationToken);

    private static async Task ExpandTopDownAsync(
        Border body,
        FrameworkElement revealHost,
        FrameworkElement layoutRoot,
        CancellationToken cancellationToken)
    {
        var targetHeight = MeasureExpandedHeight(body, revealHost, layoutRoot);
        if (targetHeight <= 0.5)
        {
            FinishExpanded(body, revealHost);
            return;
        }

        body.Visibility = Visibility.Visible;
        body.ClearValue(FrameworkElement.HeightProperty);
        body.MaxHeight = 0;
        body.Opacity = 0;

        var scale = EnsureScaleTransform(revealHost);
        revealHost.RenderTransformOrigin = new Point(0.5, 0);
        scale.ScaleY = 0.001;

        try
        {
            await AnimateProgressAsync(
                    0,
                    1,
                    progress =>
                    {
                        body.MaxHeight = targetHeight * progress;
                        scale.ScaleY = Math.Max(0.001, progress);
                        body.Opacity = progress;
                    },
                    cancellationToken)
                .ConfigureAwait(true);
        }
        catch (OperationCanceledException)
        {
            return;
        }

        if (!cancellationToken.IsCancellationRequested)
            FinishExpanded(body, revealHost);
    }

    private static async Task CollapseTopDownAsync(
        Border body,
        FrameworkElement revealHost,
        CancellationToken cancellationToken)
    {
        var startHeight = body.ActualHeight;
        if (startHeight <= 0.5)
        {
            FinishCollapsed(body, revealHost);
            return;
        }

        body.ClearValue(FrameworkElement.HeightProperty);
        body.MaxHeight = startHeight;
        body.Opacity = 1;

        var scale = EnsureScaleTransform(revealHost);
        revealHost.RenderTransformOrigin = new Point(0.5, 0);
        scale.ScaleY = 1;

        try
        {
            await AnimateProgressAsync(
                    1,
                    0,
                    progress =>
                    {
                        body.MaxHeight = startHeight * progress;
                        scale.ScaleY = Math.Max(0.001, progress);
                        body.Opacity = progress;
                    },
                    cancellationToken)
                .ConfigureAwait(true);
        }
        catch (OperationCanceledException)
        {
            return;
        }

        if (!cancellationToken.IsCancellationRequested)
            FinishCollapsed(body, revealHost);
    }

    private static async Task ExpandSlideAsync(
        Border body,
        FrameworkElement revealHost,
        FrameworkElement layoutRoot,
        CancellationToken cancellationToken)
    {
        _ = revealHost;
        var targetHeight = MeasureExpandedHeight(body, revealHost, layoutRoot);
        if (targetHeight <= 0.5)
        {
            FinishSlideExpanded(body);
            return;
        }

        body.Visibility = Visibility.Visible;
        body.ClearValue(FrameworkElement.HeightProperty);
        body.MaxHeight = 0;
        body.Opacity = 1;

        try
        {
            await AnimateProgressAsync(
                    0,
                    1,
                    progress =>
                    {
                        var height = targetHeight * progress;
                        body.MaxHeight = height;
                        SetSlideClip(body, height);
                    },
                    cancellationToken)
                .ConfigureAwait(true);
        }
        catch (OperationCanceledException)
        {
            return;
        }

        if (!cancellationToken.IsCancellationRequested)
            FinishSlideExpanded(body);
    }

    private static async Task CollapseSlideAsync(
        Border body,
        FrameworkElement revealHost,
        CancellationToken cancellationToken)
    {
        _ = revealHost;
        var startHeight = body.ActualHeight;
        if (startHeight <= 0.5)
        {
            FinishSlideCollapsed(body);
            return;
        }

        body.ClearValue(FrameworkElement.HeightProperty);
        body.MaxHeight = startHeight;
        body.Opacity = 1;

        try
        {
            await AnimateProgressAsync(
                    1,
                    0,
                    progress =>
                    {
                        var height = startHeight * progress;
                        body.MaxHeight = height;
                        SetSlideClip(body, height);
                    },
                    cancellationToken)
                .ConfigureAwait(true);
        }
        catch (OperationCanceledException)
        {
            return;
        }

        if (!cancellationToken.IsCancellationRequested)
            FinishSlideCollapsed(body);
    }

    private static void FinishSlideExpanded(Border body)
    {
        body.ClearValue(FrameworkElement.HeightProperty);
        body.ClearValue(FrameworkElement.MaxHeightProperty);
        ClearSlideClip(body);
        body.Opacity = 1;
        body.Visibility = Visibility.Visible;
    }

    private static void FinishSlideCollapsed(Border body)
    {
        body.Visibility = Visibility.Collapsed;
        body.ClearValue(FrameworkElement.HeightProperty);
        body.ClearValue(FrameworkElement.MaxHeightProperty);
        ClearSlideClip(body);
        body.Opacity = 1;
    }

    private static void SetSlideClip(Border body, double visibleHeight)
    {
        var width = body.ActualWidth;
        if (width <= 0 && body.Parent is FrameworkElement parent)
            width = parent.ActualWidth;

        body.Clip = new RectangleGeometry
        {
            Rect = new Rect(0, 0, Math.Max(width, 1), Math.Max(visibleHeight, 0))
        };
    }

    private static void ClearSlideClip(Border body) => body.Clip = null;

    private static double MeasureExpandedHeight(
        Border body,
        FrameworkElement revealHost,
        FrameworkElement layoutRoot)
    {
        var previousVisibility = body.Visibility;
        body.Visibility = Visibility.Visible;
        body.ClearValue(FrameworkElement.HeightProperty);
        body.MaxHeight = double.PositiveInfinity;
        body.Opacity = 0.01;

        layoutRoot.UpdateLayout();
        body.UpdateLayout();

        var width = ResolveMeasureWidth(body, layoutRoot);
        revealHost.Measure(new Size(width, double.PositiveInfinity));
        var innerHeight = revealHost.DesiredSize.Height;

        if (innerHeight > 0.5)
        {
            var pad = body.Padding;
            var border = body.BorderThickness;
            return innerHeight + pad.Top + pad.Bottom + border.Top + border.Bottom;
        }

        if (body.ActualHeight > 0.5)
            return body.ActualHeight;

        body.Measure(new Size(width, double.PositiveInfinity));
        var measured = body.DesiredSize.Height;
        if (measured > 0.5)
            return measured;

        body.Visibility = previousVisibility;
        return 0;
    }

    private static double ResolveMeasureWidth(Border body, FrameworkElement layoutRoot)
    {
        if (body.ActualWidth > 0)
            return body.ActualWidth;

        if (layoutRoot.ActualWidth > 0)
            return layoutRoot.ActualWidth;

        var node = body.Parent as FrameworkElement;
        while (node is not null)
        {
            if (node.ActualWidth > 0)
                return node.ActualWidth;
            node = node.Parent as FrameworkElement;
        }

        return double.PositiveInfinity;
    }

    private static ScaleTransform EnsureScaleTransform(FrameworkElement element)
    {
        if (element.RenderTransform is ScaleTransform existing)
            return existing;

        var scale = new ScaleTransform { ScaleX = 1, ScaleY = 1 };
        element.RenderTransform = scale;
        return scale;
    }

    private static void FinishExpanded(Border body, FrameworkElement revealHost)
    {
        body.ClearValue(FrameworkElement.HeightProperty);
        body.ClearValue(FrameworkElement.MaxHeightProperty);
        body.Opacity = 1;
        body.Visibility = Visibility.Visible;

        if (revealHost.RenderTransform is ScaleTransform scale)
            scale.ScaleY = 1;
    }

    private static void FinishCollapsed(Border body, FrameworkElement revealHost)
    {
        body.Visibility = Visibility.Collapsed;
        body.ClearValue(FrameworkElement.HeightProperty);
        body.ClearValue(FrameworkElement.MaxHeightProperty);
        body.Opacity = 1;

        if (revealHost.RenderTransform is ScaleTransform scale)
            scale.ScaleY = 1;
    }

    /// <summary>
    /// Покадровая интерполяция — Storyboard не анимирует MaxHeight надёжно в WinUI.
    /// </summary>
    private static async Task AnimateProgressAsync(
        double from,
        double to,
        Action<double> apply,
        CancellationToken cancellationToken)
    {
        var durationMs = Duration.TotalMilliseconds;
        var startTicks = Environment.TickCount64;

        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();

            var elapsed = Environment.TickCount64 - startTicks;
            var linear = durationMs <= 0 ? 1 : Math.Min(1, elapsed / durationMs);
            var eased = 1 - Math.Pow(1 - linear, 3);
            apply(from + (to - from) * eased);

            if (linear >= 1)
                break;

            await Task.Delay(FrameMilliseconds).ConfigureAwait(true);
        }

        apply(to);
    }
}
