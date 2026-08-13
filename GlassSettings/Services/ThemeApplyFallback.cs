using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace GlassSettings.Services;

/// <summary>
/// Fallback, если IThemeManager2 недоступен: #65 + обои (SPI) + узкая запись звуков/иконок из .theme.
/// </summary>
internal static class ThemeApplyFallback
{
    private const uint SpiSetDeskWallpaper = 0x0014;
    private const uint SpifUpdateIniFile = 0x01;
    private const uint SpifSendChange = 0x02;
    private const uint ShcneAssocChanged = 0x08000000;
    private const uint ShcnfFlush = 0x1000;

    private const string DesktopSection = "Control Panel\\Desktop";
    private const string SoundsSection = "Sounds";

    public static void ApplyPackFromThemeFile(string themeFilePath)
    {
        if (string.IsNullOrWhiteSpace(themeFilePath) || !File.Exists(themeFilePath))
            return;

        Dictionary<string, Dictionary<string, string>> sections;
        try
        {
            sections = ParseThemeSections(themeFilePath);
        }
        catch
        {
            return;
        }

        ApplyWallpaper(sections, themeFilePath);
        ApplySounds(sections);
        ApplyDesktopIcons(sections);
        SHChangeNotify(ShcneAssocChanged, ShcnfFlush, IntPtr.Zero, IntPtr.Zero);
    }

    private static void ApplyWallpaper(
        IReadOnlyDictionary<string, Dictionary<string, string>> sections,
        string themeFilePath)
    {
        if (!sections.TryGetValue(DesktopSection, out var desktop) ||
            !desktop.TryGetValue("Wallpaper", out var wallpaperRaw) ||
            string.IsNullOrWhiteSpace(wallpaperRaw))
        {
            return;
        }

        var path = VisualStyleCatalog.NormalizeResourcePath(wallpaperRaw, themeFilePath);
        path = Environment.ExpandEnvironmentVariables(path);
        if (!File.Exists(path))
            return;

        if (desktop.TryGetValue("TileWallpaper", out var tile) ||
            desktop.TryGetValue("MultimonBackgrounds", out tile))
        {
            TryWriteDesktopValue("TileWallpaper", tile == "1" ? "1" : "0");
        }

        if (desktop.TryGetValue("WallpaperStyle", out var style) ||
            desktop.TryGetValue("PicturePosition", out style))
        {
            TryWriteDesktopValue("WallpaperStyle", style);
        }

        SystemParametersInfoW(
            SpiSetDeskWallpaper,
            0,
            path,
            SpifUpdateIniFile | SpifSendChange);
    }

    private static void ApplySounds(IReadOnlyDictionary<string, Dictionary<string, string>> sections)
    {
        ResetDefaultSoundCurrents();

        if (sections.TryGetValue(SoundsSection, out var sounds) &&
            sounds.TryGetValue("SchemeName", out var schemeName))
        {
            var isDefault = schemeName.Contains("mmres.dll,-800", StringComparison.OrdinalIgnoreCase);
            TryWriteSchemesDefault(isDefault ? ".Default" : "Custom");
            if (!isDefault)
            {
                try
                {
                    using var key = Registry.CurrentUser.CreateSubKey(@"AppEvents\Schemes\Names\Custom");
                    key?.SetValue("", schemeName, RegistryValueKind.String);
                }
                catch
                {
                    // ignore
                }
            }
        }

        foreach (var (section, values) in sections)
        {
            if (!section.StartsWith("AppEvents\\Schemes\\Apps\\", StringComparison.OrdinalIgnoreCase))
                continue;

            var regPath = section;
            if (!regPath.EndsWith("\\.Current", StringComparison.OrdinalIgnoreCase) &&
                !regPath.EndsWith("\\.Default", StringComparison.OrdinalIgnoreCase))
            {
                regPath += "\\.Current";
            }

            if (!values.TryGetValue("DefaultValue", out var soundPath) &&
                !values.TryGetValue("", out soundPath))
            {
                // В .theme часто значение без имени ключа — первая запись.
                soundPath = values.Values.FirstOrDefault() ?? string.Empty;
            }

            soundPath = Environment.ExpandEnvironmentVariables(soundPath ?? string.Empty).Trim().Trim('"');
            var comma = soundPath.IndexOf(',');
            if (comma > 0)
                soundPath = soundPath[..comma];

            try
            {
                using var key = Registry.CurrentUser.CreateSubKey(regPath);
                if (key is null)
                    continue;
                key.SetValue("", soundPath, RegistryValueKind.String);
            }
            catch
            {
                // ignore
            }
        }
    }

