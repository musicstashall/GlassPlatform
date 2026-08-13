using GlassSettings.Strings;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Core.Flyout;

public static class FlyoutAnimNames
{
    private const string PopInStyleItemsResourceKey = "Combo_PopInStyleItems";

    public static string GetPopInStyle(uint index)
    {
        var items = GlassSettingsResources.GetSemicolonList(PopInStyleItemsResourceKey);
        if (index < items.Length)
            return items[index];
        return index.ToString();
    }

    public static void PopulatePopInStyleCombo(ComboBox combo)
    {
        combo.Items.Clear();
        foreach (var item in GlassSettingsResources.GetSemicolonList(PopInStyleItemsResourceKey))
            combo.Items.Add(item);
    }
}
