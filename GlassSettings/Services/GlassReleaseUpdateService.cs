using System.Net.Http;
using System.Text.Json;
using GlassSettings.Core.Registry;

namespace GlassSettings.Services;

/// <summary>Тип предложения каталога. Комплект и отдельные продукты не смешиваются.</summary>
public enum GlassUpdateKind
{
    Bundle,
    GlassSettings,
    OpenGlassResources
}

public sealed class GlassProductUpdate
{
    public required string Id { get; init; }
    public required string Architecture { get; init; }
    public required string Version { get; init; }

    /// <summary>Определяется по id каталога, поэтому старые токены в реестре читаются так же.</summary>
    public GlassUpdateKind Kind => GlassReleaseUpdateService.KindForId(Id);

    public bool IsBundle => Kind == GlassUpdateKind.Bundle;

    public string Token => Id + "|" + Architecture + "|" + Version;

    public static bool TryParse(string token, out GlassProductUpdate? update)
    {
        update = null;
        var parts = token.Split('|');
        if (parts.Length != 3)
            return false;
        if (string.IsNullOrWhiteSpace(parts[0])
            || string.IsNullOrWhiteSpace(parts[1])
            || string.IsNullOrWhiteSpace(parts[2]))
            return false;

        update = new GlassProductUpdate
        {
            Id = parts[0].Trim(),
            Architecture = parts[1].Trim(),
            Version = parts[2].Trim()
        };
        return true;
    }
}

public sealed class GlassReleaseInfo
{
    public required string Version { get; init; }
    public required string HtmlUrl { get; init; }
    public IReadOnlyList<GlassProductUpdate> Updates { get; init; } = [];

    /// <summary>
    /// Обновление комплекта перекрывает отдельные продукты Glass: если есть новый билд
    /// платформы, версии Settings/Resources не показываются даже при их собственных обновлениях.
    /// </summary>
    public IReadOnlyList<GlassProductUpdate> VisibleUpdates =>
        GlassReleaseUpdateService.VisibleOffers(Updates);
}

public enum GlassCatalogCheckKind
{
    Offline,
    Unreadable,
    Ready
}

/// <summary>
/// Результат чтения version.json. Offline и Unreadable не означают «обновлений нет»:
/// прежний pending сохраняется, а время успешной проверки не обновляется.
/// </summary>
public sealed class GlassCatalogCheck
{
    public GlassCatalogCheckKind Kind { get; init; }
    public string HtmlUrl { get; init; } = "";
    public IReadOnlyList<GlassProductUpdate> Updates { get; init; } = [];
    public IReadOnlyList<string> MentionedIds { get; init; } = [];
}

/// <summary>
/// Каталог Glass: https://github.com/musicstashall/GlassPlatform/releases/latest/download/version.json
/// products[] — версии по продукту и архитектуре. Поле glass — запасной вариант только для платформы.
/// Settings не скачивает MSI: кнопка обновления открывает закэшированный Setup.
/// </summary>
public static class GlassReleaseUpdateService
{
    private const string VersionUrl =
        "https://github.com/musicstashall/GlassPlatform/releases/latest/download/version.json";

    /// <summary>Страница релиза, если каталог не содержит отдельного поля url. Кнопка его не открывает.</summary>
    private const string ReleasesPageUrl = "https://github.com/musicstashall/GlassPlatform/releases/latest";

    private static readonly string[] ProductOrder =
    [
        "platform64",
        "platform32",
        "settings64",
        "settings32",
        "openglassResources"
    ];

    private static readonly HttpClient Http = CreateHttpClient();

    /// <summary>
    /// platform64/platform32 — номер билда всего комплекта. Остальные id — отдельные продукты.
    /// OpenGlass и SecureUxTheme сюда не входят: у них свои каталоги и ключи checks.
    /// </summary>
    public static GlassUpdateKind KindForId(string id) => id switch
    {
        "platform64" or "platform32" => GlassUpdateKind.Bundle,
        "settings64" or "settings32" => GlassUpdateKind.GlassSettings,
        "openglassResources" => GlassUpdateKind.OpenGlassResources,
        _ => GlassUpdateKind.GlassSettings
    };

