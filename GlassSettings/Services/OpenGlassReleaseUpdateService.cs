using System.Diagnostics;
using System.Net.Http;
using System.Text.Json;
using GlassSettings.Core.Registry;
using Microsoft.Win32;

namespace GlassSettings.Services;

public enum OpenGlassReleaseChannel
{
    /// <summary>Тег без суффикса. В официальном EXE это DLL с FileDescription «(MILComp)».</summary>
    Current,

    /// <summary>Тег с суффиксом -legacy. В официальном EXE это DLL с FileDescription «(Legacy)».</summary>
    Legacy
}

public sealed class OpenGlassReleaseInfo
{
    public required string Tag { get; init; }
    public required string Version { get; init; }
    public required string HtmlUrl { get; init; }
    public OpenGlassReleaseChannel Channel { get; init; }
    public bool Repair { get; init; }
}

/// <summary>
/// Оба канала за один ответ GitHub. Reached=false — сеть или разбор не удались,
/// это не «обновлений нет».
/// </summary>
public sealed class OpenGlassChannelCheck
{
    public bool Reached { get; init; }
    public OpenGlassReleaseInfo? Current { get; init; }
    public OpenGlassReleaseInfo? Legacy { get; init; }

    public OpenGlassReleaseInfo? For(OpenGlassReleaseChannel channel) =>
        channel == OpenGlassReleaseChannel.Legacy ? Legacy : Current;
}

public static class OpenGlassReleaseUpdateService
{
    /// <summary>Тот же порог, что GlassOpenGlassPackage: сборка Windows от 28000 берёт MILComp.</summary>
    public const int MilCompWindowsBuild = 28000;

    private const string ReleasesApiUrl =
        "https://api.github.com/repos/ALTaleX531/OpenGlass/releases?per_page=100";

    public const string ReleasesPageUrl = "https://github.com/ALTaleX531/OpenGlass/releases";

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

    /// <summary>
    /// Стабильные теги vA.B.C / vA.B.C.D и те же теги с -legacy.
    /// Каналы не смешиваются: более новый чужой тег не заменяет свой.
    /// </summary>
    public static async Task<OpenGlassChannelCheck> TryFetchChannelsAsync()
    {
        try
        {
            using var response = await Http.GetAsync(ReleasesApiUrl).ConfigureAwait(false);
            if (!response.IsSuccessStatusCode)
                return MarkUnreachable();

            await using var stream = await response.Content.ReadAsStreamAsync().ConfigureAwait(false);
            using var doc = await JsonDocument.ParseAsync(stream).ConfigureAwait(false);
            if (doc.RootElement.ValueKind != JsonValueKind.Array)
                return MarkUnreachable();

            OpenGlassReleaseInfo? current = null;
            OpenGlassReleaseInfo? legacy = null;
            foreach (var release in doc.RootElement.EnumerateArray())
            {
                if (release.TryGetProperty("prerelease", out var pre)
                    && pre.ValueKind == JsonValueKind.True
                    && pre.GetBoolean())
                    continue;
                if (release.TryGetProperty("draft", out var draft)
                    && draft.ValueKind == JsonValueKind.True
                    && draft.GetBoolean())
                    continue;
                if (!release.TryGetProperty("tag_name", out var tagEl))
                    continue;

                var tag = tagEl.GetString();
                if (string.IsNullOrWhiteSpace(tag) || !TryParseReleaseTag(tag, out var version, out var channel))
                    continue;

                var url = release.TryGetProperty("html_url", out var urlEl) ? urlEl.GetString() : null;
                if (string.IsNullOrWhiteSpace(url))
                    url = ReleasesPageUrl;

                var info = new OpenGlassReleaseInfo
                {
                    Tag = tag,
                    Version = version,
                    HtmlUrl = url,
                    Channel = channel
                };
                if (channel == OpenGlassReleaseChannel.Legacy)
                    legacy = PreferNewer(legacy, info);
                else
                    current = PreferNewer(current, info);
            }

            MarkReached();
            return new OpenGlassChannelCheck { Reached = true, Current = current, Legacy = legacy };
        }
        catch
        {
            return MarkUnreachable();
        }
    }

