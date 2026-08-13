using System.Net.Http;
using System.Text.Json;
using GlassSettings.Core.Registry;

namespace GlassSettings.Services;

public sealed class OpenGlassReleaseInfo
{
    public required string Tag { get; init; }
    public required string Version { get; init; }
    public required string HtmlUrl { get; init; }
}

public static class OpenGlassReleaseUpdateService
{
    private const string ReleasesApiUrl = "https://api.github.com/repos/ALTaleX531/OpenGlass/releases";
    private static readonly HttpClient Http = CreateHttpClient();

    /// <summary>Ожидает ли интервал проверки новый сетевой запрос.</summary>
    public static bool ShouldRefreshNetworkCheck()
    {
        if (GlassSettingsRegistry.ReadUpdateCheckInterval() == UpdateCheckInterval.Never)
            return false;

        var lastCheck = GlassSettingsRegistry.ReadOpenGlassUpdateLastCheckUtc();
        if (lastCheck is null)
            return true;

        return DateTime.UtcNow - lastCheck.Value >= GetCheckInterval();
    }

    /// <summary>Запрос к GitHub; обновляет LastCheckUtc. Pending пишет координатор.</summary>
    public static async Task<OpenGlassReleaseInfo?> TryFetchLatestLegacyReleaseAsync()
    {
        try
        {
            using var response = await Http.GetAsync(ReleasesApiUrl).ConfigureAwait(false);
            GlassSettingsRegistry.WriteOpenGlassUpdateLastCheckUtc(DateTime.UtcNow);

            if (!response.IsSuccessStatusCode)
                return null;

            await using var stream = await response.Content.ReadAsStreamAsync().ConfigureAwait(false);
            using var doc = await JsonDocument.ParseAsync(stream).ConfigureAwait(false);
            if (doc.RootElement.ValueKind != JsonValueKind.Array)
                return null;

            foreach (var release in doc.RootElement.EnumerateArray())
            {
                if (release.TryGetProperty("prerelease", out var pre) && pre.GetBoolean())
                    continue;

                if (!release.TryGetProperty("tag_name", out var tagEl))
                    continue;

                var tag = tagEl.GetString();
                if (string.IsNullOrWhiteSpace(tag) || !tag.Contains("-legacy", StringComparison.OrdinalIgnoreCase))
                    continue;

                if (!TryParseLegacyTag(tag, out var version))
                    continue;

                var url = release.TryGetProperty("html_url", out var urlEl)
                    ? urlEl.GetString() ?? string.Empty
                    : string.Empty;

                return new OpenGlassReleaseInfo
                {
                    Tag = tag,
                    Version = version,
                    HtmlUrl = url
                };
            }
        }
        catch
        {
            GlassSettingsRegistry.WriteOpenGlassUpdateLastCheckUtc(DateTime.UtcNow);
        }

        return null;
    }

    /// <summary>Pending из реестра (источник истины для карточек).</summary>
    public static OpenGlassReleaseInfo? TryReadPendingRelease()
    {
        var version = GlassSettingsRegistry.ReadOpenGlassUpdatePendingVersion();
        var url = GlassSettingsRegistry.ReadOpenGlassUpdatePendingUrl();
        if (string.IsNullOrWhiteSpace(version) || string.IsNullOrWhiteSpace(url))
            return null;

        return new OpenGlassReleaseInfo
        {
            Tag = "v" + version + "-legacy",
            Version = version,
            HtmlUrl = url
        };
    }

    public static bool IsInstalledOlderThan(string? installedVersion, string latestVersion)
    {
        if (string.IsNullOrWhiteSpace(installedVersion) || string.IsNullOrWhiteSpace(latestVersion))
            return false;

        var installed = ParseVersionParts(installedVersion);
        var latest = ParseVersionParts(latestVersion);
        var count = Math.Max(installed.Length, latest.Length);
        for (var i = 0; i < count; i++)
        {
            var a = i < installed.Length ? installed[i] : 0;
            var b = i < latest.Length ? latest[i] : 0;
            if (a < b) return true;
            if (a > b) return false;
        }

        return false;
    }

    public static bool IsInstalledAtLeast(string? installedVersion, string pendingVersion) =>
        !IsInstalledOlderThan(installedVersion, pendingVersion);

    private static TimeSpan GetCheckInterval() =>
        GlassSettingsRegistry.ReadUpdateCheckInterval() switch
        {
            UpdateCheckInterval.Weekly => TimeSpan.FromDays(7),
            UpdateCheckInterval.Monthly => TimeSpan.FromDays(30),
            _ => TimeSpan.FromHours(24)
        };

    internal static bool TryParseLegacyTag(string tag, out string version)
    {
        version = string.Empty;
        if (!tag.StartsWith("v", StringComparison.OrdinalIgnoreCase))
            return false;

        var body = tag[1..];
        var legacyIdx = body.IndexOf("-legacy", StringComparison.OrdinalIgnoreCase);
        if (legacyIdx < 0)
            return false;

        version = body[..legacyIdx];
        return version.Split('.').Length >= 3;
    }

    private static int[] ParseVersionParts(string version)
    {
        var cleaned = version.Trim();
        var dash = cleaned.IndexOf('-');
        if (dash >= 0)
            cleaned = cleaned[..dash];

        return cleaned.Split('.', StringSplitOptions.RemoveEmptyEntries)
            .Select(p => int.TryParse(p, out var n) ? n : 0)
            .ToArray();
    }

    private static HttpClient CreateHttpClient()
    {
        var client = new HttpClient();
        client.DefaultRequestHeaders.UserAgent.ParseAdd("GlassSettings");
        return client;
    }
}
