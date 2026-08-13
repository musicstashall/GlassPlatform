namespace GlassSettings.Services;

/// <summary>
/// Сохранение «сырого» атласа активной темы (DISKSTREAM PNG + .layout) в каталог AtlasTheme.
/// Подтверждение и перезапись — hosted-диалоги (донор: OpenGlassGUI «Export current theme atlas»).
/// </summary>
public static class ThemeAtlasExportService
{
    public enum ExportStatus
    {
        Saved,
        Cancelled,
        Failed
    }

    public readonly record struct ExportResult(ExportStatus Status, string? Path, int HResult);

    public static async Task<ExportResult> SaveCurrentThemeAtlasAsync()
    {
        var catalogDir = ThemeImageCatalog.TryResolveAtlasCatalogSaveDirectory(createIfMissing: true);
        if (string.IsNullOrWhiteSpace(catalogDir))
            return new ExportResult(ExportStatus.Failed, null, unchecked((int)0x80070003)); // ERROR_PATH_NOT_FOUND

        var fileName = ResolveExportFileName();
        var outputPath = Path.Combine(catalogDir, fileName);

        if (!await ThemeAtlasExportDialogService.ConfirmExportAsync(fileName).ConfigureAwait(true))
            return new ExportResult(ExportStatus.Cancelled, null, 0);

        if (File.Exists(outputPath)
            && !await ThemeAtlasExportDialogService.ConfirmOverwriteAsync(fileName).ConfigureAwait(true))
        {
            return new ExportResult(ExportStatus.Cancelled, outputPath, 0);
        }

        var hr = await Task.Run(() => ThemeImageCatalog.ExportCurrentThemeAtlas(outputPath)).ConfigureAwait(true);

        if (hr == 0 && File.Exists(outputPath))
            return new ExportResult(ExportStatus.Saved, outputPath, hr);

        return new ExportResult(ExportStatus.Failed, outputPath, hr);
    }

    private static string ResolveExportFileName()
    {
        if (VisualStyleThemeManager.TryGetCurrentMsStylesBaseName(out var baseName)
            && !string.IsNullOrWhiteSpace(baseName))
        {
            return baseName + ".png";
        }

        if (VisualStyleThemeManager.TryGetMsStylesBaseNameFromCurrentTheme(out baseName)
            && !string.IsNullOrWhiteSpace(baseName))
        {
            return baseName + ".png";
        }

        return "theme.png";
    }
}
