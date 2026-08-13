using System.Runtime.InteropServices;
using GlassSettings.Core.Flyout;

namespace GlassSettings.Core.Text;

/// <summary>C#-паритет Shared::InkCapsule glow ARGB + маппинг пикера.</summary>
public static class TextGlowColorHelper
{
    private const int ColorWindowIndex = 5;
    private const int ColorWindowTextIndex = 8;

    public static uint MakeFactoryGlowColorArgb()
    {
        FromStoredColorRef(ResolveColorWindowRgb(), out var r, out var g, out var b);
        return ArgbColor.Pack((byte)GlobalTextGlowModel.FactoryGlowAlpha, r, g, b);
    }

    public static uint ResolveColorWindowRgb() =>
        GetSysColor(ColorWindowIndex) & 0x00FFFFFFu;

    public static uint ResolveColorWindowTextRgb() =>
        GetSysColor(ColorWindowTextIndex) & 0x00FFFFFFu;

    public static uint ToPickerArgb(GlobalTextGlowModel model) =>
        model.GlowColor;

    public static void ApplyPickerArgb(ref GlobalTextGlowModel model, bool darkMode, uint argb)
    {
        _ = darkMode;
        if (MatchesColorWindowText(argb))
        {
            model.GlowColor = argb;
            model.GlowEnabled = 0;
            model.Clamp();
            return;
        }

        model.GlowColor = argb;
        model.Clamp();
    }

    public static void ApplyPickerRgb(ref GlobalTextGlowModel model, bool darkMode, byte r, byte g, byte b)
    {
        ArgbColor.Unpack(model.GlowColor, out var alpha, out _, out _, out _);
        ApplyPickerArgb(ref model, darkMode, ArgbColor.Pack(alpha, r, g, b));
    }

    public static bool IsColorDefault(GlobalTextGlowModel model) =>
        model.GlowColor == MakeFactoryGlowColorArgb();

    /// <summary>Radius, spread и color/alpha совпадают с factory (GlowEnabled не сравнивается).</summary>
    public static bool AreResettableParametersAtFactory(GlobalTextGlowModel model)
    {
        var factory = GlobalTextGlowModel.Factory;
        return model.GlowRadiusThousandths == factory.GlowRadiusThousandths
            && model.GlowSpreadMode == factory.GlowSpreadMode
            && model.GlowColor == factory.GlowColor;
    }

    public static bool MatchesColorWindowText(uint argb)
    {
        ArgbColor.Unpack(argb, out _, out var r, out var g, out var b);
        var rgb = (uint)(r | (g << 8) | (b << 16));
        return rgb == (ResolveColorWindowTextRgb() & 0x00FFFFFFu);
    }

    public static void FromStoredColorRef(uint colorRef, out byte r, out byte g, out byte b)
    {
        r = (byte)(colorRef & 0xFF);
        g = (byte)((colorRef >> 8) & 0xFF);
        b = (byte)((colorRef >> 16) & 0xFF);
    }

    [DllImport("user32.dll")]
    private static extern uint GetSysColor(int nIndex);
}
