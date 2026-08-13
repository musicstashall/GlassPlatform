using System.Buffers.Binary;
using System.Runtime.InteropServices;
using System.Text;

namespace GlassSettings.Core.Text;

/// <summary>Win32 LOGFONTW (92 bytes).</summary>
[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode, Size = 92)]
public struct SystemFontsLogFont
{
    public const int BinarySize = 92;
    private const int FaceNameOffset = 28;
    private const int FaceNameByteLength = 64;

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

    public static SystemFontsLogFont Default => new() { lfFaceName = string.Empty };

    public static bool EqualsForCompare(in SystemFontsLogFont reference, in SystemFontsLogFont current)
    {
        var a = reference;
        a.lfQuality = current.lfQuality;
        return a.lfHeight == current.lfHeight
               && a.lfWidth == current.lfWidth
               && a.lfEscapement == current.lfEscapement
               && a.lfOrientation == current.lfOrientation
               && a.lfWeight == current.lfWeight
               && a.lfItalic == current.lfItalic
               && a.lfUnderline == current.lfUnderline
               && a.lfStrikeOut == current.lfStrikeOut
               && a.lfCharSet == current.lfCharSet
               && a.lfOutPrecision == current.lfOutPrecision
               && a.lfClipPrecision == current.lfClipPrecision
               && a.lfPitchAndFamily == current.lfPitchAndFamily
               && string.Equals(a.lfFaceName, current.lfFaceName, StringComparison.Ordinal);
    }

    /// <summary>Сериализация без GCHandle — struct содержит string (lfFaceName).</summary>
    public static byte[] ToBytes(in SystemFontsLogFont font)
    {
        var bytes = new byte[BinarySize];
        WriteToSpan(bytes, font);
        return bytes;
    }

    public static void WriteToSpan(Span<byte> bytes, in SystemFontsLogFont font)
    {
        if (bytes.Length < BinarySize)
            return;

        BinaryPrimitives.WriteInt32LittleEndian(bytes, font.lfHeight);
        BinaryPrimitives.WriteInt32LittleEndian(bytes.Slice(4), font.lfWidth);
        BinaryPrimitives.WriteInt32LittleEndian(bytes.Slice(8), font.lfEscapement);
        BinaryPrimitives.WriteInt32LittleEndian(bytes.Slice(12), font.lfOrientation);
        BinaryPrimitives.WriteInt32LittleEndian(bytes.Slice(16), font.lfWeight);
        bytes[20] = font.lfItalic;
        bytes[21] = font.lfUnderline;
        bytes[22] = font.lfStrikeOut;
        bytes[23] = font.lfCharSet;
        bytes[24] = font.lfOutPrecision;
        bytes[25] = font.lfClipPrecision;
        bytes[26] = font.lfQuality;
        bytes[27] = font.lfPitchAndFamily;

        var face = font.lfFaceName ?? string.Empty;
        var faceBytes = Encoding.Unicode.GetBytes(face);
        faceBytes.AsSpan(0, Math.Min(FaceNameByteLength, faceBytes.Length))
            .CopyTo(bytes.Slice(FaceNameOffset));
    }

    /// <summary>Десериализация без PtrToStructure — string ломает marshaling lfHeight.</summary>
    public static SystemFontsLogFont FromBytes(ReadOnlySpan<byte> data)
    {
        if (data.Length < BinarySize)
            return Default;

        var span = data.Length == BinarySize ? data : data[..BinarySize];
        return new SystemFontsLogFont
        {
            lfHeight = BinaryPrimitives.ReadInt32LittleEndian(span),
            lfWidth = BinaryPrimitives.ReadInt32LittleEndian(span.Slice(4)),
            lfEscapement = BinaryPrimitives.ReadInt32LittleEndian(span.Slice(8)),
            lfOrientation = BinaryPrimitives.ReadInt32LittleEndian(span.Slice(12)),
            lfWeight = BinaryPrimitives.ReadInt32LittleEndian(span.Slice(16)),
            lfItalic = span[20],
            lfUnderline = span[21],
            lfStrikeOut = span[22],
            lfCharSet = span[23],
            lfOutPrecision = span[24],
            lfClipPrecision = span[25],
            lfQuality = span[26],
            lfPitchAndFamily = span[27],
            lfFaceName = ReadFaceName(span.Slice(FaceNameOffset, FaceNameByteLength)),
        };
    }

    public static int ReadLfHeight(ReadOnlySpan<byte> data) =>
        data.Length >= 4 ? BinaryPrimitives.ReadInt32LittleEndian(data) : 0;

    private static string ReadFaceName(ReadOnlySpan<byte> faceBytes)
    {
        var end = faceBytes.Length;
        while (end >= 2 && faceBytes[end - 1] == 0 && faceBytes[end - 2] == 0)
            end -= 2;

        return end <= 0 ? string.Empty : Encoding.Unicode.GetString(faceBytes[..end]);
    }
}
