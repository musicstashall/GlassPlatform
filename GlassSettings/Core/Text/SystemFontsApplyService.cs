using GlassSettings.Services;

namespace GlassSettings.Core.Text;

/// <summary>SPI-применение системных шрифтов — нативный порт TextSystemFontsPageApplyToSystemIfNeeded.</summary>
public static class SystemFontsApplyService
{
    public static void Apply(SystemFontsModel model)
    {
        var workCustom = (bool[])model.WorkCustom.Clone();
        var workBlock = NativeHostInterop.PackLogFonts(model.WorkLf);
        var refBlock = NativeHostInterop.PackLogFonts(model.RefLf);
        var flags = SystemFontsNative.SpifUpdateIniFile | SystemFontsNative.SpifSendChange;

        _ = NativeHostInterop.ApplySystemFontsSpi(
            workCustom,
            workBlock,
            refBlock,
            flags,
            out _);
    }
}
