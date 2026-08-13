using System.Runtime.InteropServices;
using System.Text;
using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;

namespace GlassSettings.Services;

/// <summary>Результат применения .theme.</summary>
public sealed record VisualStyleApplyResult(bool Success, bool UsedFallback, int HResult, string? ErrorDetail = null);

/// <summary>
/// Менеджер визуальных стилей Windows: detect + полное применение .theme.
/// Pack — IThemeManager2; шрифты — uxtheme #65 (flag 65) при EnableSystemFont.
/// DWM policy — только если OG gate ON и GlassCompat host не жив (иначе host = writer).
/// </summary>
public static class VisualStyleThemeManager
{
    /// <summary>uxtheme #65: применить шрифты sysmetrics из .msstyles.</summary>
    public const int VisualStyleFlagsWithFonts = 65;

    /// <summary>uxtheme #65: стиль без шрифтов.</summary>
    public const int VisualStyleFlagsNoFonts = 0;

    public static bool TryGetActiveTheme(out VisualStyleThemeEntry? entry)
    {
        entry = null;
        if (!TryGetCurrentThemeFilePath(out var currentPath))
            return false;

        var themes = VisualStyleCatalog.GetInstalledThemes();
        entry = themes.FirstOrDefault(theme =>
            string.Equals(
                Path.GetFullPath(theme.ThemeFilePath),
                currentPath,
                StringComparison.OrdinalIgnoreCase));

        if (entry is not null)
            return true;

        entry = VisualStyleCatalog.TryGetEntryForThemeFile(currentPath);
        return entry is not null;
    }

    /// <summary>Синхронный apply на текущем потоке (желательно STA).</summary>
    public static VisualStyleApplyResult ApplyTheme(VisualStyleThemeEntry entry)
    {
        if (entry is null)
            return new VisualStyleApplyResult(false, false, unchecked((int)0x80004003)); // E_POINTER

        if (string.IsNullOrWhiteSpace(entry.ThemeFilePath) || !File.Exists(entry.ThemeFilePath))
            return new VisualStyleApplyResult(false, false, unchecked((int)0x80070002));

        ThemeDwmBaselineSync.BeginThemeChangeWindow();

        var enableFonts = GlassSettingsRegistry.ReadEnableSystemFont();
        var flags = enableFonts ? VisualStyleFlagsWithFonts : VisualStyleFlagsNoFonts;
        var msstyles = entry.VisualStylePath;
        var color = string.IsNullOrWhiteSpace(entry.ColorStyle) ? "NormalColor" : entry.ColorStyle;
        var size = string.IsNullOrWhiteSpace(entry.Size) ? "NormalSize" : entry.Size;
        var applyDwmPolicy = ThemeDwmBaselineSync.ShouldApplyDwmPolicyFromGlassSettings();

        var packOk = ThemeManager2.TryAddAndSelectTheme(entry.ThemeFilePath, out var packHr);
        if (packOk)
        {
            // ThemeManager уже сменил стиль; #65 только для шрифтов.
            if (enableFonts && !string.IsNullOrWhiteSpace(msstyles) && File.Exists(msstyles))
            {
                var hr = NativeHostInterop.ApplyVisualStyleAndDwmPolicy(
                    msstyles,
                    color,
                    size,
                    VisualStyleFlagsWithFonts,
                    applyVisualStyle: true,
                    applyDwmPolicy);
                if (hr < 0)
                    return new VisualStyleApplyResult(false, false, hr, "SetSystemVisualStyle");
            }
            else if (applyDwmPolicy)
            {
                NativeHostInterop.ApplyDwmPolicyOnVisualStyleChange();
            }

            TryApplyOpenGlassThemeSection(entry.ThemeFilePath);
            RememberAppliedThemeSource(entry.ThemeFilePath);
            return new VisualStyleApplyResult(true, false, 0);
        }

        // Fallback: #65 + обои/звуки/иконки из .theme.
        ThemeApplyFallback.ApplyPackFromThemeFile(entry.ThemeFilePath);

        if (!string.IsNullOrWhiteSpace(msstyles) && File.Exists(msstyles))
        {
            var hr = NativeHostInterop.ApplyVisualStyleAndDwmPolicy(
                msstyles,
                color,
                size,
                flags,
                applyVisualStyle: true,
                applyDwmPolicy);
            if (hr < 0)
                return new VisualStyleApplyResult(false, true, hr, $"fallback SetSystemVisualStyle; packHr=0x{packHr:X8}");
        }
        else
        {
            if (applyDwmPolicy)
                NativeHostInterop.ApplyDwmPolicyOnVisualStyleChange();

            return new VisualStyleApplyResult(
                false,
                true,
                packHr,
                "IThemeManager2 failed and .msstyles path missing");
        }

        TryApplyOpenGlassThemeSection(entry.ThemeFilePath);
        RememberAppliedThemeSource(entry.ThemeFilePath);
        return new VisualStyleApplyResult(true, true, packHr);
    }

