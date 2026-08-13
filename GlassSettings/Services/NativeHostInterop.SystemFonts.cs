using GlassSettings.Core.Text;
using System.Runtime.InteropServices;

namespace GlassSettings.Services;

public static partial class NativeHostInterop
{
    public const int SystemFontSlotCount = 6;
    public const int SystemFontBlockBytes = SystemFontsLogFont.BinarySize * SystemFontSlotCount;

    public static bool LoadSystemFontsSpiCache() => Native_LoadSystemFontsSpiCache();

    public static bool ApplySystemFontsSpi(
        bool[] workCustom,
        byte[] workLogFonts,
        byte[] refLogFonts,
        uint flags,
        out int menuLfHeight) =>
        Native_ApplySystemFontsSpi(
            workCustom,
            workLogFonts,
            (uint)workLogFonts.Length,
            refLogFonts,
            (uint)refLogFonts.Length,
            flags,
            out menuLfHeight);

    public static bool TryCopyCachedNonClientMetrics(byte[] buffer) =>
        buffer is { Length: > 0 } && Native_TryCopyCachedNonClientMetrics(buffer, (uint)buffer.Length);

    public static bool TryCopyCachedIconLogFont(byte[] buffer) =>
        buffer is { Length: >= SystemFontsLogFont.BinarySize }
        && Native_TryCopyCachedIconLogFont(buffer, (uint)buffer.Length);

    public static byte[] PackLogFonts(SystemFontsLogFont[] fonts)
    {
        var block = new byte[SystemFontBlockBytes];
        for (var i = 0; i < SystemFontSlotCount; i++)
            SystemFontsLogFont.ToBytes(fonts[i]).CopyTo(block, i * SystemFontsLogFont.BinarySize);
        return block;
    }

    [DllImport(Dll, EntryPoint = "GlassSettings_LoadSystemFontsSpiCache")]
    private static extern bool Native_LoadSystemFontsSpiCache();

    [DllImport(Dll, EntryPoint = "GlassSettings_ApplySystemFontsSpi")]
    private static extern bool Native_ApplySystemFontsSpi(
        [MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.Bool, SizeConst = SystemFontSlotCount)]
        bool[] workCustom,
        byte[] workLogFonts,
        uint workLogFontsBytes,
        byte[] refLogFonts,
        uint refLogFontsBytes,
        uint flags,
        out int menuLfHeight);

    [DllImport(Dll, EntryPoint = "GlassSettings_TryCopyCachedNonClientMetrics")]
    private static extern bool Native_TryCopyCachedNonClientMetrics(byte[] buffer, uint bufferBytes);

    [DllImport(Dll, EntryPoint = "GlassSettings_TryCopyCachedIconLogFont")]
    private static extern bool Native_TryCopyCachedIconLogFont(byte[] buffer, uint bufferBytes);
}
