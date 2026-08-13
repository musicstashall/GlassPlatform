using System.Runtime.InteropServices;



namespace GlassSettings.Core.Text;



/// <summary>Зеркало Shared::InkCapsule::GlobalTextGlow (16 byte REG_BINARY на тему).</summary>

[StructLayout(LayoutKind.Sequential, Pack = 1)]

public struct GlobalTextGlowModel

{

    public uint GlowEnabled;

    public uint GlowRadiusThousandths;

    public uint GlowColor;

    public uint GlowSpreadMode;



    public const int BinarySize = 16;



    public const uint FactoryGlowAlpha = 149;



    public static GlobalTextGlowModel Factory => new()

    {

        GlowRadiusThousandths = 300,

        GlowColor = TextGlowColorHelper.MakeFactoryGlowColorArgb(),

    };



    public void Clamp()

    {

        GlowEnabled = GlowEnabled != 0 ? 1u : 0u;

        GlowRadiusThousandths = Math.Min(GlowRadiusThousandths, 1000u);

        GlowSpreadMode = Math.Min(GlowSpreadMode, 2u);

        var alpha = Math.Min((GlowColor >> 24) & 0xFFu, 255u);

        GlowColor = (GlowColor & 0x00FFFFFFu) | (alpha << 24);

    }



    public bool EqualsFactory()

    {

        var factory = Factory;

        return GlowEnabled == factory.GlowEnabled

               && GlowRadiusThousandths == factory.GlowRadiusThousandths

               && GlowSpreadMode == factory.GlowSpreadMode

               && GlowColor == factory.GlowColor;

    }



    public static GlobalTextGlowModel FromBytes(ReadOnlySpan<byte> data)

    {

        if (data.Length < BinarySize)

            return Factory;



        var model = MemoryMarshal.Read<GlobalTextGlowModel>(data);

        model.Clamp();

        return model;

    }



    public byte[] ToBytes()

    {

        var bytes = new byte[BinarySize];

        MemoryMarshal.Write(bytes, in this);

        return bytes;

    }

}


