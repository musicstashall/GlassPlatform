using System.Runtime.InteropServices;

namespace GlassSettings.Core.Registry;

/// <summary>Ink capsule: РµРґРёРЅС‹Р№ РёСЃС‚РѕС‡РЅРёРє вЂ” Shared::InkCapsuleQuery (GlassShared).</summary>
internal static class InkCapsuleInterop
{
    private const string Dll = "GlassSettings.Native.dll";

    public const int CapsuleBinarySize = 100;

    public static uint UiGammaMinThousandths => Native_GetInkUiGammaMinThousandths();

    public static uint UiGammaMaxThousandths => Native_GetInkUiGammaMaxThousandths();

    public static uint UiBlendMin => Native_GetInkUiBlendMin();

    public static uint UiBlendMax => Native_GetInkUiBlendMax();

    public static uint UiPseudoRadiusMaxThousandths => Native_GetInkUiPseudoRadiusMaxThousandths();

    public static uint UiPseudoRadiusMinThousandths => Native_GetInkUiPseudoRadiusMinThousandths();

    public static uint UiPseudoIntensityMax => Native_GetInkUiPseudoIntensityMax();

    public static uint UiPseudoIntensityMin => Native_GetInkUiPseudoIntensityMin();

    public static bool IsPresent() => Native_IsInkCapsulePresent();

    public static bool ReadUiInkCapsule(Span<byte> destination)
    {
        if (destination.Length < CapsuleBinarySize)
            throw new ArgumentException("Buffer too small for ink capsule.", nameof(destination));

        var buffer = new byte[CapsuleBinarySize];
        var fromRegistry = Native_ReadUiInkCapsule(buffer, (nuint)CapsuleBinarySize);
        buffer.AsSpan().CopyTo(destination);
        return fromRegistry;
    }

    public static void ReadFactoryInkCapsule(Span<byte> destination)
    {
        if (destination.Length < CapsuleBinarySize)
            throw new ArgumentException("Buffer too small for ink capsule.", nameof(destination));

        var buffer = new byte[CapsuleBinarySize];
        Native_ReadFactoryInkCapsule(buffer, (nuint)CapsuleBinarySize);
        buffer.AsSpan().CopyTo(destination);
    }

    [DllImport(Dll, EntryPoint = "GlassSettings_ReadUiInkCapsule")]
    private static extern bool Native_ReadUiInkCapsule(byte[] buffer, nuint byteCount);

    [DllImport(Dll, EntryPoint = "GlassSettings_ReadFactoryInkCapsule")]
    private static extern void Native_ReadFactoryInkCapsule(byte[] buffer, nuint byteCount);

    [DllImport(Dll, EntryPoint = "GlassSettings_IsInkCapsulePresent")]
    private static extern bool Native_IsInkCapsulePresent();

    [DllImport(Dll, EntryPoint = "GlassSettings_GetInkUiGammaMinThousandths")]
    private static extern uint Native_GetInkUiGammaMinThousandths();

    [DllImport(Dll, EntryPoint = "GlassSettings_GetInkUiGammaMaxThousandths")]
    private static extern uint Native_GetInkUiGammaMaxThousandths();

    [DllImport(Dll, EntryPoint = "GlassSettings_GetInkUiBlendMin")]
    private static extern uint Native_GetInkUiBlendMin();

    [DllImport(Dll, EntryPoint = "GlassSettings_GetInkUiBlendMax")]
    private static extern uint Native_GetInkUiBlendMax();

    [DllImport(Dll, EntryPoint = "GlassSettings_GetInkUiPseudoRadiusMaxThousandths")]
    private static extern uint Native_GetInkUiPseudoRadiusMaxThousandths();

    [DllImport(Dll, EntryPoint = "GlassSettings_GetInkUiPseudoRadiusMinThousandths")]
    private static extern uint Native_GetInkUiPseudoRadiusMinThousandths();

    [DllImport(Dll, EntryPoint = "GlassSettings_GetInkUiPseudoIntensityMax")]
    private static extern uint Native_GetInkUiPseudoIntensityMax();

    [DllImport(Dll, EntryPoint = "GlassSettings_GetInkUiPseudoIntensityMin")]
    private static extern uint Native_GetInkUiPseudoIntensityMin();
}
