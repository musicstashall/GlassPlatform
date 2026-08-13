using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Microsoft.UI.Xaml.Media.Imaging;

namespace GlassSettings.Controls;

/// <summary>Crossfade между двумя слоями Image при смене обоев.</summary>
internal sealed class WallpaperPreviewCrossfade
{
    /// <summary>Единая длительность crossfade для всех превью с обоями (hero, карточки, каскады).</summary>
    public const int DefaultCrossfadeDurationMs = 500;

    private readonly int _fadeDurationMs;
    private readonly Image _layerA;
    private readonly Image _layerB;
    private readonly UIElement _fallback;
    private Storyboard? _storyboard;
    private int _generation;
    private bool _layerAIsFront = true;
    private bool _hasImage;

    public WallpaperPreviewCrossfade(
        Image layerA,
        Image layerB,
        UIElement fallback,
        int fadeDurationMs = DefaultCrossfadeDurationMs)
    {
        _layerA = layerA;
        _layerB = layerB;
        _fallback = fallback;
        _fadeDurationMs = fadeDurationMs;
    }

    public void Apply(ImageSource? source, bool animate)
    {
        _storyboard?.Stop();
        _generation++;

        if (source is null)
        {
            _hasImage = false;
            _layerA.Source = null;
            _layerB.Source = null;
            _layerA.Opacity = 0;
            _layerB.Opacity = 0;
            _layerA.Visibility = Visibility.Collapsed;
            _layerB.Visibility = Visibility.Collapsed;
            _fallback.Visibility = Visibility.Visible;
            return;
        }

        if (!_hasImage || !animate)
        {
            if (IsSourceReady(source))
            {
                _fallback.Visibility = Visibility.Collapsed;
                ShowFront(source);
                return;
            }

            ShowFrontPendingDecode(source);
            return;
        }

        _fallback.Visibility = Visibility.Collapsed;

        var incoming = _layerAIsFront ? _layerB : _layerA;
        var outgoing = _layerAIsFront ? _layerA : _layerB;
        var generation = _generation;

        incoming.Visibility = Visibility.Visible;
        incoming.Opacity = 0;

        RoutedEventHandler? onOpened = null;
        onOpened = (_, _) =>
        {
            incoming.ImageOpened -= onOpened;
            BeginCrossfade(incoming, outgoing, generation);
        };

        incoming.ImageOpened += onOpened;
        incoming.Source = source;
        if (IsSourceReady(source))
        {
            incoming.ImageOpened -= onOpened;
            BeginCrossfade(incoming, outgoing, generation);
        }
    }

    private void ShowFrontPendingDecode(ImageSource source)
    {
        var front = _layerAIsFront ? _layerA : _layerB;
        var back = _layerAIsFront ? _layerB : _layerA;
        var generation = _generation;

        back.Source = null;
        back.Opacity = 0;
        back.Visibility = Visibility.Collapsed;

        front.Visibility = Visibility.Visible;
        front.Opacity = 0;
        _fallback.Visibility = Visibility.Visible;
        _hasImage = true;

        if (front.Source == source && IsSourceReady(source))
        {
            CompleteFirstShow(front, generation);
            return;
        }

        RoutedEventHandler? onOpened = null;
        ExceptionRoutedEventHandler? onFailed = null;

        onOpened = (_, _) =>
        {
            front.ImageOpened -= onOpened;
            if (onFailed is not null)
                front.ImageFailed -= onFailed;
            CompleteFirstShow(front, generation);
        };

        onFailed = (_, _) =>
        {
            front.ImageOpened -= onOpened;
            front.ImageFailed -= onFailed;
            if (generation != _generation)
                return;

            front.Source = null;
            front.Visibility = Visibility.Collapsed;
            _hasImage = false;
            _fallback.Visibility = Visibility.Visible;
        };

        front.ImageOpened += onOpened;
        front.ImageFailed += onFailed;
        front.Source = source;
    }

    private void CompleteFirstShow(Image front, int generation)
    {
        if (generation != _generation)
            return;

        _fallback.Visibility = Visibility.Collapsed;
        front.Opacity = 1;
        front.Visibility = Visibility.Visible;
    }

    private void ShowFront(ImageSource source)
    {
        _hasImage = true;
        var front = _layerAIsFront ? _layerA : _layerB;
        var back = _layerAIsFront ? _layerB : _layerA;
        front.Source = source;
        front.Opacity = 1;
        front.Visibility = Visibility.Visible;
        back.Source = null;
        back.Opacity = 0;
        back.Visibility = Visibility.Collapsed;
    }

    private static bool IsSourceReady(ImageSource source) =>
        source switch
        {
            BitmapImage { PixelWidth: > 0 } => true,
            WriteableBitmap { PixelWidth: > 0 } => true,
            _ => false,
        };

    private void BeginCrossfade(Image incoming, Image outgoing, int generation)
    {
        if (generation != _generation)
            return;

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

            outgoing.Source = null;
            outgoing.Opacity = 0;
            outgoing.Visibility = Visibility.Collapsed;
            incoming.Opacity = 1;
            _layerAIsFront = incoming == _layerA;
            _storyboard = null;
        };

        _storyboard = storyboard;
        storyboard.Begin();
    }
}
