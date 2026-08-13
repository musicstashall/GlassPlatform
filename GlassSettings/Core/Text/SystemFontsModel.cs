namespace GlassSettings.Core.Text;

/// <summary>Состояние 6 системных шрифтов на странице «Текст и шрифты».</summary>
public sealed class SystemFontsModel
{
    public SystemFontsLogFont[] RefLf { get; } = new SystemFontsLogFont[SystemFontsCatalog.SlotCount];
    public SystemFontsLogFont[] WorkLf { get; } = new SystemFontsLogFont[SystemFontsCatalog.SlotCount];
    public bool[] WorkCustom { get; } = new bool[SystemFontsCatalog.SlotCount];

    public int CurrentIndex { get; set; }

    public void LoadFromRegistryAndTheme()
    {
        var system = WindowMetricsFontRegistry.ReadAllFromRegistryOrSpi();
        var refs = SystemFontsThemeRef.LoadFromTheme(system);

        for (var i = 0; i < SystemFontsCatalog.SlotCount; i++)
        {
            RefLf[i] = refs[i];
            WorkLf[i] = system[i];
            WorkCustom[i] = !SystemFontsLogFont.EqualsForCompare(RefLf[i], WorkLf[i]);
        }
    }

    public void ReloadThemeRefs()
    {
        var system = WindowMetricsFontRegistry.ReadAllFromRegistryOrSpi();
        var refs = SystemFontsThemeRef.LoadFromTheme(system);
        for (var i = 0; i < SystemFontsCatalog.SlotCount; i++)
            RefLf[i] = refs[i];
    }

    public void ReloadWorkFromRegistry()
    {
        var system = WindowMetricsFontRegistry.ReadAllFromRegistryOrSpi();
        for (var i = 0; i < SystemFontsCatalog.SlotCount; i++)
        {
            WorkLf[i] = system[i];
            WorkCustom[i] = !SystemFontsLogFont.EqualsForCompare(RefLf[i], WorkLf[i]);
        }
    }

    public bool EqualsRef(int index, in SystemFontsLogFont font) =>
        index is >= 0 and < SystemFontsCatalog.SlotCount
        && SystemFontsLogFont.EqualsForCompare(RefLf[index], font);

    public void SetWorkFont(int index, in SystemFontsLogFont font)
    {
        if (index is < 0 or >= SystemFontsCatalog.SlotCount)
            return;

        WorkLf[index] = font;
    }

    public void ResetSlotToTheme(int index)
    {
        if (index is < 0 or >= SystemFontsCatalog.SlotCount)
            return;

        WorkLf[index] = RefLf[index];
        WorkCustom[index] = false;
    }

    public bool AllWorkFontsEqual()
    {
        for (var i = 1; i < SystemFontsCatalog.SlotCount; i++)
        {
            if (!SystemFontsLogFont.EqualsForCompare(WorkLf[0], WorkLf[i]))
                return false;
        }

        return true;
    }

    public bool AnyWorkCustom()
    {
        for (var i = 0; i < SystemFontsCatalog.SlotCount; i++)
        {
            if (WorkCustom[i])
                return true;
        }

        return false;
    }

    public void ApplyFontToAllSlots(in SystemFontsLogFont font)
    {
        for (var i = 0; i < SystemFontsCatalog.SlotCount; i++)
        {
            SetWorkFont(i, font);
            WorkCustom[i] = !EqualsRef(i, font);
        }
    }

    public void ResetAllSlotsToTheme()
    {
        for (var i = 0; i < SystemFontsCatalog.SlotCount; i++)
            ResetSlotToTheme(i);
    }

    public SystemFontsLogFont[] BuildApplySet()
    {
        var toApply = new SystemFontsLogFont[SystemFontsCatalog.SlotCount];
        for (var i = 0; i < SystemFontsCatalog.SlotCount; i++)
            toApply[i] = WorkCustom[i] ? WorkLf[i] : RefLf[i];
        return toApply;
    }

    public static string FormatPointSize(in SystemFontsLogFont font)
    {
        var dpiY = GetDeviceDpiY();
        if (font.lfHeight < 0)
            return MulDiv(-font.lfHeight, 72, dpiY).ToString();
        if (font.lfHeight > 0)
            return MulDiv(font.lfHeight, 72, dpiY).ToString();
        return "0";
    }

    public static int PointSizeToLfHeight(int pointSize)
    {
        if (pointSize <= 0)
            return 0;
        var dpiY = GetDeviceDpiY();
        return -MulDiv(pointSize, dpiY, 72);
    }

    public static int ReadPointSize(in SystemFontsLogFont font)
    {
        if (!int.TryParse(FormatPointSize(font), out var pt))
            return 0;
        return pt;
    }

    private static int GetDeviceDpiY()
    {
        var hdc = SystemFontsNative.CreateDCW("DISPLAY", null, null, 0);
        if (hdc == 0)
            return 96;

        try
        {
            return GetDeviceCaps(hdc, 90); // LOGPIXELSY
        }
        finally
        {
            SystemFontsNative.DeleteDC(hdc);
        }
    }

    private static int MulDiv(int value, int numerator, int denominator)
    {
        if (denominator == 0)
            return 0;
        return (int)((long)value * numerator / denominator);
    }

    [System.Runtime.InteropServices.DllImport("gdi32.dll")]
    private static extern int GetDeviceCaps(nint hdc, int index);
}
