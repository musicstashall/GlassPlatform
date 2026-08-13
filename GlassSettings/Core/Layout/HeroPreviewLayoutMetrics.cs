using GlassSettings.Controls;
using GlassSettings.Services;
using Microsoft.UI.Xaml;

namespace GlassSettings.Core.Layout;

public static class HeroPreviewLayoutMetrics
{
    public static double OuterWidth =>
        Application.Current.Resources["GlassSettingsHeroPreviewOuterWidth"] is double width ? width : 340;

    public static double OuterHeight =>
        Application.Current.Resources["GlassSettingsHeroPreviewOuterHeight"] is double height ? height : 192;

    /// <summary>Blur/text-слой hero (меньше обоев на Inset).</summary>
    public static double TextLayerOuterWidth =>
        Application.Current.Resources["GlassSettingsTextLayerPreviewOuterWidth"] is double width
            ? width
            : CompositionWallpaperPipelineBuilder.BlurLayerWidth;

    /// <summary>Blur/text-слой hero (меньше обоев на Inset).</summary>
    public static double TextLayerOuterHeight =>
        Application.Current.Resources["GlassSettingsTextLayerPreviewOuterHeight"] is double height
            ? height
            : CompositionWallpaperPipelineBuilder.BlurLayerHeight;

    /// <summary>Размер bitmap текстового overlay hero (blur − 2×padding).</summary>
    public static int TextOverlayPaintWidth =>
        CompositionWallpaperPipelineBuilder.BlurLayerWidth - (DwmCompositionCascadePreview.TextOverlayPadding * 2);

    /// <summary>Размер bitmap текстового overlay hero (blur − 2×padding).</summary>
    public static int TextOverlayPaintHeight =>
        CompositionWallpaperPipelineBuilder.BlurLayerHeight - (DwmCompositionCascadePreview.TextOverlayPadding * 2);

    /// <summary>Ширина hero outer с учётом правого margin под тень.</summary>
    public static double FootprintWidth => OuterWidth + ShadowMargin.Right;

    /// <summary>Высота hero outer с учётом нижнего margin под тень.</summary>
    public static double FootprintHeight => OuterHeight + ShadowMargin.Bottom;

    /// <summary>Ширина text/blur-слоя карточки мастера с margin под тень.</summary>
    public static double TextLayerFootprintWidth => TextLayerOuterWidth + ShadowMargin.Right;

    /// <summary>Высота text/blur-слоя карточки мастера с margin под тень.</summary>
    public static double TextLayerFootprintHeight => TextLayerOuterHeight + ShadowMargin.Bottom;

    public static Thickness ShadowMargin =>
        Application.Current.Resources["GlassSettingsHeroPreviewShadowMargin"] is Thickness shadow
            ? shadow
            : new Thickness(0, 0, 8, 8);

    public const double WizardCardSpacing = 16;

    /// <summary>Запас по горизонтали ряда: ThemeShadow + Translation выходит за footprint.</summary>
    public const double WizardCardRowShadowSpill = 16;

    /// <summary>Запас сверху первого ряда.</summary>
    public const double WizardCardRowShadowSpillVerticalTop = 12;

    /// <summary>Запас снизу нижнего ряда — только для clip/overflow, не в layout-высоту списка.</summary>
    public const double WizardCardRowShadowSpillBottom = 32;

    public const int WizardBottomPresetColumns = 3;

    public static double CardFootprintHeight => TextLayerFootprintHeight;

    /// <summary>Доп. высота под ThemeShadow при hover/selection (Translation Z).</summary>
    public const double WizardCardRowInteractiveShadowSpillBottom = 16;

    /// <summary>Высота paired-списка с запасом под интерактивную тень снизу.</summary>
    public static double TieredTwoRowViewportHeight =>
        WizardCardRowShadowSpillVerticalTop
        + TextLayerFootprintHeight
        + WizardCardRowInteractiveShadowSpillBottom
        + WizardCardSpacing
        + TextLayerFootprintHeight
        + WizardCardRowInteractiveShadowSpillBottom;

    /// <summary>Фиксированная высота области списков (paired, 2 ряда).</summary>
    public static double WizardCardListAreaHeight => TieredTwoRowViewportHeight;

    /// <summary>Ширина видимого ряда из 3 карточек (footprint + spacing).</summary>
    public static double ThreeCardRowWidth =>
        WizardBottomPresetColumns * TextLayerFootprintWidth
        + (WizardBottomPresetColumns - 1) * WizardCardSpacing;

    /// <summary>Ширина хоста ряда с запасом под тень по краям (ScrollViewer / cardArea).</summary>
    public static double ThreeCardRowHostWidth =>
        ThreeCardRowWidth + WizardCardRowShadowSpill * 2;

    /// <summary>Урезание viewport списка с каждой стороны (суммарно ~20 px), чтобы не просвечивала тень соседней карточки.</summary>
    public const double WizardCardListViewportTrim = 20;

    /// <summary>Ширина info-блока и области списков (центрированы в окне).</summary>
    public static double WizardCardListHostWidth =>
        ThreeCardRowHostWidth - WizardCardListViewportTrim;

    /// <summary>Горизонтальный spill внутри урезанного viewport (одинаковый у обоих ярусов).</summary>
    public static double WizardCardListRowHorizontalSpill =>
        (WizardCardListHostWidth - ThreeCardRowWidth) / 2;

    public static double ResolveRowHostWidth(int columns) =>
        columns * TextLayerFootprintWidth
        + Math.Max(0, columns - 1) * WizardCardSpacing
        + WizardCardListRowHorizontalSpill * 2;

    public static int ResolveWizardWindowWidth()
    {
        var pad = HostedDialogLayout.GetContentDialogPadding();
        return (int)Math.Ceiling(ThreeCardRowHostWidth + pad.Left + pad.Right);
    }
}
