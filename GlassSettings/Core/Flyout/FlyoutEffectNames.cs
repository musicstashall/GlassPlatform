using GlassSettings.Strings;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Core.Flyout;

public static class FlyoutEffectNames
{
    private const string ComboItemsResourceKey = "Combo_EffectTypeItems";
    private const string NamesResourceKey = "Combo_EffectTypeNames";

    public static string Get(uint index)
    {
        var names = GlassSettingsResources.GetSemicolonList(NamesResourceKey);
        if (index < names.Length)
            return names[index];
        return GlassSettingsResources.Format("Effect_Unknown", index);
    }

    public static void PopulateCombo(ComboBox combo)
    {
        combo.Items.Clear();
        foreach (var item in GlassSettingsResources.GetSemicolonList(ComboItemsResourceKey))
            combo.Items.Add(item);
    }
}
