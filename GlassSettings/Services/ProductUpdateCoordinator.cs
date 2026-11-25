using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;
using GlassSettings.Strings;

namespace GlassSettings.Services;

/// <summary>
/// Проверка обновлений комплекта Glass, его продуктов и двух сторонних продуктов.
/// Новый билд комплекта перекрывает версии Settings и OpenGlass Resources.
/// OpenGlass и SecureUxTheme имеют свои независимые каталоги и закрытие уведомлений.
/// Неудачная проверка не стирает уже найденное обновление и не выглядит как «обновлений нет».
/// Кнопка Glass только запускает закэшированный Setup; сторонние продукты ведут на релиз автора.
/// </summary>
public static class ProductUpdateCoordinator
{
    private static bool _startupDone;

    public static event EventHandler? PendingUpdatesChanged;

    public static bool ShouldShowOpenGlassNotice =>
        OpenGlassUiPolicy.IsFeatureExposedInUi()
        && HasPendingOpenGlassUpdate()
        && !GlassSettingsRegistry.IsOpenGlassUpdateNoticeDismissedForPending();

    public static bool HasPendingOpenGlassUpdate() => GlassSettingsRegistry.HasOpenGlassUpdatePending();

    public static OpenGlassReleaseInfo? GetPendingOpenGlass() =>
        OpenGlassReleaseUpdateService.TryReadPendingRelease();

    public static string? GetPendingOpenGlassDownloadUrl() =>
        GlassSettingsRegistry.ReadOpenGlassUpdatePendingUrl();

    public static void DismissOpenGlassNotice()
    {
        var identity = GlassSettingsRegistry.ReadOpenGlassPendingIdentity();
        if (!string.IsNullOrWhiteSpace(identity))
            GlassSettingsRegistry.WriteOpenGlassUpdateNoticeDismissedVersion(identity);
        NotifyChanged();
    }

    /// <summary>SecureUxTheme не зависит от Glass и OpenGlass: своё предложение и своё закрытие.</summary>
    public static bool ShouldShowSecureUxThemeNotice =>
        HasPendingSecureUxThemeUpdate() && !IsSecureUxThemeNoticeDismissed();

    public static bool HasPendingSecureUxThemeUpdate() =>
        GlassSettingsRegistry.HasSecureUxThemeUpdatePending();

    public static SecureUxThemeReleaseInfo? GetPendingSecureUxTheme() =>
        SecureUxThemeReleaseUpdateService.TryReadPendingRelease();

    public static void DismissSecureUxThemeNotice()
    {
        var version = GlassSettingsRegistry.ReadSecureUxThemeUpdatePendingVersion();
        if (!string.IsNullOrWhiteSpace(version))
            GlassSettingsRegistry.WriteSecureUxThemeUpdateNoticeDismissedVersion(version);
        NotifyChanged();
    }

    private static bool IsSecureUxThemeNoticeDismissed()
    {
        var pending = GlassSettingsRegistry.ReadSecureUxThemeUpdatePendingVersion();
        var dismissed = GlassSettingsRegistry.ReadSecureUxThemeUpdateNoticeDismissedVersion();
        return !string.IsNullOrWhiteSpace(pending)
            && string.Equals(pending, dismissed, StringComparison.OrdinalIgnoreCase);
    }

    public static async Task RunStartupAsync()
    {
        if (_startupDone)
            return;

        _startupDone = true;
        MigrateLegacyDismissedTag();
        ReconcileOpenGlassPendingWithInstalled();
        ReconcileSecureUxThemePendingWithInstalled();
        ReconcileGlassPendingWithInstalled();

        if (OpenGlassReleaseUpdateService.ShouldRefreshNetworkCheck() || NeedsOpenGlassRepairCheck())
            await CheckNetworkForOpenGlassAsync().ConfigureAwait(false);

        if (SecureUxThemeReleaseUpdateService.ShouldRefreshNetworkCheck())
            await CheckNetworkForSecureUxThemeAsync().ConfigureAwait(false);

        if (GlassPlatformUiPolicy.ShouldExposePlatformSections()
            && GlassReleaseUpdateService.ShouldRefreshNetworkCheck())
            await CheckNetworkForGlassAsync().ConfigureAwait(false);

        NotifyChanged();
    }

    public static async Task RefreshOpenGlassFromNetworkAsync()
    {
        if (!OpenGlassUiPolicy.IsFeatureExposedInUi())
            return;

        await CheckNetworkForOpenGlassAsync().ConfigureAwait(false);
        NotifyChanged();
    }

