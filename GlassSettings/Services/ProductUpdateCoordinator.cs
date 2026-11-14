using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;
using GlassSettings.Strings;

namespace GlassSettings.Services;

/// <summary>
/// Проверка обновлений OpenGlass и установленных продуктов Glass.
/// Уведомление закрывается отдельно для продукта, архитектуры и версии.
/// Неудачная проверка не стирает уже найденное обновление и не выглядит как «обновлений нет».
/// Кнопки только запускают закэшированный Setup.
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

    public static async Task RunStartupAsync()
    {
        if (_startupDone)
            return;

        _startupDone = true;
        MigrateLegacyDismissedTag();
        ReconcileOpenGlassPendingWithInstalled();
        ReconcileGlassPendingWithInstalled();

        if (OpenGlassReleaseUpdateService.ShouldRefreshNetworkCheck() || NeedsOpenGlassRepairCheck())
            await CheckNetworkForOpenGlassAsync().ConfigureAwait(false);

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

    public static bool ShouldShowGlassNotice =>
        HasPendingGlassUpdate() && !GlassSettingsRegistry.IsGlassUpdateNoticeDismissedForPending();

    public static bool HasPendingGlassUpdate() => GlassSettingsRegistry.HasGlassUpdatePending();

    public static GlassReleaseInfo? GetPendingGlass() =>
        GlassReleaseUpdateService.TryReadPendingRelease();

    public static bool ShouldShowOfflineNotice =>
        !ShouldShowOpenGlassNotice
        && !ShouldShowGlassNotice
        && (GlassSettingsRegistry.IsGlassUpdateLastCheckFailed()
            || (OpenGlassUiPolicy.IsFeatureExposedInUi() && GlassSettingsRegistry.IsOpenGlassUpdateLastCheckFailed()))
        && !GlassSettingsRegistry.IsUpdateCheckOfflineNoticeDismissed();

    public static void DismissGlassNotice()
    {
        GlassSettingsRegistry.DismissGlassPendingOffers();
        NotifyChanged();
    }

    public static void DismissVisibleUpdateNotice()
    {
        if (ShouldShowOpenGlassNotice)
            DismissOpenGlassNotice();
        else if (ShouldShowGlassNotice)
            DismissGlassNotice();
        else if (GlassSettingsRegistry.IsGlassUpdateLastCheckFailed()
                 || (OpenGlassUiPolicy.IsFeatureExposedInUi()
                     && GlassSettingsRegistry.IsOpenGlassUpdateLastCheckFailed()))
        {
            GlassSettingsRegistry.WriteUpdateCheckOfflineNoticeDismissed();
            NotifyChanged();
        }
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
        if (ShouldShowOfflineNotice)
            GlassMaintenanceSetupLaunchService.TryLaunch();
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

    public static string FormatGlassNotice(GlassReleaseInfo info, string installed)
    {
        if (info.Updates.Count > 1)
            return string.Format(GlassSettingsResources.Get("Home_GlassUpdateSummary"), info.Updates.Count);
        if (info.Updates.Count == 1)
            return string.Format(
                GlassSettingsResources.Get("Home_GlassUpdateNotice"),
                info.Updates[0].Version,
                InstalledOfferVersion(info.Updates[0], installed));
        return string.Format(GlassSettingsResources.Get("Home_GlassUpdateNotice"), info.Version, installed);
    }

    public static string FormatGlassSystemNotice(GlassReleaseInfo info, string installed)
    {
        if (info.Updates.Count > 1)
            return string.Format(GlassSettingsResources.Get("System_GlassUpdateSummary"), info.Updates.Count);
        if (info.Updates.Count == 1)
            return string.Format(
                GlassSettingsResources.Get("System_GlassUpdateTitle"),
                info.Updates[0].Version,
                InstalledOfferVersion(info.Updates[0], installed));
        return string.Format(GlassSettingsResources.Get("System_GlassUpdateTitle"), info.Version, installed);
    }

    public static string FormatGlassLink(GlassReleaseInfo info)
    {
        if (info.Updates.Count > 1)
            return string.Format(GlassSettingsResources.Get("Home_GlassUpdateLinkSummary"), info.Updates.Count);
        return string.Format(GlassSettingsResources.Get("Home_OpenGlassUpdateLinkFormat"), info.Version);
    }

    public static string FormatGlassSpoiler(GlassReleaseInfo info)
    {
        if (info.Updates.Count > 1)
            return string.Format(GlassSettingsResources.Get("System_GlassUpdateSpoilerSummary"), info.Updates.Count);
        return string.Format(GlassSettingsResources.Get("System_OpenGlassUpdateSpoilerFormat"), info.Version);
    }

    public static string OfflineNoticeText => GlassSettingsResources.Get("Home_UpdateCheckOffline");

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
            var installed = PlatformModuleVersions.GetGlassPlatformReleaseVersion();
            if (string.IsNullOrWhiteSpace(installed) || PlatformModuleVersions.IsPlaceholderVersion(installed))
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

        NoteSuccessfulCheck();
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

        NoteSuccessfulCheck();
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

    private static string DisplayVersion(IReadOnlyList<GlassProductUpdate> updates) =>
        updates.Count == 1 ? updates[0].Version : GlassReleaseUpdateService.HighestVersion(updates);

    private static string InstalledOfferVersion(GlassProductUpdate update, string fallback) =>
        GlassInstalledProducts.TryReadVersion(update.Id, out var installed) ? installed : fallback;

    private static void NoteSuccessfulCheck()
    {
        if (!GlassSettingsRegistry.IsGlassUpdateLastCheckFailed()
            && !GlassSettingsRegistry.IsOpenGlassUpdateLastCheckFailed())
            GlassSettingsRegistry.ClearUpdateCheckOfflineNoticeDismissed();
    }

    private static void NotifyChanged() => PendingUpdatesChanged?.Invoke(null, EventArgs.Empty);
}
