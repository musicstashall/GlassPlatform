namespace GlassSettings.Core.Registry;

/// <summary>
/// Состояние проверки стороннего SecureUxTheme. Не использует ключи Glass и OpenGlass:
/// продукты мониторятся независимо и закрываются отдельно.
/// </summary>
public static partial class GlassSettingsRegistry
{
    public const string SecureUxThemeUpdateLastCheckUtcValue = "SecureUxThemeUpdateLastCheckUtc";
    public const string SecureUxThemeUpdateLastCheckOkValue = "SecureUxThemeUpdateLastCheckOk";
    public const string SecureUxThemeUpdatePendingVersionValue = "SecureUxThemeUpdatePendingVersion";
    public const string SecureUxThemeUpdatePendingUrlValue = "SecureUxThemeUpdatePendingUrl";
    public const string SecureUxThemeUpdateNoticeDismissedVersionValue = "SecureUxThemeUpdateNoticeDismissedVersion";

    public static DateTime? ReadSecureUxThemeUpdateLastCheckUtc() =>
        ReadUpdateCheckUtc(SecureUxThemeUpdateLastCheckUtcValue);

    public static void WriteSecureUxThemeUpdateLastCheckUtc(DateTime utc) =>
        WriteUpdateCheckUtc(SecureUxThemeUpdateLastCheckUtcValue, utc);

    public static void WriteSecureUxThemeUpdateLastCheckOk(bool ok) =>
        GlassRegistry.WriteDword(SubKey, SecureUxThemeUpdateLastCheckOkValue, ok ? 1u : 0u);

    public static string? ReadSecureUxThemeUpdatePendingVersion() =>
        ReadString(SecureUxThemeUpdatePendingVersionValue);

    public static string? ReadSecureUxThemeUpdatePendingUrl() =>
        ReadString(SecureUxThemeUpdatePendingUrlValue);

    public static bool HasSecureUxThemeUpdatePending() =>
        !string.IsNullOrWhiteSpace(ReadSecureUxThemeUpdatePendingUrl())
        && !string.IsNullOrWhiteSpace(ReadSecureUxThemeUpdatePendingVersion());

    public static void WriteSecureUxThemeUpdatePending(string version, string htmlUrl)
    {
        var previous = ReadSecureUxThemeUpdatePendingVersion();
        GlassRegistry.WriteString(SubKey, SecureUxThemeUpdatePendingVersionValue, version);
        GlassRegistry.WriteString(SubKey, SecureUxThemeUpdatePendingUrlValue, htmlUrl);
        if (!string.Equals(previous, version, StringComparison.OrdinalIgnoreCase))
            ClearSecureUxThemeUpdateNoticeDismissedVersion();
    }

    public static string? ReadSecureUxThemeUpdateNoticeDismissedVersion() =>
        ReadString(SecureUxThemeUpdateNoticeDismissedVersionValue);

    public static void WriteSecureUxThemeUpdateNoticeDismissedVersion(string version) =>
        GlassRegistry.WriteString(SubKey, SecureUxThemeUpdateNoticeDismissedVersionValue, version);

    public static void ClearSecureUxThemeUpdateNoticeDismissedVersion() =>
        GlassRegistry.DeleteValue(SubKey, SecureUxThemeUpdateNoticeDismissedVersionValue);

    public static void ClearSecureUxThemeUpdatePending()
    {
        GlassRegistry.DeleteValue(SubKey, SecureUxThemeUpdatePendingVersionValue);
        GlassRegistry.DeleteValue(SubKey, SecureUxThemeUpdatePendingUrlValue);
        ClearSecureUxThemeUpdateNoticeDismissedVersion();
    }
}
