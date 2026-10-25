namespace GlassSettings.Core.Registry;



using GlassSettings.Strings;



/// <summary>Периодичность проверки новых версий продукта (единая для всех систем).</summary>

public enum UpdateCheckInterval

{

    Daily = 0,

    Weekly = 1,

    Monthly = 2,

    Never = 3

}



/// <summary>Параметры GlassSettings в HKCU\Software\StanislavKa\GlassPlatform\GlassSettings.</summary>

public static partial class GlassSettingsRegistry

{

    private const string SubKey = "GlassSettings";



    public const string EnableSystemFontValue = "EnableSystemFont";

    public const string AppThemeValue = "AppTheme";

    public const string WindowBackdropValue = "WindowBackdrop";

    public const string WindowBusyOverlayModeValue = "WindowBusyOverlayMode";

    public const string OpenGlassUpdateLastCheckUtcValue = "OpenGlassUpdateLastCheckUtc";

    public const string OpenGlassUpdatePendingVersionValue = "OpenGlassUpdatePendingVersion";

    public const string OpenGlassUpdatePendingUrlValue = "OpenGlassUpdatePendingUrl";

    public const string OpenGlassUpdateNoticeDismissedVersionValue = "OpenGlassUpdateNoticeDismissedVersion";

    public const string UpdateCheckIntervalValue = "UpdateCheckInterval";

    public const string AppUiLanguageValue = "AppUiLanguage";

    public const string ExecutableFilePickerLastPathValue = "ExecutableFilePickerLastPath";

    public const string LastAppliedThemeFilePathValue = "LastAppliedThemeFilePath";

    /// <summary>Accent baseline исходной темы (0xAARRGGBB) для Default на Colors.</summary>
    public const string LastAppliedThemeAccentColorValue = "LastAppliedThemeAccentColor";

    /// <summary>Fingerprint DWM (GlassType/Override) после apply темы — baseline для кнопки «Сохранить».</summary>
    public const string LastAppliedThemeDwmFingerprintValue = "LastAppliedThemeDwmFingerprint";



    // Устаревшие ключи (миграция при чтении).

    private const string LegacyCachedTagValue = "OpenGlassUpdateCachedTag";

    private const string LegacyCachedUrlValue = "OpenGlassUpdateCachedUrl";

    private const string LegacyDismissedTagValue = "OpenGlassUpdateDismissedTag";



    public static bool ReadEnableSystemFont(bool defaultValue = false)

    {

        var value = GlassRegistry.ReadDword(SubKey, EnableSystemFontValue);

        return value.HasValue ? value.Value != 0 : defaultValue;

    }



    public static void WriteEnableSystemFont(bool enabled) =>

        GlassRegistry.WriteDword(SubKey, EnableSystemFontValue, enabled ? 1u : 0u);



    public static AppThemeMode ReadAppTheme(AppThemeMode defaultValue = AppThemeMode.System) =>

        ReadEnum(AppThemeValue, defaultValue, static v => v <= (uint)AppThemeMode.Dark);



    public static void WriteAppTheme(AppThemeMode mode) =>

        WriteEnumOrDelete(AppThemeValue, (uint)mode, (uint)AppThemeMode.System);



    public static AppWindowBackdropMode ReadWindowBackdrop(AppWindowBackdropMode defaultValue = AppWindowBackdropMode.System) =>

        ReadEnum(WindowBackdropValue, defaultValue, static v => v <= (uint)AppWindowBackdropMode.MicaAlt);



    public static void WriteWindowBackdrop(AppWindowBackdropMode mode) =>

        WriteEnumOrDelete(WindowBackdropValue, (uint)mode, (uint)AppWindowBackdropMode.System);



    public static WindowBusyOverlayMode ReadWindowBusyOverlayMode(

        WindowBusyOverlayMode defaultValue = WindowBusyOverlayMode.AcrylicChrome) =>

        ReadEnum(WindowBusyOverlayModeValue, defaultValue, static v => v <= (uint)WindowBusyOverlayMode.Dim);