    private static void RememberAppliedThemeSource(string themeFilePath)
    {
        // Custom.theme — уже персонализированный снимок; не затираем исходник Default.
        if (string.Equals(Path.GetFileName(themeFilePath), "Custom.theme", StringComparison.OrdinalIgnoreCase))
            return;

        GlassSettingsRegistry.WriteLastAppliedThemeFilePath(themeFilePath);
        // Baseline Default = AccentPalette[3] после apply (как пикер/плитка).
        // ColorizationColor из .theme часто другой оттенок (chrome), не путать.
        var live = WindowsPersonalizationService.ReadUserAccentColor();
        GlassSettingsRegistry.WriteLastAppliedThemeAccentColor(
            ThemeAccentColorHelper.ToArgbDword(live));
    }

    private static void TryApplyOpenGlassThemeSection(string themeFilePath)
    {
        if (!OpenGlassUiPolicy.IsFeatureExposedInUi())
            return;

        if (!OpenGlassThemeSection.HasSection(themeFilePath))
            return;

        try
        {
            _ = OpenGlassThemeSection.TryApplyFromThemeFile(themeFilePath);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings TryApplyOpenGlassThemeSection: {ex}");
        }
    }

    /// <summary>Apply на STA-потоке (IThemeManager2 / themeui требуют STA).</summary>
    public static Task<VisualStyleApplyResult> ApplyThemeAsync(VisualStyleThemeEntry entry)
    {
        if (entry is null)
            return Task.FromResult(new VisualStyleApplyResult(false, false, unchecked((int)0x80004003)));

        return Task.Run(() => ApplyThemeOnStaThread(entry));
    }

    private static VisualStyleApplyResult ApplyThemeOnStaThread(VisualStyleThemeEntry entry)
    {
        VisualStyleApplyResult? result = null;
        Exception? error = null;
        var thread = new Thread(() =>
        {
            try
            {
                result = ApplyTheme(entry);
            }
            catch (Exception ex)
            {
                error = ex;
            }
        });
        thread.SetApartmentState(ApartmentState.STA);
        thread.IsBackground = true;
        thread.Start();
        thread.Join();

        if (error is not null)
            return new VisualStyleApplyResult(false, false, error.HResult, error.Message);

        return result ?? new VisualStyleApplyResult(false, false, unchecked((int)0x80004005));
    }

    /// <summary>Полный путь к активному *.theme (реестр CurrentTheme → uxtheme, с проверкой файла).</summary>
    public static bool TryGetCurrentThemeFilePath(out string themeFilePath)
    {
        themeFilePath = string.Empty;

        if (TryReadCurrentThemeFromRegistry(out var registryPath) &&
            TryResolveExistingThemeFile(registryPath, out themeFilePath))
        {
            return true;
        }

        var fileName = new StringBuilder(260);
        var colorScheme = new StringBuilder(260);
        var sizeName = new StringBuilder(260);
        if (GetCurrentThemeName(
                fileName,
                fileName.Capacity,
                colorScheme,
                colorScheme.Capacity,
                sizeName,
                sizeName.Capacity) == 0 &&
            fileName.Length > 0 &&
            TryResolveExistingThemeFile(fileName.ToString(), out themeFilePath))
        {
            return true;
        }

        return false;
    }