    public static async Task RefreshSecureUxThemeFromNetworkAsync()
    {
        await CheckNetworkForSecureUxThemeAsync().ConfigureAwait(false);
        NotifyChanged();
    }

    public static async Task RefreshGlassFromNetworkAsync()
    {
        if (!GlassPlatformUiPolicy.ShouldExposePlatformSections())
            return;

        await CheckNetworkForGlassAsync().ConfigureAwait(false);
        NotifyChanged();
    }

    public static Task OpenPendingOpenGlassDownloadAsync()
    {
        if (HasPendingOpenGlassUpdate())
            GlassMaintenanceSetupLaunchService.TryLaunch();
        return Task.CompletedTask;
    }

    /// <summary>Сторонний MSI Settings не скачивает и не запускает: только страница релиза автора.</summary>
    public static Task OpenPendingSecureUxThemeDownloadAsync()
    {
        var url = GlassSettingsRegistry.ReadSecureUxThemeUpdatePendingUrl();
        if (HasPendingSecureUxThemeUpdate() && !string.IsNullOrWhiteSpace(url))
        {
            try
            {
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(url)
                {
                    UseShellExecute = true
                });
            }
            catch (Exception error)
            {
                System.Diagnostics.Trace.TraceWarning("[UpdateCheck] SecureUxTheme open release {0}: {1}",
                    error.GetType().Name, error.Message);
            }
        }