    public static void WriteWindowBusyOverlayMode(WindowBusyOverlayMode mode) =>

        WriteEnumOrDelete(WindowBusyOverlayModeValue, (uint)mode, (uint)WindowBusyOverlayMode.AcrylicChrome);



    /// <summary>BCP-47 или null — системный язык.</summary>
    public static string? ReadAppUiLanguage()
    {
        var stored = ReadString(AppUiLanguageValue);
        if (!string.IsNullOrWhiteSpace(stored))
            return AppUiLanguage.Normalize(stored);

        return ReadLegacyAppUiLanguageDword();
    }



    public static void WriteAppUiLanguage(string? bcp47Tag)
    {
        var normalized = AppUiLanguage.Normalize(bcp47Tag);
        if (normalized is null)
        {
            GlassRegistry.DeleteValue(SubKey, AppUiLanguageValue);
            return;
        }

        if (!UiLanguageCatalog.GetLanguageTags().Contains(normalized, StringComparer.OrdinalIgnoreCase))
            return;

        GlassRegistry.WriteString(SubKey, AppUiLanguageValue, normalized);
    }

    public static string? ReadExecutableFilePickerLastPath() =>
        ReadString(ExecutableFilePickerLastPathValue);

    public static void WriteExecutableFilePickerLastPath(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
            return;

        GlassRegistry.WriteString(SubKey, ExecutableFilePickerLastPathValue, path);
    }

    /// <summary>Последний успешно применённый .theme (исходник для Default акцента на Colors).</summary>
    public static string? ReadLastAppliedThemeFilePath() =>
        ReadString(LastAppliedThemeFilePathValue);

    public static void WriteLastAppliedThemeFilePath(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
            return;

        GlassRegistry.WriteString(SubKey, LastAppliedThemeFilePathValue, path);
    }

    public static uint? ReadLastAppliedThemeAccentColor() =>
        GlassRegistry.ReadDword(SubKey, LastAppliedThemeAccentColorValue);

    public static void WriteLastAppliedThemeAccentColor(uint colorRefDword) =>
        GlassRegistry.WriteDword(SubKey, LastAppliedThemeAccentColorValue, colorRefDword);

    public static string? ReadLastAppliedThemeDwmFingerprint() =>
        ReadString(LastAppliedThemeDwmFingerprintValue);

    public static void WriteLastAppliedThemeDwmFingerprint(string fingerprint)
    {
        if (string.IsNullOrWhiteSpace(fingerprint))
            return;

        GlassRegistry.WriteString(SubKey, LastAppliedThemeDwmFingerprintValue, fingerprint);
    }



    private static string? ReadLegacyAppUiLanguageDword()
    {
        var legacy = GlassRegistry.ReadDword(SubKey, AppUiLanguageValue);
        return legacy switch
        {
            1u => AppUiLanguage.RuRu,
            2u => AppUiLanguage.EnUs,
            _ => null
        };
    }



    public static UpdateCheckInterval ReadUpdateCheckInterval(UpdateCheckInterval defaultValue = UpdateCheckInterval.Daily) =>

        ReadEnum(UpdateCheckIntervalValue, defaultValue, static v => v <= (uint)UpdateCheckInterval.Never);



    public static void WriteUpdateCheckInterval(UpdateCheckInterval value) =>

        WriteEnumOrDelete(UpdateCheckIntervalValue, (uint)value, (uint)UpdateCheckInterval.Daily);



    public static DateTime? ReadOpenGlassUpdateLastCheckUtc()

    {

        var raw = GlassRegistry.ReadDword(SubKey, OpenGlassUpdateLastCheckUtcValue);

        if (!raw.HasValue || raw.Value == 0)

            return null;



        try

        {

            return DateTime.FromFileTimeUtc((long)raw.Value);

        }

        catch

        {

            return null;

        }

    }



    public static void WriteOpenGlassUpdateLastCheckUtc(DateTime utc) =>

