using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media.Animation;

namespace GlassSettings.Services;

/// <summary>Проявление hosted-панели: opacity 0→1 за 250 мс.</summary>
internal static class HostedDialogRevealAnimator
{
    public const int DurationMs = 250;

    public static void PrepareHidden(UIElement target) => target.Opacity = 0;

    public static void ResetVisible(UIElement target) => target.Opacity = 1;

    public static Task RevealAsync(UIElement target)
    {
        target.Opacity = 0;

        var storyboard = new Storyboard();
        var fade = new DoubleAnimation
        {
            From = 0,
            To = 1,
            Duration = TimeSpan.FromMilliseconds(DurationMs),
            EnableDependentAnimation = true
        };
        Storyboard.SetTarget(fade, target);
        Storyboard.SetTargetProperty(fade, nameof(UIElement.Opacity));
        storyboard.Children.Add(fade);

        return RunStoryboardAsync(storyboard);
    }

    private static Task RunStoryboardAsync(Storyboard storyboard)
    {
        var tcs = new TaskCompletionSource<bool>();
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
