namespace GlassSettings.Core.Flyout;

public static class ArgbColor
{
    public static uint Pack(byte a, byte r, byte g, byte b)
        => ((uint)a << 24) | ((uint)r << 16) | ((uint)g << 8) | b;

    public static void Unpack(uint argb, out byte a, out byte r, out byte g, out byte b)
    {
        a = (byte)(argb >> 24);
        r = (byte)(argb >> 16);
        g = (byte)(argb >> 8);
        b = (byte)argb;
    }

    public static bool TryParseHex(string? text, out uint argb)
    {
        argb = 0;
        if (string.IsNullOrWhiteSpace(text)) return false;
        var s = text.Trim().TrimStart('#');
        if (s.Length == 6 && uint.TryParse(s, System.Globalization.NumberStyles.HexNumber, null, out var rgb))
        {
            argb = 0xFF000000 | rgb;
            return true;
        }
        if (s.Length == 8 && uint.TryParse(s, System.Globalization.NumberStyles.HexNumber, null, out argb))
            return true;
        return false;
    }
}
