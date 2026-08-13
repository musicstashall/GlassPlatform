using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Windows.UI.ViewManagement;

namespace GlassSettings.Controls;

/// <summary>Crossfade между двумя UIElement (opacity 0↔1).</summary>
internal sealed class UiElementCrossfade
{
    private readonly int _fadeDurationMs;

    private readonly UIElement _layerA;
    private readonly UIElement _layerB;
    private Storyboard? _storyboard;
    private TaskCompletionSource<bool>? _activeTcs;
    private int _generation;
    private bool _layerAIsFront = true;

    public UiElementCrossfade(UIElement layerA, UIElement layerB, int fadeDurationMs = 500)
    {
        _layerA = layerA;
        _layerB = layerB;
        _fadeDurationMs = fadeDurationMs;
    }

    public bool IsAnimating => _storyboard is not null;

    public Task ShowFrontAsync(bool layerAIsFront, bool animate) =>
        animate ? ShowFrontAnimatedAsync(layerAIsFront) : ShowFrontImmediateAsync(layerAIsFront);

    public void ShowFront(bool layerAIsFront, bool animate)
    {
        if (animate)
            _ = ShowFrontAnimatedAsync(layerAIsFront);
        else
            ShowFrontImmediate(layerAIsFront);
    }

    private Task ShowFrontImmediateAsync(bool layerAIsFront)
    {
        ShowFrontImmediate(layerAIsFront);
        return Task.CompletedTask;
    }

    private void ShowFrontImmediate(bool layerAIsFront)
    {
        StopActiveAnimation(snapToLogicalFront: true);
        CompletePendingTask();
        SnapToFront(layerAIsFront);
    }

    private Task ShowFrontAnimatedAsync(bool layerAIsFront)
    {
        if (_layerAIsFront == layerAIsFront && IsFrontLayerVisible(layerAIsFront) && !IsAnimating)
            return Task.CompletedTask;

        StopActiveAnimation(snapToLogicalFront: true);
        CompletePendingTask();

        if (_layerAIsFront == layerAIsFront)
        {
            EnsureFrontVisible(layerAIsFront);
            return Task.CompletedTask;
        }

        if (!ShouldAnimate())
        {
            SnapToFront(layerAIsFront);
            return Task.CompletedTask;
        }

        var tcs = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        _activeTcs = tcs;
        var generation = ++_generation;

        var front = layerAIsFront ? _layerA : _layerB;
        var back = layerAIsFront ? _layerB : _layerA;
        BeginCrossfade(front, back, generation, layerAIsFront, tcs);
        return tcs.Task;
    }

    private void StopActiveAnimation(bool snapToLogicalFront)
    {
        if (_storyboard is null)
            return;

        _storyboard.Stop();
        _storyboard = null;
        if (snapToLogicalFront)
            SnapToFront(_layerAIsFront);
    }

    private void CompletePendingTask()
    {
        if (_activeTcs is not { } tcs)
            return;

        _activeTcs = null;
        tcs.TrySetResult(true);
    }

    private void SnapToFront(bool layerAIsFront)
    {
        var front = layerAIsFront ? _layerA : _layerB;
        var back = layerAIsFront ? _layerB : _layerA;
        front.Opacity = 1;
        front.Visibility = Visibility.Visible;
        back.Opacity = 0;
        back.Visibility = Visibility.Collapsed;
        _layerAIsFront = layerAIsFront;
    }

    private static bool IsFrontLayerVisible(bool layerAIsFront, UIElement layerA, UIElement layerB)
    {
        var front = layerAIsFront ? layerA : layerB;
        return front.Visibility == Visibility.Visible && front.Opacity >= 0.99;
    }

    private bool IsFrontLayerVisible(bool layerAIsFront) =>
        IsFrontLayerVisible(layerAIsFront, _layerA, _layerB);

    private void EnsureFrontVisible(bool layerAIsFront)
    {
        if (IsFrontLayerVisible(layerAIsFront))
            return;

        SnapToFront(layerAIsFront);
    }

    private void BeginCrossfade(
        UIElement incoming,
        UIElement outgoing,
        int generation,
        bool layerAIsFront,
        TaskCompletionSource<bool> tcs)
    {
        incoming.Visibility = Visibility.Visible;
        incoming.Opacity = 0;

        var easing = new CubicEase { EasingMode = EasingMode.EaseInOut };
        var fadeOut = new DoubleAnimation
        {
            From = 1,
            To = 0,
            Duration = TimeSpan.FromMilliseconds(_fadeDurationMs),
            EasingFunction = easing
        };
        var fadeIn = new DoubleAnimation
        {
            From = 0,
            To = 1,
            Duration = TimeSpan.FromMilliseconds(_fadeDurationMs),
            EasingFunction = easing
        };

        Storyboard.SetTarget(fadeOut, outgoing);
        Storyboard.SetTargetProperty(fadeOut, nameof(UIElement.Opacity));
        Storyboard.SetTarget(fadeIn, incoming);
        Storyboard.SetTargetProperty(fadeIn, nameof(UIElement.Opacity));

        var storyboard = new Storyboard();
        storyboard.Children.Add(fadeOut);
        storyboard.Children.Add(fadeIn);
        storyboard.Completed += (_, _) =>
        {
            if (generation != _generation)
                return;

            outgoing.Opacity = 0;
            outgoing.Visibility = Visibility.Collapsed;
            incoming.Opacity = 1;
            _layerAIsFront = layerAIsFront;
            _storyboard = null;
            if (ReferenceEquals(_activeTcs, tcs))
            {
                _activeTcs = null;
                tcs.TrySetResult(true);
            }
        };

        _storyboard = storyboard;
        storyboard.Begin();
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
}