        GlassRegistry.WriteDword(SubKey, OpenGlassUpdateLastCheckUtcValue, unchecked((uint)utc.ToFileTimeUtc()));



    public static string? ReadOpenGlassUpdatePendingVersion() =>

        ReadString(OpenGlassUpdatePendingVersionValue)

        ?? ReadString(LegacyCachedTagValue);



    public static string? ReadOpenGlassUpdatePendingUrl() =>

        ReadString(OpenGlassUpdatePendingUrlValue)

        ?? ReadString(LegacyCachedUrlValue);



    public static bool HasOpenGlassUpdatePending()

    {

        var url = ReadOpenGlassUpdatePendingUrl();

        var version = ReadOpenGlassUpdatePendingVersion();

        return !string.IsNullOrWhiteSpace(url) && !string.IsNullOrWhiteSpace(version);

    }



    public static void WriteOpenGlassUpdatePending(string version, string htmlUrl)

    {

        var previous = ReadOpenGlassUpdatePendingVersion();

        GlassRegistry.WriteString(SubKey, OpenGlassUpdatePendingVersionValue, version);

        GlassRegistry.WriteString(SubKey, OpenGlassUpdatePendingUrlValue, htmlUrl);

        GlassRegistry.DeleteValue(SubKey, LegacyCachedTagValue);

        GlassRegistry.DeleteValue(SubKey, LegacyCachedUrlValue);

        GlassRegistry.DeleteValue(SubKey, LegacyDismissedTagValue);

        if (!string.Equals(previous, version, StringComparison.OrdinalIgnoreCase))

            ClearOpenGlassUpdateNoticeDismissedVersion();

    }



    public static string? ReadOpenGlassUpdateNoticeDismissedVersion() =>

        ReadString(OpenGlassUpdateNoticeDismissedVersionValue);



    public static void WriteOpenGlassUpdateNoticeDismissedVersion(string version) =>

        GlassRegistry.WriteString(SubKey, OpenGlassUpdateNoticeDismissedVersionValue, version);



    public static void ClearOpenGlassUpdateNoticeDismissedVersion() =>

        GlassRegistry.DeleteValue(SubKey, OpenGlassUpdateNoticeDismissedVersionValue);



    public static bool IsOpenGlassUpdateNoticeDismissedForPending()

    {

        var pending = ReadOpenGlassUpdatePendingVersion();

        var dismissed = ReadOpenGlassUpdateNoticeDismissedVersion();

        return !string.IsNullOrWhiteSpace(pending)

               && !string.IsNullOrWhiteSpace(dismissed)

               && string.Equals(pending, dismissed, StringComparison.OrdinalIgnoreCase);

    }



    public static void ClearOpenGlassUpdatePending()

    {

        GlassRegistry.DeleteValue(SubKey, OpenGlassUpdatePendingVersionValue);

        GlassRegistry.DeleteValue(SubKey, OpenGlassUpdatePendingUrlValue);

        GlassRegistry.DeleteValue(SubKey, LegacyCachedTagValue);

        GlassRegistry.DeleteValue(SubKey, LegacyCachedUrlValue);

        GlassRegistry.DeleteValue(SubKey, LegacyDismissedTagValue);

        ClearOpenGlassUpdateNoticeDismissedVersion();

    }



    private static string? ReadString(string valueName)

    {

        using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(

            RegistryPaths.GlassRoot + "\\" + SubKey, false);

        return key?.GetValue(valueName) as string;

    }



    private static T ReadEnum<T>(string valueName, T defaultValue, Func<uint, bool> isValid)

        where T : struct, Enum

    {

        var value = GlassRegistry.ReadDword(SubKey, valueName);

        if (!value.HasValue)

            return defaultValue;



        return isValid(value.Value) ? (T)(object)value.Value : defaultValue;

    }



    private static void WriteEnumOrDelete(string valueName, uint value, uint defaultValue)

    {

        if (value == defaultValue)

            GlassRegistry.DeleteValue(SubKey, valueName);

        else

            GlassRegistry.WriteDword(SubKey, valueName, value);

    }

}


