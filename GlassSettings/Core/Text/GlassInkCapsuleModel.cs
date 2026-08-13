using System.Runtime.InteropServices;

namespace GlassSettings.Core.Text;

/// <summary>Зеркало Shared::InkCapsule::GlassInkAlphaCapsule (100 byte REG_BINARY).</summary>
public sealed class GlassInkCapsuleModel
{
    public const uint Magic = 0x31545647; // 'GVT1' little-endian

    public const int BinarySize = 100;

    public uint RenderMode { get; set; }

    public GlassInkProfileModel Single { get; set; }

    public GlassInkProfileModel Dual { get; set; }

    public static GlassInkCapsuleModel Factory => GlassInkCapsuleFactoryCache.Factory;

    public bool IsAutoMode => RenderMode == 0;

    public GlassInkProfileModel GetActiveProfileCopy() =>
        RenderMode == 2 ? Dual : Single;

    public void SetActiveProfile(GlassInkProfileModel profile)
    {
        if (RenderMode == 2)
            Dual = profile;
        else
            Single = profile;
    }

    public void EnterAutoMode()
    {
        RenderMode = 0;
        Single = GlassInkProfileModel.AutoSingle;
        Dual = GlassInkProfileModel.AutoDual;
    }

    public void Clamp()
    {
        RenderMode = Math.Min(RenderMode, 2u);
        Single.Clamp();
        Dual.Clamp();
    }

    public bool EqualsFactory()
    {
        var factory = Factory;
        return RenderMode == factory.RenderMode
               && ProfilesEqual(Single, factory.Single)
               && ProfilesEqual(Dual, factory.Dual);
    }

    public GlassInkCapsuleModel Clone() => new()
    {
        RenderMode = RenderMode,
        Single = Single,
        Dual = Dual,
    };

    public byte[] ToBytes()
    {
        var bytes = new byte[BinarySize];
        WriteTo(bytes);
        return bytes;
    }

    public void WriteTo(Span<byte> destination)
    {
        if (destination.Length < BinarySize)
            throw new ArgumentException("Buffer too small for ink capsule.", nameof(destination));

        Clamp();
        var offset = 0;
        WriteUInt32(destination, ref offset, Magic);
        WriteUInt32(destination, ref offset, RenderMode);
        WriteUInt32(destination, ref offset, 0);
        WriteProfile(destination, ref offset, Single);
        WriteProfile(destination, ref offset, Dual);
    }

    public static GlassInkCapsuleModel FromBytes(ReadOnlySpan<byte> data)
    {
        if (data.Length < BinarySize)
            return Factory;

        var model = new GlassInkCapsuleModel();
        var offset = 0;
        _ = ReadUInt32(data, ref offset);
        model.RenderMode = ReadUInt32(data, ref offset);
        _ = ReadUInt32(data, ref offset);
        model.Single = GlassInkProfileModel.FromBytes(data, offset);
        offset += GlassInkProfileModel.BinarySize;
        model.Dual = GlassInkProfileModel.FromBytes(data, offset);
        model.Clamp();
        return model;
    }

    private static bool ProfilesEqual(GlassInkProfileModel a, GlassInkProfileModel b) =>
        a.CoverageGammaThousandths == b.CoverageGammaThousandths
        && a.TargetBlendAlpha == b.TargetBlendAlpha
        && a.PseudoGlowEnabled == b.PseudoGlowEnabled
        && a.PseudoGlowRadiusThousandths == b.PseudoGlowRadiusThousandths
        && a.PseudoGlowIntensity == b.PseudoGlowIntensity;

    private static void WriteUInt32(Span<byte> buffer, ref int offset, uint value)
    {
        BitConverter.TryWriteBytes(buffer.Slice(offset), value);
        offset += 4;
    }

    private static uint ReadUInt32(ReadOnlySpan<byte> buffer, ref int offset)
    {
        var value = BitConverter.ToUInt32(buffer.Slice(offset));
        offset += 4;
        return value;
    }

    private static void WriteProfile(Span<byte> buffer, ref int offset, GlassInkProfileModel profile)
    {
        profile.Clamp();
        MemoryMarshal.Write(buffer.Slice(offset), in profile);
        offset += GlassInkProfileModel.BinarySize;
    }
}
