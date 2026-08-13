using GlassSettings.Strings;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Core.Flyout;

public static class FlyoutCornerNames
{
    private const string ItemsResourceKey = "Combo_CornerTypeItems";
    private const string XamlItemsResourceKey = "Combo_XamlCornerTypeItems";

    public static string Get(uint index, bool xamlPath = false)
    {
        var key = xamlPath ? XamlItemsResourceKey : ItemsResourceKey;
        var items = GlassSettingsResources.GetSemicolonList(key);
        if (index < items.Length)
            return items[index];
        return index.ToString();
    }

    public static void PopulateCombo(ComboBox combo, bool xamlPath = false)
    {
        combo.Items.Clear();
        var key = xamlPath ? XamlItemsResourceKey : ItemsResourceKey;
        foreach (var item in GlassSettingsResources.GetSemicolonList(key))
            combo.Items.Add(item);
    }
}