        return Task.CompletedTask;
    }

    public static bool ShouldShowGlassNotice =>
        HasPendingGlassUpdate() && !IsGlassNoticeDismissedForVisibleOffers();

    public static bool HasPendingGlassUpdate() => GlassSettingsRegistry.HasGlassUpdatePending();

    public static GlassReleaseInfo? GetPendingGlass() =>
        GlassReleaseUpdateService.TryReadPendingRelease();

    /// <summary>
    /// Закрываются только видимые предложения: комплект или отдельные продукты, но не оба.
    /// Иначе скрытый обновлением комплекта продукт пропал бы после установки комплекта.
    /// </summary>
    public static void DismissGlassNotice()
    {
        var pending = GetPendingGlass();
        if (pending is null)
        {
            GlassSettingsRegistry.DismissGlassPendingOffers();
            NotifyChanged();
            return;
        }

        var visible = GlassReleaseUpdateService.VisibleOffers(pending.Updates);
        if (visible.Count == 0)
        {
            if (!string.IsNullOrWhiteSpace(pending.Version))
                GlassSettingsRegistry.WriteGlassUpdateNoticeDismissedVersion(pending.Version);
        }
        else
        {
            GlassSettingsRegistry.WriteGlassUpdateDismissedOffers(visible.Select(item => item.Token));
        }

        NotifyChanged();
    }

    private static bool IsGlassNoticeDismissedForVisibleOffers()
    {
        var pending = GetPendingGlass();
        if (pending is null)
            return false;

        var visible = GlassReleaseUpdateService.VisibleOffers(pending.Updates);
        if (visible.Count == 0)
        {
            var version = GlassSettingsRegistry.ReadGlassUpdatePendingVersion();
            return !string.IsNullOrWhiteSpace(version)
                && string.Equals(version, GlassSettingsRegistry.ReadGlassUpdateNoticeDismissedVersion(),
                    StringComparison.OrdinalIgnoreCase);
        }

        var dismissed = GlassSettingsRegistry.ReadGlassUpdateDismissedOffers();
        var dismissedTokens = new HashSet<string>(
            (dismissed ?? string.Empty).Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries),
            StringComparer.OrdinalIgnoreCase);
        var legacy = GlassSettingsRegistry.ReadGlassUpdateNoticeDismissedVersion();
        foreach (var offer in visible)
        {
            if (dismissedTokens.Contains(offer.Token))
                continue;
            if (visible.Count == 1 && !string.IsNullOrWhiteSpace(legacy)
                && string.Equals(legacy, offer.Version, StringComparison.OrdinalIgnoreCase))
                continue;
            return false;
        }

        return true;
    }

    public static void DismissVisibleUpdateNotice()
    {
        if (ShouldShowOpenGlassNotice)
            DismissOpenGlassNotice();
        else if (ShouldShowGlassNotice)
            DismissGlassNotice();
        else if (ShouldShowSecureUxThemeNotice)
            DismissSecureUxThemeNotice();
    }

    public static Task OpenPendingGlassDownloadAsync()
    {
        if (HasPendingGlassUpdate())
            GlassMaintenanceSetupLaunchService.TryLaunch();
        return Task.CompletedTask;
    }

    public static Task OpenVisibleUpdateDownloadAsync()
    {
        if (ShouldShowOpenGlassNotice)
            return OpenPendingOpenGlassDownloadAsync();
        if (ShouldShowGlassNotice)
            return OpenPendingGlassDownloadAsync();
        if (ShouldShowSecureUxThemeNotice)
            return OpenPendingSecureUxThemeDownloadAsync();
        return Task.CompletedTask;
    }

    public static string FormatOpenGlassNotice(OpenGlassReleaseInfo info, string installed)
    {
        if (info.Repair)
            return string.Format(GlassSettingsResources.Get("Home_OpenGlassRepairNotice"), info.Version);
        return string.Format(GlassSettingsResources.Get("Home_OpenGlassUpdateNotice"), info.Version, installed);
    }

    public static string FormatOpenGlassSystemNotice(OpenGlassReleaseInfo info, string installed)
    {
        if (info.Repair)
            return string.Format(GlassSettingsResources.Get("System_OpenGlassRepairTitle"), info.Version);
        return string.Format(GlassSettingsResources.Get("System_OpenGlassUpdateTitle"), info.Version, installed);
    }

    public static string FormatOpenGlassLink(OpenGlassReleaseInfo info)
    {
        if (info.Repair)
            return string.Format(GlassSettingsResources.Get("Home_OpenGlassRepairLink"), info.Version);
        return string.Format(GlassSettingsResources.Get("Home_OpenGlassUpdateLinkFormat"), info.Version);
    }

    public static string FormatOpenGlassSpoiler(OpenGlassReleaseInfo info)
    {
        if (info.Repair)
            return string.Format(GlassSettingsResources.Get("System_OpenGlassRepairSpoiler"), info.Version);
        return string.Format(GlassSettingsResources.Get("System_OpenGlassUpdateSpoilerFormat"), info.Version);
    }

    public static string FormatSecureUxThemeNotice(SecureUxThemeReleaseInfo info, string installed) =>
        string.Format(GlassSettingsResources.Get("Home_SecureUxThemeUpdateNotice"), info.Version, installed);

    public static string FormatSecureUxThemeSystemNotice(SecureUxThemeReleaseInfo info, string installed) =>
        string.Format(GlassSettingsResources.Get("System_SecureUxThemeUpdateTitle"), info.Version, installed);

    public static string FormatSecureUxThemeLink(SecureUxThemeReleaseInfo info) =>
        string.Format(GlassSettingsResources.Get("Home_SecureUxThemeUpdateLinkFormat"), info.Version);

    public static string FormatSecureUxThemeSpoiler(SecureUxThemeReleaseInfo info) =>
        string.Format(GlassSettingsResources.Get("System_SecureUxThemeUpdateSpoilerFormat"), info.Version);

    public static string FormatGlassNotice(GlassReleaseInfo info, string installed)
    {
        var visible = info.VisibleUpdates;
        if (visible.Count == 0)
            return string.Format(GlassSettingsResources.Get("Home_GlassUpdateNotice"), info.Version, installed);
        if (visible[0].IsBundle)
            return string.Format(
                GlassSettingsResources.Get("Home_GlassBundleUpdateNotice"),
                GlassReleaseUpdateService.HighestVersion(visible),
                installed);
        if (visible.Count == 1)
            return string.Format(
                GlassSettingsResources.Get("Home_GlassProductUpdateNotice"),
                ProductName(visible[0].Kind),
                visible[0].Version,
                InstalledOfferVersion(visible[0], installed));
        return string.Format(
            GlassSettingsResources.Get("Home_GlassProductUpdateSummary"),
            ProductEntries(visible));
    }

    public static string FormatGlassSystemNotice(GlassReleaseInfo info, string installed)
    {
        var visible = info.VisibleUpdates;
        if (visible.Count == 0)
            return string.Format(GlassSettingsResources.Get("System_GlassUpdateTitle"), info.Version, installed);
        if (visible[0].IsBundle)
            return string.Format(
                GlassSettingsResources.Get("System_GlassBundleUpdateTitle"),
                GlassReleaseUpdateService.HighestVersion(visible),
                installed);
        if (visible.Count == 1)
            return string.Format(
                GlassSettingsResources.Get("System_GlassProductUpdateTitle"),
                ProductName(visible[0].Kind),
                visible[0].Version,
                InstalledOfferVersion(visible[0], installed));
        return string.Format(
            GlassSettingsResources.Get("System_GlassProductUpdateSummary"),
            ProductEntries(visible));
    }

    public static string FormatGlassLink(GlassReleaseInfo info)
    {
        var visible = info.VisibleUpdates;
        if (visible.Count == 0)
            return string.Format(GlassSettingsResources.Get("Home_OpenGlassUpdateLinkFormat"), info.Version);
        if (visible[0].IsBundle)
            return string.Format(
                GlassSettingsResources.Get("Home_GlassBundleUpdateLinkFormat"),
                GlassReleaseUpdateService.HighestVersion(visible));
        if (visible.Count == 1)
            return string.Format(
                GlassSettingsResources.Get("Home_GlassProductUpdateLinkFormat"),
                ProductName(visible[0].Kind),
                visible[0].Version);
        return string.Format(GlassSettingsResources.Get("Home_GlassUpdateLinkSummary"), visible.Count);
    }

    public static string FormatGlassSpoiler(GlassReleaseInfo info)
    {
        var visible = info.VisibleUpdates;
        if (visible.Count == 0)
            return string.Format(GlassSettingsResources.Get("System_OpenGlassUpdateSpoilerFormat"), info.Version);
        if (visible[0].IsBundle)
            return string.Format(
                GlassSettingsResources.Get("System_GlassBundleUpdateSpoilerFormat"),
                GlassReleaseUpdateService.HighestVersion(visible));
        if (visible.Count == 1)
            return string.Format(
                GlassSettingsResources.Get("System_GlassProductUpdateSpoilerFormat"),
                ProductName(visible[0].Kind),
                visible[0].Version);
        return string.Format(GlassSettingsResources.Get("System_GlassUpdateSpoilerSummary"), visible.Count);
    }

    private static string ProductName(GlassUpdateKind kind) => GlassSettingsResources.Get(kind switch
    {
        GlassUpdateKind.Bundle => "GlassUpdate_Name_Bundle",
        GlassUpdateKind.GlassSettings => "GlassUpdate_Name_Settings",
        _ => "GlassUpdate_Name_Resources"
    });

    private static string ProductEntries(IReadOnlyList<GlassProductUpdate> updates) =>
        string.Join(", ", updates
            .GroupBy(item => item.Kind)
            .OrderBy(group => group.Key)
            .Select(group => string.Format(
                GlassSettingsResources.Get("GlassUpdate_ProductEntryFormat"),
                ProductName(group.Key),
                GlassReleaseUpdateService.HighestVersion(group.ToList()))));

    private static void MigrateLegacyDismissedTag()
    {
        if (GlassSettingsRegistry.IsOpenGlassUpdateNoticeDismissedForPending())
            return;

        var pending = GlassSettingsRegistry.ReadOpenGlassUpdatePendingVersion();
        if (string.IsNullOrWhiteSpace(pending))
            return;

        using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(
            RegistryPaths.GlassRoot + "\\GlassSettings", false);
        var legacy = key?.GetValue("OpenGlassUpdateDismissedTag") as string;
        if (string.IsNullOrWhiteSpace(legacy))
            return;

        if (string.Equals(legacy, pending, StringComparison.OrdinalIgnoreCase))
            GlassSettingsRegistry.WriteOpenGlassUpdateNoticeDismissedVersion(pending);
    }

    private static void ReconcileOpenGlassPendingWithInstalled()
    {
        if (!GlassSettingsRegistry.HasOpenGlassUpdatePending())
            return;
        if (!OpenGlassVersionService.TryGetInstalledVersion(out var installed, out var dllPath))
            return;

        var installedChannel = OpenGlassReleaseUpdateService.TryGetInstalledChannel(dllPath);
        var build = OpenGlassReleaseUpdateService.ReadWindowsBuild();
        var buildChannel = build > 0
            ? OpenGlassReleaseUpdateService.ChannelForWindowsBuild(build)
            : (OpenGlassReleaseChannel?)null;

        if (GlassSettingsRegistry.IsOpenGlassUpdateRepair())
        {
            if (installedChannel is { } repaired
                && buildChannel is { } expected
                && repaired == expected)
                GlassSettingsRegistry.ClearOpenGlassUpdatePending();
            return;
        }

        var pendingVersion = GlassSettingsRegistry.ReadOpenGlassUpdatePendingVersion();
        if (string.IsNullOrWhiteSpace(pendingVersion))
            return;

        if (OpenGlassReleaseUpdateService.IsInstalledAtLeast(installed, pendingVersion))
            GlassSettingsRegistry.ClearOpenGlassUpdatePending();
    }

    private static void ReconcileGlassPendingWithInstalled()
    {
        if (!GlassSettingsRegistry.HasGlassUpdatePending())
            return;

        var offers = GlassReleaseUpdateService.ParseOffers(GlassSettingsRegistry.ReadGlassUpdatePendingOffers());
        if (offers.Count == 0)
        {
            // Старый pending без списка продуктов сравнивается с DisplayVersion MSI,
            // как и каталог. ProductVersion DLL сюда не подставляется.
            string? installed = null;
            foreach (var id in new[] { "platform64", "platform32" })
            {
                if (!GlassInstalledProducts.TryReadVersion(id, out var version))
                    continue;
                if (installed is null || GlassReleaseUpdateService.IsInstalledOlderThan(version, installed))
                    installed = version;
            }

            if (string.IsNullOrWhiteSpace(installed))
                return;

            var pendingVersion = GlassSettingsRegistry.ReadGlassUpdatePendingVersion();
            if (string.IsNullOrWhiteSpace(pendingVersion))
                return;

            if (GlassReleaseUpdateService.IsInstalledAtLeast(installed, pendingVersion))
                GlassSettingsRegistry.ClearGlassUpdatePending();
            return;
        }

        var still = offers.Where(GlassInstalledProducts.IsStillOlder).ToList();
        if (still.Count == 0)
        {
            GlassSettingsRegistry.ClearGlassUpdatePending();
            return;
        }

        if (still.Count == offers.Count)
            return;

        var url = GlassSettingsRegistry.ReadGlassUpdatePendingUrl();
        if (string.IsNullOrWhiteSpace(url))
            return;

        GlassSettingsRegistry.WriteGlassUpdatePending(
            DisplayVersion(still),
            url,
            GlassReleaseUpdateService.JoinOffers(still));
    }

    private static void ReconcileSecureUxThemePendingWithInstalled()
    {
        if (!GlassSettingsRegistry.HasSecureUxThemeUpdatePending())
            return;

        var installed = SecureUxThemeMsiService.TryReadInstalledVersion();
        var pending = GlassSettingsRegistry.ReadSecureUxThemeUpdatePendingVersion();
        if (string.IsNullOrWhiteSpace(installed) || string.IsNullOrWhiteSpace(pending))
            return;

        if (!GlassReleaseUpdateService.IsInstalledOlderThan(installed, pending))
            GlassSettingsRegistry.ClearSecureUxThemeUpdatePending();
    }

    private static async Task CheckNetworkForSecureUxThemeAsync()
    {
        // Нет установленного MSI — нет и основанного на версии предложения: оставшиеся
        // файлы или DLL сами по себе не доказывают установку продукта.
        var installed = SecureUxThemeMsiService.TryReadInstalledVersion();
        if (string.IsNullOrWhiteSpace(installed))
            return;

        var check = await SecureUxThemeReleaseUpdateService.TryFetchLatestAsync().ConfigureAwait(false);
        if (!check.Reached || check.Latest is null)
            return;

        if (GlassReleaseUpdateService.IsInstalledOlderThan(installed, check.Latest.Version))
            GlassSettingsRegistry.WriteSecureUxThemeUpdatePending(check.Latest.Version, check.Latest.HtmlUrl);
        else
            GlassSettingsRegistry.ClearSecureUxThemeUpdatePending();
    }

    private static async Task CheckNetworkForGlassAsync()
    {
        var check = await GlassReleaseUpdateService.TryFetchLatestAsync().ConfigureAwait(false);
        if (check.Kind != GlassCatalogCheckKind.Ready)
            return;

        var previous = GlassReleaseUpdateService.ParseOffers(GlassSettingsRegistry.ReadGlassUpdatePendingOffers());
        var merged = MergeGlassOffers(previous, check);
        if (merged.Count == 0)
            GlassSettingsRegistry.ClearGlassUpdatePending();
        else if (!string.IsNullOrWhiteSpace(check.HtmlUrl))
        {
            GlassSettingsRegistry.WriteGlassUpdatePending(
                DisplayVersion(merged),
                check.HtmlUrl,
                GlassReleaseUpdateService.JoinOffers(merged));
        }

    }

    private static async Task CheckNetworkForOpenGlassAsync()
    {
        if (!OpenGlassUiPolicy.IsFeatureExposedInUi())
            return;
        if (!OpenGlassVersionService.TryGetInstalledVersion(out var installed, out var dllPath))
            return;

        var check = await OpenGlassReleaseUpdateService.TryFetchChannelsAsync().ConfigureAwait(false);
        if (!check.Reached)
            return;

        var build = OpenGlassReleaseUpdateService.ReadWindowsBuild();
        var buildChannel = build > 0
            ? OpenGlassReleaseUpdateService.ChannelForWindowsBuild(build)
            : (OpenGlassReleaseChannel?)null;
        var installedChannel = OpenGlassReleaseUpdateService.TryGetInstalledChannel(dllPath);
        var repair = installedChannel is { } installedVariant
            && buildChannel is { } wanted
            && installedVariant != wanted;

        OpenGlassReleaseInfo? offer = null;
        var offerRepair = false;
        if (repair && buildChannel is { } repairChannel)
        {
            // Чужой более новый тег канал не заменяет. Если у нужного канала ещё нет релиза,
            // ремонт всё равно сообщается: кнопка только открывает закэшированный Setup.
            offer = check.For(repairChannel) ?? new OpenGlassReleaseInfo
            {
                Tag = OpenGlassReleaseUpdateService.ChannelId(repairChannel),
                Version = installed ?? "0",
                HtmlUrl = OpenGlassReleaseUpdateService.ReleasesPageUrl,
                Channel = repairChannel,
                Repair = true
            };
            offerRepair = true;
        }
        else
        {
            var channel = installedChannel ?? buildChannel ?? OpenGlassReleaseChannel.Legacy;
            var latest = check.For(channel);
            if (latest is not null && OpenGlassReleaseUpdateService.IsInstalledOlderThan(installed, latest.Version))
                offer = latest;
        }

        if (offer is null)
            GlassSettingsRegistry.ClearOpenGlassUpdatePending();
        else
        {
            GlassSettingsRegistry.WriteOpenGlassUpdateOffer(
                offer.Version,
                offer.HtmlUrl,
                OpenGlassReleaseUpdateService.ChannelId(offer.Channel),
                offerRepair,
                build);
        }

    }

    private static bool NeedsOpenGlassRepairCheck()
    {
        if (GlassSettingsRegistry.ReadUpdateCheckInterval() == UpdateCheckInterval.Never)
            return false;
        if (!OpenGlassUiPolicy.IsFeatureExposedInUi())
            return false;
        if (!OpenGlassVersionService.TryGetInstalledVersion(out _, out var dllPath))
            return false;

        var installedChannel = OpenGlassReleaseUpdateService.TryGetInstalledChannel(dllPath);
        var build = OpenGlassReleaseUpdateService.ReadWindowsBuild();
        if (installedChannel is null || build <= 0)
            return false;

        return installedChannel.Value != OpenGlassReleaseUpdateService.ChannelForWindowsBuild(build);
    }

    private static List<GlassProductUpdate> MergeGlassOffers(
        IReadOnlyList<GlassProductUpdate> previous,
        GlassCatalogCheck check)
    {
        var mentioned = new HashSet<string>(check.MentionedIds, StringComparer.OrdinalIgnoreCase);
        var next = new List<GlassProductUpdate>();
        foreach (var old in previous)
        {
            if (mentioned.Contains(old.Id))
                continue;
            if (GlassInstalledProducts.IsStillOlder(old))
                next.Add(old);
        }

        foreach (var fresh in check.Updates)
        {
            if (next.Any(item => string.Equals(item.Id, fresh.Id, StringComparison.OrdinalIgnoreCase)))
                continue;
            next.Add(fresh);
        }

        return next;
    }

    /// <summary>
    /// pending.version — версия видимого предложения. При обновлении комплекта скрытые
    /// версии Settings/Resources не должны подменять номер билда в уведомлении.
    /// </summary>
    private static string DisplayVersion(IReadOnlyList<GlassProductUpdate> updates)
    {
        var visible = GlassReleaseUpdateService.VisibleOffers(updates);
        return visible.Count == 1 ? visible[0].Version : GlassReleaseUpdateService.HighestVersion(visible);
    }

    private static string InstalledOfferVersion(GlassProductUpdate update, string fallback) =>
        GlassInstalledProducts.TryReadVersion(update.Id, out var installed) ? installed : fallback;

    private static void NotifyChanged() => PendingUpdatesChanged?.Invoke(null, EventArgs.Empty);
}
