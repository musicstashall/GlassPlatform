using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Core.Layout;

/// <summary>
/// Поиск в title bar (Medium/Wide) или под карточкой пользователя в overlay-pane (Narrow + pane open).
/// </summary>
public static class NavigationSearchLayout
{
    public static bool Apply(
        SettingsLayoutMode mode,
        bool isNavPaneOpen,
        AutoSuggestBox searchBox,
        TitleBar titleBar,
        Panel navPaneSearchHost)
    {
        var changed = false;

        if (mode == SettingsLayoutMode.Narrow)
        {
            changed |= MoveToNavPane(searchBox, titleBar, navPaneSearchHost);
            var visible = isNavPaneOpen ? Visibility.Visible : Visibility.Collapsed;
            if (navPaneSearchHost.Visibility != visible)
            {
                navPaneSearchHost.Visibility = visible;
                changed = true;
            }
        }
        else
        {
            changed |= MoveToTitleBar(searchBox, titleBar, navPaneSearchHost);
            if (navPaneSearchHost.Visibility != Visibility.Collapsed)
            {
                navPaneSearchHost.Visibility = Visibility.Collapsed;
                changed = true;
            }
        }

        return changed;
    }

    private static bool MoveToTitleBar(
        AutoSuggestBox searchBox,
        TitleBar titleBar,
        Panel navPaneSearchHost)
    {
        if (ReferenceEquals(titleBar.Content, searchBox))
        {
            ConfigureTitleBarSearch(searchBox);
            return false;
        }

        navPaneSearchHost.Children.Remove(searchBox);
        titleBar.Content = searchBox;
        ConfigureTitleBarSearch(searchBox);
        return true;
    }

    private static bool MoveToNavPane(
        AutoSuggestBox searchBox,
        TitleBar titleBar,
        Panel navPaneSearchHost)
    {
        if (navPaneSearchHost.Children.Contains(searchBox))
        {
            ConfigureNavPaneSearch(searchBox);
            return false;
        }

        titleBar.Content = null;
        navPaneSearchHost.Children.Add(searchBox);
        ConfigureNavPaneSearch(searchBox);
        return true;
    }

    private static void ConfigureTitleBarSearch(AutoSuggestBox searchBox)
    {
        searchBox.Width = SettingsWindowLayout.TitleSearchWidthDefault;
        searchBox.HorizontalAlignment = HorizontalAlignment.Right;
        searchBox.VerticalAlignment = VerticalAlignment.Center;
        searchBox.Margin = new Thickness(0);
    }

    private static void ConfigureNavPaneSearch(AutoSuggestBox searchBox)
    {
        searchBox.ClearValue(FrameworkElement.WidthProperty);
        searchBox.HorizontalAlignment = HorizontalAlignment.Stretch;
        searchBox.VerticalAlignment = VerticalAlignment.Top;
        searchBox.Margin = new Thickness(0);
    }
}
