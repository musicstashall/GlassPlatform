namespace GlassSettings.Core.Registry;

public static partial class GlassSettingsRegistry
{
    public const string GlassUpdateLastCheckUtcValue = "GlassUpdateLastCheckUtc";
    public const string GlassUpdatePendingVersionValue = "GlassUpdatePendingVersion";
    public const string GlassUpdatePendingUrlValue = "GlassUpdatePendingUrl";
    public const string GlassUpdateNoticeDismissedVersionValue = "GlassUpdateNoticeDismissedVersion";

    public static DateTime? ReadGlassUpdateLastCheckUtc()
    {
        var raw = GlassRegistry.ReadDword(SubKey, GlassUpdateLastCheckUtcValue);
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

    public static void WriteGlassUpdateLastCheckUtc(DateTime utc) =>
        GlassRegistry.WriteDword(SubKey, GlassUpdateLastCheckUtcValue, unchecked((uint)utc.ToFileTimeUtc()));

    public static string? ReadGlassUpdatePendingVersion() =>
        ReadString(GlassUpdatePendingVersionValue);

    public static string? ReadGlassUpdatePendingUrl() =>
        ReadString(GlassUpdatePendingUrlValue);

    public static bool HasGlassUpdatePending()
    {
        var url = ReadGlassUpdatePendingUrl();
        var version = ReadGlassUpdatePendingVersion();
        return !string.IsNullOrWhiteSpace(url) && !string.IsNullOrWhiteSpace(version);
    }

    public static void WriteGlassUpdatePending(string version, string htmlUrl)
    {
        var previous = ReadGlassUpdatePendingVersion();
        GlassRegistry.WriteString(SubKey, GlassUpdatePendingVersionValue, version);
        GlassRegistry.WriteString(SubKey, GlassUpdatePendingUrlValue, htmlUrl);
        if (!string.Equals(previous, version, StringComparison.OrdinalIgnoreCase))
            ClearGlassUpdateNoticeDismissedVersion();
    }

    public static string? ReadGlassUpdateNoticeDismissedVersion() =>
        ReadString(GlassUpdateNoticeDismissedVersionValue);

    public static void WriteGlassUpdateNoticeDismissedVersion(string version) =>
        GlassRegistry.WriteString(SubKey, GlassUpdateNoticeDismissedVersionValue, version);

    public static void ClearGlassUpdateNoticeDismissedVersion() =>
        GlassRegistry.DeleteValue(SubKey, GlassUpdateNoticeDismissedVersionValue);

    public static bool IsGlassUpdateNoticeDismissedForPending()
    {
        var pending = ReadGlassUpdatePendingVersion();
        var dismissed = ReadGlassUpdateNoticeDismissedVersion();
        return !string.IsNullOrWhiteSpace(pending)
            && string.Equals(pending, dismissed, StringComparison.OrdinalIgnoreCase);
    }

    public static void ClearGlassUpdatePending()
    {
        GlassRegistry.DeleteValue(SubKey, GlassUpdatePendingVersionValue);
        GlassRegistry.DeleteValue(SubKey, GlassUpdatePendingUrlValue);
        ClearGlassUpdateNoticeDismissedVersion();
    }
}
