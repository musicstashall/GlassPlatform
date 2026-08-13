using Windows.UI;

namespace GlassSettings.Services;

/// <summary>Ближайшее имя цвета акцента (как подпись в Персонализации Windows).</summary>
public static class AccentColorNameHelper
{
    private readonly record struct NamedSwatch(string ResourceKey, byte R, byte G, byte B);

    // Палитра имён — ориентир на системные подписи Personalization (ru/en через resw).
    private static readonly NamedSwatch[] Swatches =
    [
        new("Themes_ColorName_YellowGold", 255, 185, 0),
        new("Themes_ColorName_Gold", 255, 140, 0),
        new("Themes_ColorName_OrangeBright", 247, 99, 12),
        new("Themes_ColorName_OrangeDark", 202, 80, 16),
        new("Themes_ColorName_Rust", 218, 59, 1),
        new("Themes_ColorName_PaleRust", 239, 105, 80),
        new("Themes_ColorName_BrickRed", 209, 52, 56),
        new("Themes_ColorName_ModRed", 255, 67, 67),
        new("Themes_ColorName_PaleRed", 231, 72, 86),
        new("Themes_ColorName_Red", 232, 17, 35),
        new("Themes_ColorName_RoseBright", 234, 0, 94),
        new("Themes_ColorName_Rose", 195, 0, 82),
        new("Themes_ColorName_PlumLight", 227, 0, 140),
        new("Themes_ColorName_Plum", 191, 0, 119),
        new("Themes_ColorName_OrchidLight", 194, 57, 179),
        new("Themes_ColorName_Orchid", 154, 0, 137),
        new("Themes_ColorName_DefaultBlue", 0, 120, 215),
        new("Themes_ColorName_NavyBlue", 0, 99, 177),
        new("Themes_ColorName_PurpleShadow", 142, 140, 216),
        new("Themes_ColorName_Purple", 107, 105, 214),
        new("Themes_ColorName_IrisPastel", 136, 23, 152),
        new("Themes_ColorName_IrisSpring", 116, 77, 169),
        new("Themes_ColorName_VioletRedLight", 177, 70, 194),
        new("Themes_ColorName_VioletRed", 136, 39, 144),
        new("Themes_ColorName_CoolBlueBright", 0, 183, 195),
        new("Themes_ColorName_CoolBlue", 3, 131, 135),
        new("Themes_ColorName_Seafoam", 0, 178, 148),
        new("Themes_ColorName_SeafoamTeal", 1, 133, 116),
        new("Themes_ColorName_MintLight", 0, 204, 106),
        new("Themes_ColorName_MintDark", 0, 138, 73),
        new("Themes_ColorName_TurfGreen", 16, 137, 62),
        new("Themes_ColorName_SportGreen", 0, 133, 67),
        new("Themes_ColorName_Gray", 122, 117, 116),
        new("Themes_ColorName_GrayBrown", 93, 90, 88),
        new("Themes_ColorName_SteelBlue", 104, 118, 138),
        new("Themes_ColorName_MetalBlue", 81, 92, 107),
        new("Themes_ColorName_PaleMoss", 86, 124, 115),
        new("Themes_ColorName_Moss", 72, 104, 96),
        new("Themes_ColorName_MeadowGreen", 73, 130, 5),
        new("Themes_ColorName_Green", 16, 124, 16),
        new("Themes_ColorName_Overcast", 119, 121, 122),
        new("Themes_ColorName_Storm", 76, 74, 72),
        new("Themes_ColorName_BlueGray", 96, 94, 92),
        new("Themes_ColorName_GrayDark", 68, 67, 66),
        new("Themes_ColorName_LiddyGreen", 105, 121, 126),
        new("Themes_ColorName_Sage", 74, 84, 89),
        new("Themes_ColorName_CamouflageDesert", 100, 124, 100),
        new("Themes_ColorName_Camouflage", 73, 91, 73)
    ];

    public static string ResolveDisplayName(Color color)
    {
        NamedSwatch? best = null;
        var bestDist = double.MaxValue;
        foreach (var swatch in Swatches)
        {
            var dr = color.R - swatch.R;
            var dg = color.G - swatch.G;
            var db = color.B - swatch.B;
            var dist = dr * dr + dg * dg + db * db;
            if (dist >= bestDist)
                continue;

            bestDist = dist;
            best = swatch;
        }

        if (best is null)
            return Strings.GlassSettingsResources.Get("Themes_ColorName_Gray");

        return Strings.GlassSettingsResources.Get(best.Value.ResourceKey);
    }

    public static string ResolveCurrentAccentDisplayName() =>
        ResolveDisplayName(WindowsPersonalizationService.ReadUserAccentColor());
}
