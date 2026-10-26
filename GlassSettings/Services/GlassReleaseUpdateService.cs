using System.Net.Http;
using System.Text.Json;
using GlassSettings.Core.Registry;

namespace GlassSettings.Services;

public sealed class GlassReleaseInfo
{
    public required string Version { get; init; }
    public required string HtmlUrl { get; init; }
}

/// <summary>
/// Последний релиз Glass: https://github.com/musicstashall/GlassPlatform/releases/latest/download/version.json
/// Поле <c>glass</c> — версия комплекта (GlassBundleVersion / ProductVersion модулей); <c>url</c> — страница релиза.
/// </summary>
public static class GlassReleaseUpdateService
{
    private const string VersionUrl =
        "https://github.com/musicstashall/GlassPlatform/releases/latest/download/version.json";

    private static readonly HttpClient Http = CreateHttpClient();

    public static bool ShouldRefreshNetworkCheck()
    {
        if (GlassSettingsRegistry.ReadUpdateCheckInterval() == UpdateCheckInterval.Never)
            return false;

        var lastCheck = GlassSettingsRegistry.ReadGlassUpdateLastCheckUtc();
        if (lastCheck is null)
            return true;

        return DateTime.UtcNow - lastCheck.Value >= GetCheckInterval();
    }

    public static async Task<GlassReleaseInfo?> TryFetchLatestAsync()
    {
        try
        {
            using var response = await Http.GetAsync(VersionUrl).ConfigureAwait(false);
            GlassSettingsRegistry.WriteGlassUpdateLastCheckUtc(DateTime.UtcNow);
            if (!response.IsSuccessStatusCode)
                return null;

            await using var stream = await response.Content.ReadAsStreamAsync().ConfigureAwait(false);
            using var doc = await JsonDocument.ParseAsync(stream).ConfigureAwait(false);
            if (!doc.RootElement.TryGetProperty("glass", out var glassEl))
                return null;

            var version = glassEl.GetString();
            if (string.IsNullOrWhiteSpace(version))
                return null;

            var url = doc.RootElement.TryGetProperty("url", out var urlEl)
                ? urlEl.GetString() ?? string.Empty
                : string.Empty;
            if (string.IsNullOrWhiteSpace(url))
                return null;

            return new GlassReleaseInfo { Version = version, HtmlUrl = url };
        }
        catch
        {
            GlassSettingsRegistry.WriteGlassUpdateLastCheckUtc(DateTime.UtcNow);
            return null;
        }
    }

    public static GlassReleaseInfo? TryReadPendingRelease()
    {
        var version = GlassSettingsRegistry.ReadGlassUpdatePendingVersion();
        var url = GlassSettingsRegistry.ReadGlassUpdatePendingUrl();
        if (string.IsNullOrWhiteSpace(version) || string.IsNullOrWhiteSpace(url))
            return null;

        return new GlassReleaseInfo { Version = version, HtmlUrl = url };
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

    private static int[] ParseVersionParts(string version)
    {
        var cleaned = version.Trim();
        if (cleaned.StartsWith('v') || cleaned.StartsWith('V'))
            cleaned = cleaned[1..];

        var dash = cleaned.IndexOf('-');
        if (dash >= 0)
            cleaned = cleaned[..dash];

        return cleaned.Split('.', StringSplitOptions.RemoveEmptyEntries)
            .Select(p => int.TryParse(p, out var n) ? n : 0)
            .ToArray();
    }

    private static HttpClient CreateHttpClient()
    {
        var client = new HttpClient { Timeout = TimeSpan.FromSeconds(8) };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("GlassSettings");
        return client;
    }
}
