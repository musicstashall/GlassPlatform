using GlassSettings.Strings;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Core.Text;

public static class InkRenderModeNames
{
    private const string ComboItemsResourceKey = "Combo_InkRenderModeItems";

    public static void PopulateCombo(ComboBox combo)
    {
        combo.Items.Clear();
        foreach (var item in GlassSettingsResources.GetSemicolonList(ComboItemsResourceKey))
            combo.Items.Add(item);
    }
}
