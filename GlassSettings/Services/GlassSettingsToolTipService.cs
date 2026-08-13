using Microsoft.UI.Composition.SystemBackdrops;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;

namespace GlassSettings.Services;

/// <summary>
/// ToolTip с acrylic popup-backdrop. Backdrop вешается на Loaded (до показа), без смены фона на Opened.
/// </summary>
internal static class GlassSettingsToolTipService
{
    private static readonly DependencyProperty AcrylicHookProperty =
        DependencyProperty.RegisterAttached(
            "AcrylicHook",
            typeof(bool),
            typeof(GlassSettingsToolTipService),
            new PropertyMetadata(false));

    public static void SetToolTip(DependencyObject element, object? content)
    {
        if (content is null)
        {
            ToolTipService.SetToolTip(element, null);
            return;
        }

        if (content is string text)
        {
            if (string.IsNullOrEmpty(text))
            {
                ToolTipService.SetToolTip(element, null);
                return;
            }

            var textToolTip = CreateToolTip(text);
            ToolTipService.SetToolTip(element, textToolTip);
            return;
        }

        if (content is ToolTip existingToolTip)
        {
            EnsureAcrylicHook(existingToolTip);
            ToolTipService.SetToolTip(element, existingToolTip);
            return;
        }

        ToolTipService.SetToolTip(element, content);
    }

    private static ToolTip CreateToolTip(string text)
    {
        var toolTip = new ToolTip
        {
            Content = text,
            ContentTransitions = new TransitionCollection()
        };
        EnsureAcrylicHook(toolTip);
        return toolTip;
    }

    private static void EnsureAcrylicHook(ToolTip toolTip)
    {
        if (toolTip.GetValue(AcrylicHookProperty) is true)
            return;

        toolTip.SetValue(AcrylicHookProperty, true);
        toolTip.ContentTransitions ??= new TransitionCollection();
        toolTip.Loaded += OnToolTipLoaded;
        toolTip.Opened += OnToolTipOpened;
        toolTip.Closed += OnToolTipClosed;
    }

    private static void OnToolTipLoaded(object sender, RoutedEventArgs e)
    {
        if (sender is ToolTip toolTip)
            TryApplyAcrylic(toolTip);
    }

    private static void OnToolTipOpened(object sender, object e)
    {
        if (sender is ToolTip toolTip)
            ApplyAcrylicFallback(toolTip);
    }

    private static void OnToolTipClosed(object sender, object e)
    {
        if (sender is ToolTip toolTip)
            RestoreToolTipBackground(toolTip);
    }

    private static void ApplyAcrylicFallback(ToolTip toolTip)
    {
        if (TryApplyAcrylic(toolTip))
            return;

        var queue = toolTip.DispatcherQueue;
        if (queue is null)
            return;

        queue.TryEnqueue(DispatcherQueuePriority.High, () =>
        {
            if (!TryApplyAcrylic(toolTip))
                RestoreToolTipBackground(toolTip);
        });
    }

    private static bool TryApplyAcrylic(ToolTip toolTip)
    {
        var popup = FindPopupForToolTip(toolTip);
        if (popup is null)
            return false;

        try
        {
            popup.ShouldConstrainToRootBounds = false;

            if (popup.SystemBackdrop is null)
                popup.SystemBackdrop = new DesktopAcrylicBackdrop();

            if (popup.SystemBackdrop is not null)
            {
                toolTip.Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
                return true;
            }

            RestoreToolTipBackground(toolTip);
            return false;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings ToolTip acrylic: {ex}");
            RestoreToolTipBackground(toolTip);
            return false;
        }
    }

    private static void RestoreToolTipBackground(ToolTip toolTip)
    {
        if (Application.Current.Resources["AcrylicInAppFillColorDefaultBrush"] is Brush brush)
            toolTip.Background = brush;
    }

    private static Popup? FindPopupForToolTip(ToolTip toolTip)
    {
        if (VisualTreeHelper.GetParent(toolTip) is Popup parentPopup)
            return parentPopup;

        var xamlRoot = toolTip.XamlRoot;
        if (xamlRoot is null)
            return null;

        foreach (var popup in VisualTreeHelper.GetOpenPopupsForXamlRoot(xamlRoot))
        {
            if (ReferenceEquals(popup.Child, toolTip))
                return popup;
        }

        return null;
    }
}
