namespace GlassSettings.Core.Registry;

public static partial class GlassSettingsRegistry
{
    public const string GlassUpdateLastCheckUtcValue = "GlassUpdateLastCheckUtc";
    public const string GlassUpdatePendingVersionValue = "GlassUpdatePendingVersion";
    public const string GlassUpdatePendingUrlValue = "GlassUpdatePendingUrl";
    public const string GlassUpdatePendingOffersValue = "GlassUpdatePendingOffers";
    public const string GlassUpdateNoticeDismissedVersionValue = "GlassUpdateNoticeDismissedVersion";
    public const string GlassUpdateDismissedOffersValue = "GlassUpdateDismissedOffers";
    public const string GlassUpdateLastCheckOkValue = "GlassUpdateLastCheckOk";
    public const string UpdateCheckOfflineNoticeDismissedValue = "UpdateCheckOfflineNoticeDismissed";

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

    public static void WriteGlassUpdateLastCheckOk(bool ok) =>
        GlassRegistry.WriteDword(SubKey, GlassUpdateLastCheckOkValue, ok ? 1u : 0u);

    /// <summary>0 записан после неудачной проверки. Отсутствие значения — проверки ещё не было.</summary>
    public static bool IsGlassUpdateLastCheckFailed() =>
        GlassRegistry.ReadDword(SubKey, GlassUpdateLastCheckOkValue) == 0;

    public static string? ReadGlassUpdatePendingVersion() =>
        ReadString(GlassUpdatePendingVersionValue);

    public static string? ReadGlassUpdatePendingUrl() =>
        ReadString(GlassUpdatePendingUrlValue);

    public static string? ReadGlassUpdatePendingOffers() =>
        ReadString(GlassUpdatePendingOffersValue);

    public static bool HasGlassUpdatePending()
    {
        var url = ReadGlassUpdatePendingUrl();
        var version = ReadGlassUpdatePendingVersion();
        return !string.IsNullOrWhiteSpace(url) && !string.IsNullOrWhiteSpace(version);
    }

    /// <summary>
    /// Закрытые токены продукт|архитектура|версия не сбрасываются: новая версия того же
    /// продукта получает другой токен и уведомляет снова.
    /// </summary>
    public static void WriteGlassUpdatePending(string version, string htmlUrl, string? offers)
    {
        GlassRegistry.WriteString(SubKey, GlassUpdatePendingVersionValue, version);
        GlassRegistry.WriteString(SubKey, GlassUpdatePendingUrlValue, htmlUrl);
        if (string.IsNullOrWhiteSpace(offers))
            GlassRegistry.DeleteValue(SubKey, GlassUpdatePendingOffersValue);
        else
            GlassRegistry.WriteString(SubKey, GlassUpdatePendingOffersValue, offers);
    }

    public static string? ReadGlassUpdateNoticeDismissedVersion() =>
        ReadString(GlassUpdateNoticeDismissedVersionValue);

    public static string? ReadGlassUpdateDismissedOffers() =>
        ReadString(GlassUpdateDismissedOffersValue);

    public static void WriteGlassUpdateNoticeDismissedVersion(string version) =>
        GlassRegistry.WriteString(SubKey, GlassUpdateNoticeDismissedVersionValue, version);

    public static void ClearGlassUpdateNoticeDismissedVersion() =>
        GlassRegistry.DeleteValue(SubKey, GlassUpdateNoticeDismissedVersionValue);

    public static void DismissGlassPendingOffers()
    {
        var pending = ReadGlassUpdatePendingOffers();
        if (string.IsNullOrWhiteSpace(pending))
        {
            var version = ReadGlassUpdatePendingVersion();
            if (!string.IsNullOrWhiteSpace(version))
                WriteGlassUpdateNoticeDismissedVersion(version);
            return;
        }

        var merged = new HashSet<string>(SplitOfferTokens(ReadGlassUpdateDismissedOffers()), StringComparer.OrdinalIgnoreCase);
        foreach (var token in SplitOfferTokens(pending))
            merged.Add(token);

        var ordered = merged.OrderBy(token => token, StringComparer.OrdinalIgnoreCase);
        GlassRegistry.WriteString(SubKey, GlassUpdateDismissedOffersValue, string.Join(';', ordered));
    }

    public static bool IsGlassUpdateNoticeDismissedForPending()
    {
        var pending = SplitOfferTokens(ReadGlassUpdatePendingOffers());
        var dismissed = new HashSet<string>(SplitOfferTokens(ReadGlassUpdateDismissedOffers()), StringComparer.OrdinalIgnoreCase);
        var legacy = ReadGlassUpdateNoticeDismissedVersion();
        if (pending.Count == 0)
        {
            var version = ReadGlassUpdatePendingVersion();
            return !string.IsNullOrWhiteSpace(version)
                && string.Equals(version, legacy, StringComparison.OrdinalIgnoreCase);
        }

        foreach (var token in pending)
        {
            if (dismissed.Contains(token))
                continue;
            if (pending.Count == 1
                && !string.IsNullOrWhiteSpace(legacy)
                && token.EndsWith("|" + legacy, StringComparison.OrdinalIgnoreCase))
                continue;
            return false;
        }

        return true;
    }

    public static void WriteUpdateCheckOfflineNoticeDismissed() =>
        GlassRegistry.WriteDword(SubKey, UpdateCheckOfflineNoticeDismissedValue, 1);

    public static void ClearUpdateCheckOfflineNoticeDismissed() =>
        GlassRegistry.DeleteValue(SubKey, UpdateCheckOfflineNoticeDismissedValue);

    public static bool IsUpdateCheckOfflineNoticeDismissed() =>
        GlassRegistry.ReadDword(SubKey, UpdateCheckOfflineNoticeDismissedValue) == 1;

    public static void ClearGlassUpdatePending()
    {
        GlassRegistry.DeleteValue(SubKey, GlassUpdatePendingVersionValue);
        GlassRegistry.DeleteValue(SubKey, GlassUpdatePendingUrlValue);
        GlassRegistry.DeleteValue(SubKey, GlassUpdatePendingOffersValue);
        GlassRegistry.DeleteValue(SubKey, GlassUpdateDismissedOffersValue);
        ClearGlassUpdateNoticeDismissedVersion();
    }

    private static List<string> SplitOfferTokens(string? text)
    {
        if (string.IsNullOrWhiteSpace(text))
            return [];

        return text.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Where(token => token.Length > 0)
            .ToList();
    }
}