    /// <summary>Видимые предложения: комплект целиком либо только отдельные продукты.</summary>
    public static IReadOnlyList<GlassProductUpdate> VisibleOffers(IReadOnlyList<GlassProductUpdate> updates)
    {
        var bundle = updates.Where(item => item.IsBundle).ToList();
        return bundle.Count > 0 ? bundle : updates.Where(item => !item.IsBundle).ToList();
    }

    public static bool ShouldRefreshNetworkCheck()
    {
        if (GlassSettingsRegistry.ReadUpdateCheckInterval() == UpdateCheckInterval.Never)
            return false;

        var lastCheck = GlassSettingsRegistry.ReadGlassUpdateLastCheckUtc();
        if (lastCheck is null)
            return true;

        return DateTime.UtcNow - lastCheck.Value >= GetCheckInterval();
    }

    public static async Task<GlassCatalogCheck> TryFetchLatestAsync()
    {
        try
        {
            using var response = await Http.GetAsync(VersionUrl).ConfigureAwait(false);
            if (!response.IsSuccessStatusCode)
            {
                System.Diagnostics.Trace.TraceWarning("[UpdateCheck] Glass catalog HTTP {0}", (int)response.StatusCode);
                return MarkUnreachable();
            }

            await using var stream = await response.Content.ReadAsStreamAsync().ConfigureAwait(false);
            using var doc = await JsonDocument.ParseAsync(stream).ConfigureAwait(false);
            if (!TryReadCatalog(doc.RootElement, out var check))
            {
                System.Diagnostics.Trace.TraceWarning("[UpdateCheck] Glass catalog schema is unsupported or empty");
                return MarkUnreachable(GlassCatalogCheckKind.Unreadable);
            }

            MarkReached();
            return check;
        }
        catch (JsonException error)
        {
            System.Diagnostics.Trace.TraceWarning("[UpdateCheck] Glass catalog JSON: {0}", error.Message);
            return MarkUnreachable(GlassCatalogCheckKind.Unreadable);
        }
        catch (Exception error)
        {
            System.Diagnostics.Trace.TraceWarning("[UpdateCheck] Glass catalog {0}: {1}", error.GetType().Name, error.Message);
            return MarkUnreachable();
        }
    }

    public static GlassReleaseInfo? TryReadPendingRelease()
    {
        var version = GlassSettingsRegistry.ReadGlassUpdatePendingVersion();
        var url = GlassSettingsRegistry.ReadGlassUpdatePendingUrl();
        if (string.IsNullOrWhiteSpace(version) || string.IsNullOrWhiteSpace(url))
            return null;

        return new GlassReleaseInfo
        {
            Version = version,
            HtmlUrl = url,
            Updates = ParseOffers(GlassSettingsRegistry.ReadGlassUpdatePendingOffers())
        };
    }

    public static IReadOnlyList<GlassProductUpdate> ParseOffers(string? text)
    {
        if (string.IsNullOrWhiteSpace(text))
            return [];

        var list = new List<GlassProductUpdate>();
        foreach (var part in text.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            if (GlassProductUpdate.TryParse(part, out var update) && update is not null)
                list.Add(update);
        }

        return list;
    }

    public static string JoinOffers(IReadOnlyList<GlassProductUpdate> updates) =>
        string.Join(';', Order(updates).Select(item => item.Token));

