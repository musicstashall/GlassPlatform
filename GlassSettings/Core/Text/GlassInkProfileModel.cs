using System.Runtime.InteropServices;
using GlassSettings.Core.Registry;

namespace GlassSettings.Core.Text;

/// <summary>Зеркало Shared::InkCapsule::ProfilePayload (44 byte).</summary>
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct GlassInkProfileModel
{
    public uint CoverageGammaThousandths;
    public uint PreserveClearTypeTint;
    public uint CovInAlphaScaleThousandths;
    public uint FringeGainThousandths;
    public uint AlphaPostAdd;
    public uint TargetBlendAlpha;
    public uint GrayBgLevel;
    public uint FringeSofteningThousandths;
    public uint PseudoGlowEnabled;
    public uint PseudoGlowRadiusThousandths;
    public uint PseudoGlowIntensity;

    public const int BinarySize = 44;

    public static GlassInkProfileModel FactorySingle => GlassInkCapsuleFactoryCache.Single;

    public static GlassInkProfileModel FactoryDual => GlassInkCapsuleFactoryCache.Dual;

    public static GlassInkProfileModel AutoSingle => FactorySingle;

    public static GlassInkProfileModel AutoDual => FactoryDual;

    public void Clamp()
    {
        CoverageGammaThousandths = (uint)Math.Clamp(
            (int)CoverageGammaThousandths,
            (int)InkCapsuleInterop.UiGammaMinThousandths,
            (int)InkCapsuleInterop.UiGammaMaxThousandths);
        TargetBlendAlpha = (uint)Math.Clamp(
            (int)TargetBlendAlpha,
            (int)InkCapsuleInterop.UiBlendMin,
            (int)InkCapsuleInterop.UiBlendMax);
        if (PseudoGlowEnabled != 0)
        {
            PseudoGlowRadiusThousandths = (uint)Math.Clamp(
                (int)PseudoGlowRadiusThousandths,
                (int)InkCapsuleInterop.UiPseudoRadiusMinThousandths,
                (int)InkCapsuleInterop.UiPseudoRadiusMaxThousandths);
            PseudoGlowIntensity = (uint)Math.Clamp(
                (int)PseudoGlowIntensity,
                (int)InkCapsuleInterop.UiPseudoIntensityMin,
                (int)InkCapsuleInterop.UiPseudoIntensityMax);
        }
        else
        {
            PseudoGlowRadiusThousandths = Math.Min(
                PseudoGlowRadiusThousandths,
                InkCapsuleInterop.UiPseudoRadiusMaxThousandths);
            PseudoGlowIntensity = (uint)Math.Clamp(
                (int)PseudoGlowIntensity,
                0,
                (int)InkCapsuleInterop.UiPseudoIntensityMax);
        }
        SyncPseudoEnabledFlag();
        WriteNeutralDeprecatedSlots();
    }

    public void SyncPseudoEnabledFlag()
    {
        if (PseudoGlowRadiusThousandths > 0 || PseudoGlowIntensity > 0)
            PseudoGlowEnabled = 1u;
        // Не сбрасывать PseudoGlowEnabled в 0 при нулевых R/I — gate задаёт флаг явно.
    }

    public void WriteNeutralDeprecatedSlots()
    {
        PreserveClearTypeTint = 0;
        CovInAlphaScaleThousandths = 1000;
        FringeGainThousandths = 1000;
        AlphaPostAdd = 0;
        GrayBgLevel = 0;
        FringeSofteningThousandths = 1000;
    }

    public bool EqualsFactorySingle()
    {
        var f = FactorySingle;
        return CoverageGammaThousandths == f.CoverageGammaThousandths
               && TargetBlendAlpha == f.TargetBlendAlpha
               && PseudoGlowEnabled == f.PseudoGlowEnabled
               && PseudoGlowRadiusThousandths == f.PseudoGlowRadiusThousandths
               && PseudoGlowIntensity == f.PseudoGlowIntensity;
    }

    public bool EqualsFactoryDual()
    {
        var f = FactoryDual;
        return CoverageGammaThousandths == f.CoverageGammaThousandths
               && TargetBlendAlpha == f.TargetBlendAlpha
               && PseudoGlowEnabled == f.PseudoGlowEnabled
               && PseudoGlowRadiusThousandths == f.PseudoGlowRadiusThousandths
               && PseudoGlowIntensity == f.PseudoGlowIntensity;
    }

    public static GlassInkProfileModel FromBytes(ReadOnlySpan<byte> data, int offset = 0)
    {
        if (data.Length - offset < BinarySize)
            return FactorySingle;

        var model = MemoryMarshal.Read<GlassInkProfileModel>(data.Slice(offset));
        model.Clamp();
        return model;
    }
}
