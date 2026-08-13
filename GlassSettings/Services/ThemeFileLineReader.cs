using System.Text;

namespace GlassSettings.Services;

/// <summary>Чтение строк .theme (UTF-8/ANSI или UTF-16 LE с/без BOM).</summary>
internal static class ThemeFileLineReader
{
    public static IEnumerable<string> ReadLines(string themeFilePath)
    {
        if (string.IsNullOrWhiteSpace(themeFilePath) || !File.Exists(themeFilePath))
            return Array.Empty<string>();

        return File.ReadLines(themeFilePath, DetectEncoding(themeFilePath));
    }

    /// <summary>Публичный детект для повторного чтения DisplayName при mojibake.</summary>
    public static Encoding DetectEncoding(string themeFilePath)
    {
        using var stream = File.OpenRead(themeFilePath);
        Span<byte> bom = stackalloc byte[4];
        var read = stream.Read(bom);
        if (read >= 2 && bom[0] == 0xFF && bom[1] == 0xFE)
            return Encoding.Unicode;

        if (read >= 2 && bom[0] == 0xFE && bom[1] == 0xFF)
            return Encoding.BigEndianUnicode;

        if (read >= 3 && bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF)
            return new UTF8Encoding(encoderShouldEmitUTF8Identifier: true);

        var sampleSize = (int)Math.Min(1024, stream.Length);
        if (sampleSize <= 0)
            return Encoding.Unicode;

        stream.Position = 0;
        var sample = new byte[sampleSize];
        _ = stream.Read(sample, 0, sampleSize);

        // Windows .theme почти всегда UTF-16 LE: чётная длина и нулевые старшие байты ASCII-секций ([Theme]).
        if (sample.Length >= 4 && sample.Length % 2 == 0)
        {
            var nulHigh = 0;
            var asciiLow = 0;
            for (var i = 0; i + 1 < sample.Length; i += 2)
            {
                if (sample[i + 1] == 0)
                {
                    nulHigh++;
                    if (sample[i] is >= 0x20 and <= 0x7E or (byte)'\r' or (byte)'\n' or (byte)'\t')
                        asciiLow++;
                }
            }

            var pairs = sample.Length / 2;
            if (nulHigh > pairs / 4 && asciiLow > 8)
                return Encoding.Unicode;

            // Классический heuristic (латиница): нули на нечётных индексах.
            if (nulHigh > pairs / 8)
                return Encoding.Unicode;
        }

        // Ранний NUL в файле без BOM — типичный UTF-16 LE.
        for (var i = 0; i < Math.Min(64, sample.Length); i++)
        {
            if (sample[i] == 0)
                return Encoding.Unicode;
        }

        return new UTF8Encoding(encoderShouldEmitUTF8Identifier: false);
    }

    public static Encoding[] CandidateEncodings(string themeFilePath)
    {
        var primary = DetectEncoding(themeFilePath);
        if (Equals(primary, Encoding.Unicode))
            return [Encoding.Unicode, new UTF8Encoding(false), Encoding.Default];

        return [primary, Encoding.Unicode, new UTF8Encoding(false)];
    }
}
