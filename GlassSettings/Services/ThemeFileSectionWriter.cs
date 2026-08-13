using System.Text;

namespace GlassSettings.Services;

/// <summary>Запись/обновление INI-секции в .theme с сохранением исходной кодировки.</summary>
internal static class ThemeFileSectionWriter
{
    public static bool TryUpsertSection(
        string themeFilePath,
        string sectionName,
        IReadOnlyDictionary<string, string> values)
    {
        if (string.IsNullOrWhiteSpace(themeFilePath) || string.IsNullOrWhiteSpace(sectionName))
            return false;

        try
        {
            var encoding = DetectEncoding(themeFilePath);
            var lines = File.Exists(themeFilePath)
                ? File.ReadAllLines(themeFilePath, encoding).ToList()
                : new List<string>();

            var header = "[" + sectionName + "]";
            var start = -1;
            var end = lines.Count;
            for (var i = 0; i < lines.Count; i++)
            {
                var trimmed = lines[i].Trim();
                if (!trimmed.StartsWith('[') || !trimmed.EndsWith(']'))
                    continue;

                var name = trimmed[1..^1].Trim();
                if (start < 0)
                {
                    if (string.Equals(name, sectionName, StringComparison.OrdinalIgnoreCase))
                        start = i;
                    continue;
                }

                end = i;
                break;
            }

            var block = new List<string> { header };
            foreach (var pair in values.OrderBy(p => p.Key, StringComparer.OrdinalIgnoreCase))
                block.Add(pair.Key + "=" + pair.Value);

            if (start < 0)
            {
                if (lines.Count > 0 && !string.IsNullOrWhiteSpace(lines[^1]))
                    lines.Add(string.Empty);
                lines.AddRange(block);
            }
            else
            {
                lines.RemoveRange(start, end - start);
                lines.InsertRange(start, block);
            }

            var directory = Path.GetDirectoryName(themeFilePath);
            if (!string.IsNullOrWhiteSpace(directory))
                Directory.CreateDirectory(directory);

            File.WriteAllLines(themeFilePath, lines, encoding);
            return true;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings ThemeFileSectionWriter.TryUpsertSection: {ex}");
            return false;
        }
    }

    public static bool SectionExists(string themeFilePath, string sectionName)
    {
        if (string.IsNullOrWhiteSpace(themeFilePath) || !File.Exists(themeFilePath))
            return false;

        try
        {
            foreach (var raw in ThemeFileLineReader.ReadLines(themeFilePath))
            {
                var line = raw.Trim();
                if (!line.StartsWith('[') || !line.EndsWith(']'))
                    continue;

                if (string.Equals(line[1..^1].Trim(), sectionName, StringComparison.OrdinalIgnoreCase))
                    return true;
            }
        }
        catch
        {
            return false;
        }

        return false;
    }

    private static Encoding DetectEncoding(string themeFilePath)
    {
        if (!File.Exists(themeFilePath))
            return Encoding.Unicode;

        using var stream = File.OpenRead(themeFilePath);
        Span<byte> bom = stackalloc byte[4];
        var read = stream.Read(bom);
        if (read >= 2 && bom[0] == 0xFF && bom[1] == 0xFE)
            return Encoding.Unicode;

        if (read >= 2 && bom[0] == 0xFE && bom[1] == 0xFF)
            return Encoding.BigEndianUnicode;

        if (read >= 3 && bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF)
            return new UTF8Encoding(encoderShouldEmitUTF8Identifier: true);

        var sampleSize = (int)Math.Min(512, stream.Length);
        if (sampleSize <= 0)
            return Encoding.Unicode;

        stream.Position = 0;
        var sample = new byte[sampleSize];
        _ = stream.Read(sample, 0, sampleSize);

        var utf16NullPairs = 0;
        for (var i = 1; i < sample.Length; i += 2)
        {
            if (sample[i] == 0 && sample[i - 1] != 0)
                utf16NullPairs++;
        }

        if (utf16NullPairs > sample.Length / 8)
            return Encoding.Unicode;

        return new UTF8Encoding(encoderShouldEmitUTF8Identifier: false);
    }
}
