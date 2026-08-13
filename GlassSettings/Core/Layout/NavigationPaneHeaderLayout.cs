using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Foundation;

namespace GlassSettings.Core.Layout;

/// <summary>
/// Выравнивает карточку пользователя в pane с верхом фиксированной области контента (PageContentHost).
/// </summary>
public static class NavigationPaneHeaderLayout
{
    private const double PaneContentGap = 8;
    private const double MarginApplyEpsilon = 1;

    public static void Apply(
        NavigationView navigationView,
        FrameworkElement userCardHost,
        FrameworkElement contentHost)
    {
        if (navigationView.ActualWidth <= 0 ||
            !userCardHost.IsLoaded ||
            !contentHost.IsLoaded)
            return;

        try
        {
            var contentTop = contentHost.TransformToVisual(navigationView).TransformPoint(new Point(0, 0)).Y;
            var hostTop = userCardHost.TransformToVisual(navigationView).TransformPoint(new Point(0, 0)).Y;
            var currentMargin = userCardHost.Margin;
            var naturalHostTop = hostTop - currentMargin.Top;
            var targetMarginTop = Math.Max(0, Math.Round(contentTop - naturalHostTop));

            if (Math.Abs(targetMarginTop - currentMargin.Top) < MarginApplyEpsilon)
                return;

            userCardHost.Margin = new Thickness(0, targetMarginTop, PaneContentGap, 0);
        }
        catch
        {
            var fallback = userCardHost.Margin;
            if (fallback.Top != 0 || fallback.Right != PaneContentGap)
                userCardHost.Margin = new Thickness(0, 0, PaneContentGap, 0);
        }
    }
}
