using GlassSettings.Core.Platform;
using GlassSettings.Strings;

namespace GlassSettings.Services;

/// <summary>Сохранение именованной темы: OG-секция в Custom.theme → COM CloneAndSetCurrentTheme → verify.</summary>
public static class UserThemeSaveService
{
    public sealed record SaveResult(bool Success, string? ThemeFilePath, string? ErrorDetail);

    /// <summary>
    /// «Сохранить» как в Параметрах Windows: dirty OG DWM относительно baseline темы
    /// или активна Custom.theme (персонализация, в т.ч. смена акцента).
    /// </summary>
    /// <summary>
    /// «Сохранить» как в Параметрах Windows: dirty OG DWM относительно baseline темы
    /// или активна Custom.theme (персонализация, в т.ч. смена акцента).
    /// </summary>
    public static bool ShouldShowSaveButton() =>
        OpenGlassThemeSection.HasNonDefaultControlledSettings()
        || UserThemeCatalog.IsActivePersonalizedTheme();

    public static SaveResult SaveNamedTheme(IntPtr ownerHwnd, string themeDisplayName)
    {
        if (string.IsNullOrWhiteSpace(themeDisplayName))
            return new SaveResult(false, null, "empty name");

        if (!ThemeManager2.TryUpdateCustomTheme(out var updateHr))
        {
            System.Diagnostics.Debug.WriteLine(
                $"GlassSettings UserThemeSaveService.UpdateCustomTheme hr=0x{updateHr:X8}");
        }

        var customPath = EnsureCustomThemePath();
        if (customPath is null)
            return new SaveResult(false, null, "Custom.theme missing");

        if (OpenGlassUiPolicy.IsFeatureExposedInUi()
            && !OpenGlassThemeSection.TryWriteToThemeFile(customPath))
        {
            return new SaveResult(false, null, "Glass.OpenGlass write failed");
        }

        if (!ThemeManager2.TryCloneAndSetCurrentTheme(ownerHwnd, themeDisplayName.Trim(), out var namedPath, out var cloneHr)
            || string.IsNullOrWhiteSpace(namedPath))
        {
            return new SaveResult(false, null, $"CloneAndSetCurrentTheme 0x{cloneHr:X8}");
        }

        namedPath = Path.GetFullPath(namedPath);
        if (OpenGlassUiPolicy.IsFeatureExposedInUi()
            && !OpenGlassThemeSection.HasSection(namedPath)
            && !OpenGlassThemeSection.TryWriteToThemeFile(namedPath))
        {
            return new SaveResult(false, namedPath, "named theme missing Glass.OpenGlass after COM");
        }

        return new SaveResult(true, namedPath, null);
    }

    public static async Task<SaveResult> SaveNamedThemeAsync(IntPtr ownerHwnd, string themeDisplayName)
    {
        return await Task.Run(() =>
        {
            SaveResult? result = null;
            Exception? error = null;
            var thread = new Thread(() =>
            {
                try
                {
                    result = SaveNamedTheme(ownerHwnd, themeDisplayName);
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
                return new SaveResult(false, null, error.Message);

            return result ?? new SaveResult(false, null, "unknown");
        }).ConfigureAwait(false);
    }

    public static string SuggestThemeName()
    {
        var stamp = DateTime.Now.ToString("yyyy-MM-dd HH-mm");
        return GlassSettingsResources.Format("Themes_SaveThemeNameFormat", stamp);
    }

    private static string? EnsureCustomThemePath()
    {
        var existing = UserThemeCatalog.TryGetPersonalizedThemePath();
        if (existing is not null)
            return existing;

        var userDir = UserThemeCatalog.UserThemesDirectory;
        if (string.IsNullOrWhiteSpace(userDir))
            return null;

        Directory.CreateDirectory(userDir);
        var path = Path.Combine(userDir, "Custom.theme");
        if (!File.Exists(path))
        {
            // Минимальный каркас — UpdateCustomTheme обычно создаёт файл; иначе пустая секция.
            try
            {
                File.WriteAllText(path, "[Theme]\r\nDisplayName=Custom\r\n", System.Text.Encoding.Unicode);
            }
            catch
            {
                return null;
            }
        }

        return path;
    }
}
