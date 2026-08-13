using System.Numerics;
using Microsoft.UI.Composition;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace GlassSettings.Core.Layout;

/// <summary>
/// Composition opacity-mask на viewport ScrollContentPresenter: края контента растворяются в Mica.
/// Redirect — sibling child visual на viewport host, выровненный по presenter (не на самом presenter).
/// </summary>
internal static class SettingsScrollEdgeFade
{
    private const double FadeHeightPx = 28;

    private static readonly Color VisibleMask = Color.FromArgb(255, 255, 255, 255);
    private static readonly Color HiddenMask = Color.FromArgb(0, 255, 255, 255);

    public static void Attach(ScrollViewer scroll)
    {
        var state = new FadeState(scroll);

        scroll.SizeChanged += (_, _) => Update(state);
        scroll.Loaded += (_, _) => Update(state);
        scroll.Unloaded += (_, _) => Detach(state);
    }

    private sealed class FadeState(ScrollViewer scroll)
    {
        public ScrollViewer Scroll { get; } = scroll;
        public ScrollContentPresenter? Presenter;
        public UIElement? ViewportHost;
        public CompositionLinearGradientBrush? GradientMask;
        public CompositionColorGradientStop? TopOuter;
        public CompositionColorGradientStop? TopInner;
        public CompositionColorGradientStop? BottomInner;
        public CompositionColorGradientStop? BottomOuter;
        public CompositionMaskBrush? MaskBrush;
        public bool RedirectActive;
    }

    private static void Detach(FadeState state)
    {
        TeardownRedirect(state);
        state.GradientMask = null;
        state.RedirectActive = false;
    }

    private static void Update(FadeState state)
    {
        if (!EnsureTargets(state))
            return;

        ApplyGradient(state);
        EnsureRedirect(state);
    }

    private static bool EnsureTargets(FadeState state)
    {
        if (state.Presenter is not null && state.ViewportHost is not null)
            return true;

        state.Presenter = FindScrollContentPresenter(state.Scroll);
        if (state.Presenter is null)
            return false;

        state.ViewportHost = VisualTreeHelper.GetParent(state.Presenter) as UIElement;
        return state.ViewportHost is not null;
    }

    private static void ApplyGradient(FadeState state)
    {
        if (state.GradientMask is null
            || state.TopOuter is null
            || state.TopInner is null
            || state.BottomInner is null
            || state.BottomOuter is null
            || state.Presenter is null)
            return;

        var height = state.Presenter.ActualHeight;
        if (height <= 0)
            return;

        var fadeRatio = (float)Math.Min(FadeHeightPx / height, 0.5);

        state.TopOuter.Offset = 0;
        state.TopOuter.Color = HiddenMask;
        state.TopInner.Offset = fadeRatio;
        state.TopInner.Color = VisibleMask;
        state.BottomInner.Offset = 1 - fadeRatio;
        state.BottomInner.Color = VisibleMask;
        state.BottomOuter.Offset = 1;
        state.BottomOuter.Color = HiddenMask;
    }

    private static void EnsureRedirect(FadeState state)
    {
        if (state.RedirectActive
            || state.Presenter is null
            || state.ViewportHost is null)
            return;

        var presenterVisual = ElementCompositionPreview.GetElementVisual(state.Presenter);
        var compositor = presenterVisual.Compositor;

        state.GradientMask = compositor.CreateLinearGradientBrush();
        state.GradientMask.StartPoint = new Vector2(0, 0);
        state.GradientMask.EndPoint = new Vector2(0, 1);
        state.GradientMask.MappingMode = CompositionMappingMode.Relative;

        state.TopOuter = compositor.CreateColorGradientStop(0, HiddenMask);
        state.TopInner = compositor.CreateColorGradientStop(0, VisibleMask);
        state.BottomInner = compositor.CreateColorGradientStop(1, VisibleMask);
        state.BottomOuter = compositor.CreateColorGradientStop(1, HiddenMask);
        state.GradientMask.ColorStops.Add(state.TopOuter);
        state.GradientMask.ColorStops.Add(state.TopInner);
        state.GradientMask.ColorStops.Add(state.BottomInner);
        state.GradientMask.ColorStops.Add(state.BottomOuter);

        state.MaskBrush = compositor.CreateMaskBrush();
        state.MaskBrush.Source = CreateSourceBrush(state.Presenter);
        state.MaskBrush.Mask = state.GradientMask;

        var redirectVisual = compositor.CreateSpriteVisual();
        redirectVisual.Brush = state.MaskBrush;

        var sizeAnimation = compositor.CreateExpressionAnimation("presenter.Size");
        sizeAnimation.SetReferenceParameter("presenter", presenterVisual);
        redirectVisual.StartAnimation(nameof(redirectVisual.Size), sizeAnimation);

        var offsetAnimation = compositor.CreateExpressionAnimation("presenter.Offset");
        offsetAnimation.SetReferenceParameter("presenter", presenterVisual);
        redirectVisual.StartAnimation(nameof(redirectVisual.Offset), offsetAnimation);

        ElementCompositionPreview.SetElementChildVisual(state.ViewportHost, redirectVisual);

        state.RedirectActive = true;
        ApplyGradient(state);
    }

    private static void TeardownRedirect(FadeState state)
    {
        if (!state.RedirectActive)
            return;

        if (state.Presenter is not null)
            ElementCompositionPreview.GetElementVisual(state.Presenter).Opacity = 1;

        if (state.ViewportHost is not null)
            ElementCompositionPreview.SetElementChildVisual(state.ViewportHost, null);

        state.MaskBrush = null;
        state.RedirectActive = false;
    }

    private static CompositionSurfaceBrush CreateSourceBrush(ScrollContentPresenter presenter)
    {
        var visual = ElementCompositionPreview.GetElementVisual(presenter);
        var compositor = visual.Compositor;

        var visualSurface = compositor.CreateVisualSurface();
        visualSurface.SourceVisual = visual;

        var sourceSizeAnimation = compositor.CreateExpressionAnimation($"{nameof(visual)}.Size");
        sourceSizeAnimation.SetReferenceParameter(nameof(visual), visual);
        visualSurface.StartAnimation(nameof(visualSurface.SourceSize), sourceSizeAnimation);

        var brush = compositor.CreateSurfaceBrush(visualSurface);
        brush.Stretch = CompositionStretch.Fill;
        brush.HorizontalAlignmentRatio = 0;
        brush.VerticalAlignmentRatio = 0;
        visual.Opacity = 0;
        return brush;
    }

    private static ScrollContentPresenter? FindScrollContentPresenter(DependencyObject root)
    {
        if (root is ScrollContentPresenter presenter)
            return presenter;

        var count = VisualTreeHelper.GetChildrenCount(root);
        for (var i = 0; i < count; i++)
        {
            var found = FindScrollContentPresenter(VisualTreeHelper.GetChild(root, i));
            if (found is not null)
                return found;
        }

        return null;
    }
}