    private static void ApplyDesktopIcons(IReadOnlyDictionary<string, Dictionary<string, string>> sections)
    {
        const string explorerClsid = @"Software\Microsoft\Windows\CurrentVersion\Explorer\CLSID\";
        foreach (var (section, values) in sections)
        {
            string regPath;
            if (section.StartsWith("CLSID\\", StringComparison.OrdinalIgnoreCase))
                regPath = explorerClsid + section["CLSID\\".Length..];
            else if (section.StartsWith(explorerClsid, StringComparison.OrdinalIgnoreCase))
                regPath = section;
            else
                continue;

            if (!regPath.Contains("DefaultIcon", StringComparison.OrdinalIgnoreCase))
                continue;

            foreach (var (name, value) in values)
            {
                var valueName = string.Equals(name, "DefaultValue", StringComparison.OrdinalIgnoreCase)
                    || string.IsNullOrEmpty(name)
                    ? ""
                    : name;
                // Full → default (empty) для корзины, как в BWM.
                if (string.Equals(name, "Full", StringComparison.OrdinalIgnoreCase))
                    valueName = "";

                try
                {
                    using var key = Registry.CurrentUser.CreateSubKey(regPath);
                    key?.SetValue(valueName, value, RegistryValueKind.ExpandString);
                }
                catch
                {
                    // ignore
                }
            }
        }
    }

    private static void ResetDefaultSoundCurrents()
    {
        foreach (var app in new[] { @".Default", @"Explorer" })
        {
            var root = @"AppEvents\Schemes\Apps\" + app;
            try
            {
                using var appKey = Registry.CurrentUser.OpenSubKey(root);
                if (appKey is null)
                    continue;

                foreach (var eventName in appKey.GetSubKeyNames())
                {
                    try
                    {
                        using var defKey = Registry.CurrentUser.OpenSubKey($@"{root}\{eventName}\.Default");
                        var def = defKey?.GetValue("") as string;
                        if (def is null)
                            continue;

                        using var curKey = Registry.CurrentUser.CreateSubKey($@"{root}\{eventName}\.Current");
                        curKey?.SetValue("", Environment.ExpandEnvironmentVariables(def), RegistryValueKind.String);
                    }
                    catch
                    {
                        // ignore
                    }
                }
            }
            catch
            {
                // ignore
            }
        }
    }

    private static void TryWriteSchemesDefault(string value)
    {
        try
        {
            using var key = Registry.CurrentUser.CreateSubKey(@"AppEvents\Schemes");
            key?.SetValue("", value, RegistryValueKind.String);
        }
        catch
        {
            // ignore
        }
    }

    private static void TryWriteDesktopValue(string name, string? value)
    {
        if (value is null)
            return;
        try
        {
            using var key = Registry.CurrentUser.CreateSubKey(@"Control Panel\Desktop");
            key?.SetValue(name, value, RegistryValueKind.String);
        }
        catch
        {
            // ignore
        }
    }

    private static Dictionary<string, Dictionary<string, string>> ParseThemeSections(string themeFilePath)
    {
        var sections = new Dictionary<string, Dictionary<string, string>>(StringComparer.OrdinalIgnoreCase);
        string? current = null;
        foreach (var rawLine in ThemeFileLineReader.ReadLines(themeFilePath))
        {
            var line = rawLine.Trim();
            if (line.Length == 0 || line[0] == ';')
                continue;

            if (line.StartsWith('[') && line.EndsWith(']'))
            {
                current = line[1..^1].Trim();
                if (!sections.ContainsKey(current))
                    sections[current] = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                continue;
            }

            if (current is null)
                continue;

            var separator = line.IndexOf('=');
            if (separator < 0)
                continue;

            var key = line[..separator].Trim();
            var value = line[(separator + 1)..].Trim().Trim('"');
            sections[current][key] = value;
        }

        return sections;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool SystemParametersInfoW(
        uint uiAction,
        uint uiParam,
        string pvParam,
        uint fWinIni);

    [DllImport("shell32.dll")]
    private static extern void SHChangeNotify(uint wEventId, uint uFlags, IntPtr dwItem1, IntPtr dwItem2);
}
