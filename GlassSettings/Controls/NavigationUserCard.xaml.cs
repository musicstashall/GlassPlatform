using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Controls;

public sealed partial class NavigationUserCard : UserControl
{
    public NavigationUserCard()
    {
        InitializeComponent();
        ApplyHorizontalOffset();
        Loaded += (_, _) => Refresh();
    }

    private void ApplyHorizontalOffset()
    {
        if (Application.Current.Resources.TryGetValue("NavigationUserCardHorizontalOffset", out var value) &&
            value is double offset &&
            offset != 0)
        {
            RenderTransform = new TranslateTransform { X = offset };
        }
    }

    private void Refresh()
    {
        var displayName = Environment.UserName;
        var accountLabel = GlassSettingsResources.Get("NavUser_LocalAccount");

        try
        {
            var full = Environment.GetEnvironmentVariable("USERNAME");
            if (!string.IsNullOrWhiteSpace(full))
                displayName = full;
        }
        catch
        {
            /* ignore */
        }

        DisplayNameText.Text = displayName;
        AccountTypeText.Text = accountLabel;
        InitialsText.Text = MakeInitials(displayName);
    }

    private static string MakeInitials(string name)
    {
        if (string.IsNullOrWhiteSpace(name))
            return "?";

        var parts = name.Split(' ', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        if (parts.Length >= 2)
            return $"{char.ToUpperInvariant(parts[0][0])}{char.ToUpperInvariant(parts[^1][0])}";
        return char.ToUpperInvariant(name[0]).ToString();
    }

    private void RootBorder_OnPointerEntered(object sender, PointerRoutedEventArgs e) =>
        VisualStateManager.GoToState(this, "PointerOver", true);

    private void RootBorder_OnPointerExited(object sender, PointerRoutedEventArgs e) =>
        VisualStateManager.GoToState(this, "Normal", true);
}
