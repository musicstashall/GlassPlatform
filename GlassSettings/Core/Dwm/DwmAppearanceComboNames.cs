using GlassSettings.Strings;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Core.Dwm;

/// <summary>Semicolon-списки комбобоксов страницы «Внешний вид» (паритет OpenGlass Appearance).</summary>
public static class DwmAppearanceComboNames
{
    public const string BlurOptimizationItems = "Combo_BlurOptimizationItems";
    public const string RoundRectProfileItems = "Combo_RoundRectProfileItems";
    public const string CaptionButtonsItems = "Combo_CaptionButtonsItems";
    public const string CenterCaptionItems = "Combo_CenterCaptionItems";
    public const string TextGlowModeItems = "Combo_TextGlowModeItems";
    public const string CaptionColorModeItems = "Combo_CaptionColorModeItems";

    public static void PopulateCombo(ComboBox combo, string resourceKey)
    {
        combo.Items.Clear();
        foreach (var item in GlassSettingsResources.GetSemicolonList(resourceKey))
            combo.Items.Add(item);
    }

    public static void PopulateBlurOptimization(ComboBox combo) =>
        PopulateCombo(combo, BlurOptimizationItems);

    public static void PopulateRoundRectProfile(ComboBox combo) =>
        PopulateCombo(combo, RoundRectProfileItems);

    public static void PopulateCaptionButtons(ComboBox combo) =>
        PopulateCombo(combo, CaptionButtonsItems);

    public static void PopulateCenterCaption(ComboBox combo) =>
        PopulateCombo(combo, CenterCaptionItems);

    public static void PopulateTextGlowMode(ComboBox combo) =>
        PopulateCombo(combo, TextGlowModeItems);

    public static void PopulateCaptionColorMode(ComboBox combo) =>
        PopulateCombo(combo, CaptionColorModeItems);
}
