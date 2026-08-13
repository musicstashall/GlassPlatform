using System.Runtime.InteropServices;
using GlassSettings.Core.Registry;
using GlassSettings.Services;

namespace GlassSettings.Core.Text;

/// <summary>P/Invoke структуры hero-превью текста (TextPreviewExport.h).</summary>
internal static class TextPreviewNative
{
    public const int PreviewFontPointSize = 10;

    private static readonly object s_nativePaintGate = new();

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeRect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    // LOGFONTW = 92 байта (как SystemFontsLogFont / winuser.h).
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode, Size = 92)]
    internal struct LogFontW
    {
        public int lfHeight;
        public int lfWidth;
        public int lfEscapement;
        public int lfOrientation;
        public int lfWeight;
        public byte lfItalic;
        public byte lfUnderline;
        public byte lfStrikeOut;
        public byte lfCharSet;
        public byte lfOutPrecision;
        public byte lfClipPrecision;
        public byte lfQuality;
        public byte lfPitchAndFamily;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string lfFaceName;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    internal struct PreviewInk
    {
        public byte RenderMode;
        public byte TargetBlendAlpha;
        public byte PseudoGlowEnabled;
        public byte Reserved0;
        public float CoverageGamma;
        public uint PseudoGlowRadiusThousandths;
        public uint PseudoGlowIntensity;
    }

    // Должен совпадать с GlassCoreTextPreviewRequest (#pragma pack(1) в TextPreviewExport.h).
    // Без Pack=1 на x86 paint ловил SEH 0x80000003 → пустой hero-текст.
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode, Pack = 1)]
    internal struct NativeRequest
    {
        public nint SampleText;
        public uint SampleCch;
        public LogFontW Font;
        public nint Glow;
        public nint Ink;
        public nint InkProfile;
        public NativeRect PaintRect;
        [MarshalAs(UnmanagedType.Bool)]
        public bool IsDarkTheme;
    }

    public static int PaintProfileToBuffer(
        byte[] buffer,
        int width,
        int height,
        string sampleText,
        bool isDarkTheme,
        bool interactiveDuringGesture,
        uint renderMode,
        GlassInkProfileModel profile)
    {
        if (buffer.Length < width * height * 4)
            return unchecked((int)0x80070057);

        lock (s_nativePaintGate)
        {
            profile.Clamp();
            NativeHostInterop.FillPreviewLogFont(out var font, PreviewFontPointSize);

            var textBytes = System.Text.Encoding.Unicode.GetBytes(sampleText + '\0');
            var textHandle = GCHandle.Alloc(textBytes, GCHandleType.Pinned);
            var inkHandle = GCHandle.Alloc(default(PreviewInk), GCHandleType.Pinned);
            var profileHandle = GCHandle.Alloc(profile, GCHandleType.Pinned);
            try
            {
                var ink = new PreviewInk
                {
                    RenderMode = (byte)Math.Min(renderMode, 2u),
                    TargetBlendAlpha = (byte)Math.Clamp(
                        profile.TargetBlendAlpha,
                        InkCapsuleInterop.UiBlendMin,
                        InkCapsuleInterop.UiBlendMax),
                    PseudoGlowEnabled = (byte)(profile.PseudoGlowEnabled != 0 ? 1 : 0),
                    CoverageGamma = Math.Clamp(
                        profile.CoverageGammaThousandths / 1000f,
                        InkCapsuleInterop.UiGammaMinThousandths / 1000f,
                        InkCapsuleInterop.UiGammaMaxThousandths / 1000f),
                    PseudoGlowRadiusThousandths = profile.PseudoGlowRadiusThousandths,
                    PseudoGlowIntensity = profile.PseudoGlowIntensity,
                };
                Marshal.StructureToPtr(ink, inkHandle.AddrOfPinnedObject(), false);

                var request = new NativeRequest
                {
                    SampleText = textHandle.AddrOfPinnedObject(),
                    SampleCch = (uint)sampleText.Length,
                    Font = font,
                    Glow = nint.Zero,
                    Ink = inkHandle.AddrOfPinnedObject(),
                    InkProfile = profileHandle.AddrOfPinnedObject(),
                    PaintRect = new NativeRect { Left = 0, Top = 0, Right = width, Bottom = height },
                    IsDarkTheme = isDarkTheme,
                };

                return NativeHostInterop.PaintTextPreviewToBuffer(
                    buffer,
                    width,
                    height,
                    width * 4,
                    ref request,
                    interactiveDuringGesture);
            }
            finally
            {
                profileHandle.Free();
                inkHandle.Free();
                textHandle.Free();
            }
        }
    }

    /// <summary>Hero ink-only preview (без glow — ExtTextOutWithAlpha не рисует halo).</summary>
    public static int PaintToBuffer(
        byte[] buffer,
        int width,
        int height,
        string sampleText,
        bool isDarkTheme,
        bool interactiveDuringGesture,
        GlassInkCapsuleModel? inkCapsule = null)
    {
        if (buffer.Length < width * height * 4)
            return unchecked((int)0x80070057); // E_INVALIDARG

        lock (s_nativePaintGate)
        {
            NativeHostInterop.FillPreviewLogFont(out var font, PreviewFontPointSize);

        var textBytes = System.Text.Encoding.Unicode.GetBytes(sampleText + '\0');
        var textHandle = GCHandle.Alloc(textBytes, GCHandleType.Pinned);
        GCHandle inkHandle = default;
        GCHandle profileHandle = default;
        try
        {
            nint inkPtr = nint.Zero;
            nint profilePtr = nint.Zero;

            if (inkCapsule is { } cap && !cap.IsAutoMode)
            {
                var profile = cap.GetActiveProfileCopy();
                profile.Clamp();

                var ink = new PreviewInk
                {
                    RenderMode = (byte)Math.Min(cap.RenderMode, 2u),
                    TargetBlendAlpha = (byte)Math.Clamp(
                        profile.TargetBlendAlpha,
                        InkCapsuleInterop.UiBlendMin,
                        InkCapsuleInterop.UiBlendMax),
                    PseudoGlowEnabled = (byte)(profile.PseudoGlowEnabled != 0 ? 1 : 0),
                    CoverageGamma = Math.Clamp(
                        profile.CoverageGammaThousandths / 1000f,
                        InkCapsuleInterop.UiGammaMinThousandths / 1000f,
                        InkCapsuleInterop.UiGammaMaxThousandths / 1000f),
                    PseudoGlowRadiusThousandths = profile.PseudoGlowRadiusThousandths,
                    PseudoGlowIntensity = profile.PseudoGlowIntensity,
                };

                inkHandle = GCHandle.Alloc(ink, GCHandleType.Pinned);
                profileHandle = GCHandle.Alloc(profile, GCHandleType.Pinned);
                inkPtr = inkHandle.AddrOfPinnedObject();
                profilePtr = profileHandle.AddrOfPinnedObject();
            }

            var request = new NativeRequest
            {
                SampleText = textHandle.AddrOfPinnedObject(),
                SampleCch = (uint)sampleText.Length,
                Font = font,
                Glow = nint.Zero,
                Ink = inkPtr,
                InkProfile = profilePtr,
                PaintRect = new NativeRect
                {
                    Left = 0,
                    Top = 0,
                    Right = width,
                    Bottom = height,
                },
                IsDarkTheme = isDarkTheme,
            };

            return NativeHostInterop.PaintTextPreviewToBuffer(
                buffer,
                width,
                height,
                width * 4,
                ref request,
                interactiveDuringGesture);
        }
        finally
        {
            if (profileHandle.IsAllocated)
                profileHandle.Free();
            if (inkHandle.IsAllocated)
                inkHandle.Free();
            textHandle.Free();
        }
        }
    }
}
