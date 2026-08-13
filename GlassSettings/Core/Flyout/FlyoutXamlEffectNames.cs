using GlassSettings.Strings;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Core.Flyout;

/// <summary>Имена эффектов XAML-пути панели задач (Tray\Xaml\EffectType, 0–5).</summary>
public static class FlyoutXamlEffectNames
{
    private const string ComboItemsResourceKey = "Combo_XamlEffectTypeItems";
    private const string NamesResourceKey = "Combo_XamlEffectTypeNames";

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