    public static string HighestVersion(IReadOnlyList<GlassProductUpdate> updates)
    {
        string? best = null;
        foreach (var update in updates)
        {
            if (best is null || IsInstalledOlderThan(best, update.Version))
                best = update.Version;
        }

        return best ?? string.Empty;
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

    private static bool TryReadCatalog(JsonElement root, out GlassCatalogCheck check)
    {
        check = new GlassCatalogCheck { Kind = GlassCatalogCheckKind.Unreadable };
        if (root.ValueKind != JsonValueKind.Object)
            return false;

        var url = root.TryGetProperty("url", out var urlEl) ? urlEl.GetString() : null;
        if (string.IsNullOrWhiteSpace(url))
            url = ReleasesPageUrl;

        if (root.TryGetProperty("products", out var productsEl))
        {
            if (productsEl.ValueKind != JsonValueKind.Array || productsEl.GetArrayLength() == 0)
                return false;

            var mentioned = new List<string>();
            var updates = new List<GlassProductUpdate>();
            foreach (var product in productsEl.EnumerateArray())
            {
                if (!product.TryGetProperty("id", out var idEl))
                    continue;
                var id = idEl.GetString();
                if (string.IsNullOrWhiteSpace(id) || Array.IndexOf(ProductOrder, id) < 0)
                    continue;
                if (mentioned.Any(item => string.Equals(item, id, StringComparison.OrdinalIgnoreCase)))
                    continue;

                var comparable = ComparableVersion(id, product);
                mentioned.Add(id);
                if (string.IsNullOrWhiteSpace(comparable))
                    continue;
                if (!GlassInstalledProducts.TryReadVersion(id, out var installed))
                    continue;
                if (!IsInstalledOlderThan(installed, comparable))
                    continue;

                updates.Add(new GlassProductUpdate
                {
                    Id = id,
                    Architecture = ArchitectureFor(id),
                    Version = comparable.Trim()
                });
            }

            if (mentioned.Count == 0)
                return false;

            check = new GlassCatalogCheck
            {
                Kind = GlassCatalogCheckKind.Ready,
                HtmlUrl = url,
                Updates = Order(updates),
                MentionedIds = mentioned
            };
            return true;
        }

        if (!root.TryGetProperty("glass", out var glassEl))
            return false;
        var glass = glassEl.GetString();
        if (string.IsNullOrWhiteSpace(glass))
            return false;

        var fallback = new List<GlassProductUpdate>();
        foreach (var id in new[] { "platform64", "platform32" })
        {
            if (!GlassInstalledProducts.TryReadVersion(id, out var installed))
                continue;
            if (!IsInstalledOlderThan(installed, glass))
                continue;
            fallback.Add(new GlassProductUpdate
            {
                Id = id,
                Architecture = ArchitectureFor(id),
                Version = glass.Trim()
            });
        }

        check = new GlassCatalogCheck
        {
            Kind = GlassCatalogCheckKind.Ready,
            HtmlUrl = url,
            Updates = fallback,
            MentionedIds = ["platform64", "platform32"]
        };
        return true;
    }

    /// <summary>
    /// Settings сравнивается по payloadVersion (FileVersion exe). ProductVersion MSI и FileVersion
    /// расходятся, поэтому без payloadVersion продукт в это сравнение не входит.
    /// </summary>
    private static string? ComparableVersion(string id, JsonElement product)
    {
        if (id is "settings64" or "settings32")
        {
            if (!product.TryGetProperty("payloadVersion", out var payload))
                return null;
            var text = payload.GetString();
            return string.IsNullOrWhiteSpace(text) ? null : text;
        }

        if (!product.TryGetProperty("version", out var version))
            return null;
        var value = version.GetString();
        return string.IsNullOrWhiteSpace(value) ? null : value;
    }

    private static string ArchitectureFor(string id) => id switch
    {
        "platform32" or "settings32" => "x86",
        _ => "x64"
    };

    private static IReadOnlyList<GlassProductUpdate> Order(IEnumerable<GlassProductUpdate> updates) =>
        updates
            .OrderBy(item =>
            {
                var index = Array.IndexOf(ProductOrder, item.Id);
                return index < 0 ? ProductOrder.Length : index;
            })
            .ThenBy(item => item.Architecture, StringComparer.OrdinalIgnoreCase)
            .ToList();

    private static GlassCatalogCheck MarkUnreachable(GlassCatalogCheckKind kind = GlassCatalogCheckKind.Offline)
    {
        GlassSettingsRegistry.WriteGlassUpdateLastCheckOk(false);
        return new GlassCatalogCheck { Kind = kind };
    }

    private static void MarkReached()
    {
        GlassSettingsRegistry.WriteGlassUpdateLastCheckUtc(DateTime.UtcNow);
        GlassSettingsRegistry.WriteGlassUpdateLastCheckOk(true);
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