    /// <summary>Имя активной темы (как в тултипе плитки).</summary>
    public static string? TryGetCurrentThemeDisplayName() =>
        ThemeDisplayNameResolver.TryResolveForCurrentThemeFile();

    /// <summary>Путь к активному *.theme только из реестра CurrentTheme (для детекта смены темы).</summary>
    public static bool TryGetCurrentThemeRegistryFilePath(out string themeFilePath)
    {
        themeFilePath = string.Empty;
        if (!TryReadCurrentThemeFromRegistry(out var registryPath) || string.IsNullOrWhiteSpace(registryPath))
            return false;

        var normalized = ThemePathNormalizer.Normalize(registryPath);
        if (string.IsNullOrWhiteSpace(normalized))
            normalized = registryPath.Trim().Trim('"');

        if (string.IsNullOrWhiteSpace(normalized))
            return false;

        themeFilePath = Path.GetFullPath(normalized);
        return true;
    }

    /// <summary>Базовое имя активного .msstyles (без расширения) для имён файлов атласа.</summary>
    public static bool TryGetCurrentMsStylesBaseName(out string baseName)
    {
        baseName = string.Empty;
        var fileName = new StringBuilder(260);
        var colorScheme = new StringBuilder(260);
        var sizeName = new StringBuilder(260);
        if (GetCurrentThemeName(
                fileName,
                fileName.Capacity,
                colorScheme,
                colorScheme.Capacity,
                sizeName,
                sizeName.Capacity) != 0
            || fileName.Length == 0)
        {
            return false;
        }

        baseName = Path.GetFileNameWithoutExtension(fileName.ToString());
        return !string.IsNullOrWhiteSpace(baseName);
    }

    /// <summary>Базовое имя .msstyles из [VisualStyles] Path активного .theme (реестр CurrentTheme).</summary>
    public static bool TryGetMsStylesBaseNameFromCurrentTheme(out string baseName)
    {
        baseName = string.Empty;
        if (!TryGetCurrentThemeRegistryFilePath(out var themePath)
            && !TryGetCurrentThemeFilePath(out themePath))
        {
            return false;
        }

        if (!VisualStyleCatalog.TryReadVisualStylePath(themePath, out var msStylesPath)
            || string.IsNullOrWhiteSpace(msStylesPath))
        {
            return false;
        }

        baseName = Path.GetFileNameWithoutExtension(msStylesPath);
        return !string.IsNullOrWhiteSpace(baseName);
    }

    private static bool TryResolveExistingThemeFile(string rawPath, out string themeFilePath)
    {
        themeFilePath = string.Empty;
        var normalized = ThemePathNormalizer.Normalize(rawPath);
        if (string.IsNullOrWhiteSpace(normalized))
            return false;

        normalized = Path.GetFullPath(normalized);
        if (!normalized.EndsWith(".theme", StringComparison.OrdinalIgnoreCase))
            return false;

        if (!File.Exists(normalized))
            return false;

        themeFilePath = normalized;
        return true;
    }

    private static bool TryReadCurrentThemeFromRegistry(out string themeFilePath)
    {
        themeFilePath = string.Empty;
        try
        {
            using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(
                @"Software\Microsoft\Windows\CurrentVersion\Themes");
            if (key?.GetValue("CurrentTheme") is not string value || string.IsNullOrWhiteSpace(value))
                return false;

            themeFilePath = value.Trim().Trim('"');
            return !string.IsNullOrWhiteSpace(themeFilePath);
        }
        catch
        {
            return false;
        }
    }

    [DllImport("uxtheme.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern int GetCurrentThemeName(
        StringBuilder themeFileName,
        int themeFileNameSize,
        StringBuilder colorScheme,
        int colorSchemeSize,
        StringBuilder sizeName,
        int sizeNameSize);
}
