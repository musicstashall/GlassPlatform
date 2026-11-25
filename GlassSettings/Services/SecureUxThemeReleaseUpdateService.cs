using System.Diagnostics;
using System.Net.Http;
using System.Text.Json;
using GlassSettings.Core.Registry;

namespace GlassSettings.Services;

public sealed class SecureUxThemeReleaseInfo
{
    public required string Tag { get; init; }
    public required string Version { get; init; }
    public required string HtmlUrl { get; init; }
}

/// <summary>
/// Результат чтения https://api.github.com/repos/namazso/SecureUxTheme/releases/latest.
/// Reached=false — сеть/схема недоступны, это не «обновлений нет» и не стирает pending.
/// </summary>
public sealed class SecureUxThemeReleaseCheck
{
    public bool Reached { get; init; }
    public SecureUxThemeReleaseInfo? Latest { get; init; }
}

/// <summary>
/// Сторонний продукт: свой GitHub-релиз, свои ключи последней проверки и предложения.
/// Не читает catalog Glass, не зависит от проверки OpenGlass.
/// </summary>
public static class SecureUxThemeReleaseUpdateService
{
    private const string LatestReleaseUrl =
        "https://api.github.com/repos/namazso/SecureUxTheme/releases/latest";

    public const string ReleasesPageUrl = "https://github.com/namazso/SecureUxTheme/releases/latest";

    private static readonly HttpClient Http = CreateHttpClient();

    public static bool ShouldRefreshNetworkCheck()
    {
        if (GlassSettingsRegistry.ReadUpdateCheckInterval() == UpdateCheckInterval.Never)
            return false;

        var lastCheck = GlassSettingsRegistry.ReadSecureUxThemeUpdateLastCheckUtc();
        return lastCheck is null
            || DateTime.UtcNow - lastCheck.Value >= GetCheckInterval();
    }

    public static async Task<SecureUxThemeReleaseCheck> TryFetchLatestAsync()
    {
        try
        {
            using var response = await Http.GetAsync(LatestReleaseUrl).ConfigureAwait(false);
            if (!response.IsSuccessStatusCode)
            {
                Trace.TraceWarning("[UpdateCheck] SecureUxTheme releases HTTP {0}", (int)response.StatusCode);
                return MarkUnreachable();
            }

            await using var stream = await response.Content.ReadAsStreamAsync().ConfigureAwait(false);
            using var doc = await JsonDocument.ParseAsync(stream).ConfigureAwait(false);
            if (doc.RootElement.ValueKind != JsonValueKind.Object)
            {
                Trace.TraceWarning("[UpdateCheck] SecureUxTheme releases schema: expected object");
                return MarkUnreachable();
            }

            var root = doc.RootElement;
            if (root.TryGetProperty("prerelease", out var pre) && pre.ValueKind == JsonValueKind.True && pre.GetBoolean())
                return MarkUnreachable();
            if (root.TryGetProperty("draft", out var draft) && draft.ValueKind == JsonValueKind.True && draft.GetBoolean())
                return MarkUnreachable();
            if (!root.TryGetProperty("tag_name", out var tagElement))
                return MarkUnreachable();

            var tag = tagElement.GetString();
            if (!TryParseReleaseTag(tag, out var version))
            {
                Trace.TraceWarning("[UpdateCheck] SecureUxTheme releases unsupported tag: {0}", tag);
                return MarkUnreachable();
            }

            var url = root.TryGetProperty("html_url", out var urlElement) ? urlElement.GetString() : null;
            if (string.IsNullOrWhiteSpace(url))
                url = ReleasesPageUrl;

            MarkReached();
            return new SecureUxThemeReleaseCheck
            {
                Reached = true,
                Latest = new SecureUxThemeReleaseInfo
                {
                    Tag = tag!,
                    Version = version,
                    HtmlUrl = url
                }
            };
        }
        catch (JsonException error)
        {
            Trace.TraceWarning("[UpdateCheck] SecureUxTheme releases JSON: {0}", error.Message);
            return MarkUnreachable();
        }
        catch (Exception error)
        {
            Trace.TraceWarning("[UpdateCheck] SecureUxTheme releases {0}: {1}", error.GetType().Name, error.Message);
            return MarkUnreachable();
        }
    }

    /// <summary>Pending из отдельного ключа. Никаких версий из Glass/OpenGlass.</summary>
    public static SecureUxThemeReleaseInfo? TryReadPendingRelease()
    {
        var version = GlassSettingsRegistry.ReadSecureUxThemeUpdatePendingVersion();
        var url = GlassSettingsRegistry.ReadSecureUxThemeUpdatePendingUrl();
        if (string.IsNullOrWhiteSpace(version) || string.IsNullOrWhiteSpace(url))
            return null;

        return new SecureUxThemeReleaseInfo
        {
            Tag = "v" + version,
            Version = version,
            HtmlUrl = url
        };
    }

    internal static bool TryParseReleaseTag(string? tag, out string version)
    {
        version = string.Empty;
        if (string.IsNullOrWhiteSpace(tag))
            return false;

        var body = tag.Trim();
        if (body.StartsWith('v') || body.StartsWith('V'))
            body = body[1..];
        if (body.Length == 0 || body.Any(character => !char.IsDigit(character) && character != '.'))
            return false;

        var parts = body.Split('.');
        if (parts.Length is < 2 or > 4 || parts.Any(part => part.Length == 0 || !int.TryParse(part, out _)))
            return false;

        version = body;
        return true;
    }

    private static SecureUxThemeReleaseCheck MarkUnreachable()
    {
        GlassSettingsRegistry.WriteSecureUxThemeUpdateLastCheckOk(false);
        return new SecureUxThemeReleaseCheck { Reached = false };
    }

    private static void MarkReached()
    {
        GlassSettingsRegistry.WriteSecureUxThemeUpdateLastCheckUtc(DateTime.UtcNow);
        GlassSettingsRegistry.WriteSecureUxThemeUpdateLastCheckOk(true);
    }

    private static TimeSpan GetCheckInterval() =>
        GlassSettingsRegistry.ReadUpdateCheckInterval() switch
        {
            UpdateCheckInterval.Weekly => TimeSpan.FromDays(7),
            UpdateCheckInterval.Monthly => TimeSpan.FromDays(30),
            _ => TimeSpan.FromHours(24)
        };

    private static HttpClient CreateHttpClient()
    {
        var client = new HttpClient { Timeout = TimeSpan.FromSeconds(8) };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("GlassSettings");
        return client;
    }
}
