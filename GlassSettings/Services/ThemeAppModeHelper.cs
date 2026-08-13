namespace GlassSettings.Services;

/// <summary>AppMode / SystemMode из секции [VisualStyles] файла .theme.</summary>
public static class ThemeAppModeHelper
{
    public static bool TryReadIsDarkMode(string themeFilePath, out bool isDark)
    {
        isDark = false;
        if (string.IsNullOrWhiteSpace(themeFilePath) || !File.Exists(themeFilePath))
            return false;

        try
        {
            var inVisualStyles = false;
            string? appMode = null;
            string? systemMode = null;

            foreach (var rawLine in ThemeFileLineReader.ReadLines(themeFilePath))
            {
                var line = rawLine.Trim();
                if (line.Length == 0 || line[0] == ';')
                    continue;

                if (line.StartsWith('[') && line.EndsWith(']'))
                {
                    var section = line[1..^1].Trim();
                    inVisualStyles = string.Equals(section, "VisualStyles", StringComparison.OrdinalIgnoreCase);
                    continue;
                }

                if (!inVisualStyles)
                    continue;

                var separator = line.IndexOf('=');
                if (separator <= 0)
                    continue;

                var key = line[..separator].Trim();
                var value = line[(separator + 1)..].Trim();
                if (string.Equals(key, "AppMode", StringComparison.OrdinalIgnoreCase))
                    appMode = value;
                else if (string.Equals(key, "SystemMode", StringComparison.OrdinalIgnoreCase))
                    systemMode = value;
            }

            if (TryParseMode(appMode, out isDark))
                return true;

            if (TryParseMode(systemMode, out isDark))
                return true;
        }
        catch
        {
            return false;
        }

        return false;
    }

    public static bool ReadIsDarkMode(string themeFilePath) =>
        TryReadIsDarkMode(themeFilePath, out var isDark) ? isDark : false;

    private static bool TryParseMode(string? raw, out bool isDark)
    {
        isDark = false;
        if (string.IsNullOrWhiteSpace(raw))
            return false;

        if (raw.Contains("Dark", StringComparison.OrdinalIgnoreCase))
        {
            isDark = true;
            return true;
        }

        if (raw.Contains("Light", StringComparison.OrdinalIgnoreCase))
        {
            isDark = false;
            return true;
        }

        return false;
    }
}