    /// <summary>Pending из реестра. Канал не дописывается как -legacy, если pending его не хранит.</summary>
    public static OpenGlassReleaseInfo? TryReadPendingRelease()
    {
        var version = GlassSettingsRegistry.ReadOpenGlassUpdatePendingVersion();
        var url = GlassSettingsRegistry.ReadOpenGlassUpdatePendingUrl();
        if (string.IsNullOrWhiteSpace(version) || string.IsNullOrWhiteSpace(url))
            return null;

        var channel = OpenGlassReleaseChannel.Legacy;
        if (GlassSettingsRegistry.ReadOpenGlassUpdateChannelId() is { } channelId
            && TryParseChannelId(channelId, out var parsed))
            channel = parsed;
        var repair = GlassSettingsRegistry.IsOpenGlassUpdateRepair();
        return new OpenGlassReleaseInfo
        {
            Tag = channel == OpenGlassReleaseChannel.Legacy ? "v" + version + "-legacy" : "v" + version,
            Version = version,
            HtmlUrl = url,
            Channel = channel,
            Repair = repair
        };
    }

    public static OpenGlassReleaseChannel ChannelForWindowsBuild(int build) =>
        build >= MilCompWindowsBuild ? OpenGlassReleaseChannel.Current : OpenGlassReleaseChannel.Legacy;

    public static int ReadWindowsBuild()
    {
        try
        {
            var view = Environment.Is64BitOperatingSystem ? RegistryView.Registry64 : RegistryView.Default;
            using var hive = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view);
            using var key = hive.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion");
            var text = key?.GetValue("CurrentBuild") as string
                ?? key?.GetValue("CurrentBuildNumber") as string;
            return int.TryParse(text, out var build) ? build : 0;
        }
        catch
        {
            return 0;
        }
    }

    /// <summary>Вариант установленной DLL: суффикс FileDescription, как при разборе официального EXE.</summary>
    public static OpenGlassReleaseChannel? TryGetInstalledChannel(string? dllPath)
    {
        if (string.IsNullOrWhiteSpace(dllPath) || !File.Exists(dllPath))
            return null;

        try
        {
            var description = FileVersionInfo.GetVersionInfo(dllPath).FileDescription;
            if (string.IsNullOrWhiteSpace(description))
                return null;
            if (description.EndsWith("(MILComp)", StringComparison.OrdinalIgnoreCase))
                return OpenGlassReleaseChannel.Current;
            if (description.EndsWith("(Legacy)", StringComparison.OrdinalIgnoreCase))
                return OpenGlassReleaseChannel.Legacy;
        }
        catch
        {
            return null;
        }

        return null;
    }

    public static string ChannelId(OpenGlassReleaseChannel channel) =>
        channel == OpenGlassReleaseChannel.Legacy ? "legacy" : "current";

    public static bool TryParseChannelId(string? text, out OpenGlassReleaseChannel channel)
    {
        if (string.Equals(text, "legacy", StringComparison.OrdinalIgnoreCase))
        {
            channel = OpenGlassReleaseChannel.Legacy;
            return true;
        }

        if (string.Equals(text, "current", StringComparison.OrdinalIgnoreCase))
        {
            channel = OpenGlassReleaseChannel.Current;
            return true;
        }

        channel = OpenGlassReleaseChannel.Current;
        return false;
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

    internal static bool TryParseReleaseTag(string tag, out string version, out OpenGlassReleaseChannel channel)
    {
        version = string.Empty;
        channel = OpenGlassReleaseChannel.Current;
        if (!tag.StartsWith('v') && !tag.StartsWith('V'))
            return false;

        var body = tag[1..];
        const string legacySuffix = "-legacy";
        if (body.EndsWith(legacySuffix, StringComparison.OrdinalIgnoreCase))
        {
            channel = OpenGlassReleaseChannel.Legacy;
            body = body[..^legacySuffix.Length];
        }
        else if (body.Contains('-'))
        {
            return false;
        }

        var parts = body.Split('.');
        if (parts.Length is < 3 or > 4)
            return false;
        foreach (var part in parts)
        {
            if (part.Length == 0 || !int.TryParse(part, out var number) || number < 0)
                return false;
        }

        version = body;
        return true;
    }

    private static OpenGlassReleaseInfo PreferNewer(OpenGlassReleaseInfo? current, OpenGlassReleaseInfo candidate)
    {
        if (current is null || IsInstalledOlderThan(current.Version, candidate.Version))
            return candidate;
        return current;
    }

    private static OpenGlassChannelCheck MarkUnreachable()
    {
        GlassSettingsRegistry.WriteOpenGlassUpdateLastCheckOk(false);
        return new OpenGlassChannelCheck { Reached = false };
    }

    private static void MarkReached()
    {
        GlassSettingsRegistry.WriteOpenGlassUpdateLastCheckUtc(DateTime.UtcNow);
        GlassSettingsRegistry.WriteOpenGlassUpdateLastCheckOk(true);
    }

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

        var cut = cleaned.IndexOfAny(['-', ' ']);
        if (cut >= 0)
            cleaned = cleaned[..cut];

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
