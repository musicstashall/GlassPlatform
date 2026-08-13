using System.Runtime.InteropServices;

namespace GlassSettings.Core.Text;

/// <summary>P/Invoke для системных шрифтов, SPI и GDI enum.</summary>
internal static class SystemFontsNative
{
    public const uint SpiGetNonClientMetrics = 0x0029;
    public const uint SpiSetNonClientMetrics = 0x002A;
    public const uint SpiGetIconTitleLogFont = 0x001F;
    public const uint SpiSetIconTitleLogFont = 0x0022;
    public const uint SpifUpdateIniFile = 0x0001;
    public const uint SpifSendChange = 0x0002;

    public const int FontenumProcContinue = 1;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    internal struct NonClientMetricsW
    {
        public uint cbSize;
        public int iBorderWidth;
        public int iScrollWidth;
        public int iScrollHeight;
        public int iCaptionWidth;
        public int iCaptionHeight;
        public SystemFontsLogFont lfCaptionFont;
        public SystemFontsLogFont lfSmCaptionFont;
        public SystemFontsLogFont lfMenuFont;
        public SystemFontsLogFont lfStatusFont;
        public SystemFontsLogFont lfMessageFont;
        /// <summary>Windows 10 1809+ — без поля SPI_GET/SET падает.</summary>
        public int iPaddedBorderWidth;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    internal struct EnumLogFontExW
    {
        public SystemFontsLogFont elfLogFont;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]
        public string elfFullName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string elfStyle;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    internal struct TextMetricW
    {
        public int tmHeight;
        public int tmAscent;
        public int tmDescent;
        public int tmInternalLeading;
        public int tmExternalLeading;
        public int tmAveCharWidth;
        public int tmMaxCharWidth;
        public int tmWeight;
        public int tmOverhang;
        public int tmDigitizedAspectX;
        public int tmDigitizedAspectY;
        public char tmFirstChar;
        public char tmLastChar;
        public char tmDefaultChar;
        public char tmBreakChar;
        public byte tmItalic;
        public byte tmUnderlined;
        public byte tmStruckOut;
        public byte tmPitchAndFamily;
        public byte tmCharSet;
    }

    internal delegate int FontEnumProc(ref EnumLogFontExW lpelfe, ref TextMetricW lpntme, uint fontType, nint lParam);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    internal static extern bool SystemParametersInfoW(
        uint uiAction,
        uint uiParam,
        nint pvParam,
        uint fWinIni);

    [DllImport("gdi32.dll", CharSet = CharSet.Unicode)]
    internal static extern int EnumFontFamiliesExW(
        nint hdc,
        ref SystemFontsLogFont lpLogfont,
        FontEnumProc lpEnumFontFamExProc,
        nint lParam,
        uint dwFlags);

    [DllImport("gdi32.dll", CharSet = CharSet.Unicode)]
    internal static extern nint CreateDCW(string lpszDriver, string? lpszDevice, string? lpszOutput, nint lpInitData);

    [DllImport("gdi32.dll")]
    internal static extern bool DeleteDC(nint hdc);

    [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)]
    internal static extern nint OpenThemeData(nint hwnd, string pszClassList);

    [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)]
    internal static extern int GetThemeSysFont(nint hTheme, int iFontId, out SystemFontsLogFont plf);

    [DllImport("uxtheme.dll")]
    internal static extern int CloseThemeData(nint hTheme);

    [DllImport("user32.dll")]
    internal static extern nint GetDesktopWindow();

    [DllImport("kernel32.dll")]
    internal static extern uint GetLastError();

    internal static uint NonClientMetricsCbSize =>
        (uint)Marshal.SizeOf<NonClientMetricsW>();

    /// <summary>Буфер NONCLIENTMETRICSW с корректным cbSize (AllocHGlobal не обнуляет память).</summary>
    internal static nint AllocNonClientMetricsBuffer()
    {
        var size = (int)NonClientMetricsCbSize;
        var ptr = Marshal.AllocHGlobal(size);
        Marshal.WriteInt32(ptr, (int)NonClientMetricsCbSize);
        return ptr;
    }
}
