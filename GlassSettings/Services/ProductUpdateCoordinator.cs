using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;

using Windows.System;



namespace GlassSettings.Services;



/// <summary>

/// Проверка обновлений OpenGlass/Glass: reconcile при старте, pending в реестре, notice по dismissed-версии.

/// </summary>

public static class ProductUpdateCoordinator

{

    private static bool _startupDone;



    public static event EventHandler? PendingUpdatesChanged;



    public static bool ShouldShowOpenGlassNotice =>

        OpenGlassUiPolicy.IsFeatureExposedInUi()

        && HasPendingOpenGlassUpdate() && !GlassSettingsRegistry.IsOpenGlassUpdateNoticeDismissedForPending();



    public static bool HasPendingOpenGlassUpdate() => GlassSettingsRegistry.HasOpenGlassUpdatePending();



    public static OpenGlassReleaseInfo? GetPendingOpenGlass() =>

        OpenGlassReleaseUpdateService.TryReadPendingRelease();



    public static string? GetPendingOpenGlassDownloadUrl() =>

        GlassSettingsRegistry.ReadOpenGlassUpdatePendingUrl();



    public static void DismissOpenGlassNotice()

    {

        var pending = GlassSettingsRegistry.ReadOpenGlassUpdatePendingVersion();

        if (!string.IsNullOrWhiteSpace(pending))

            GlassSettingsRegistry.WriteOpenGlassUpdateNoticeDismissedVersion(pending);

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

        if (OpenGlassReleaseUpdateService.ShouldRefreshNetworkCheck())
            await CheckNetworkForOpenGlassAsync().ConfigureAwait(false);

        if (GlassReleaseUpdateService.ShouldRefreshNetworkCheck())
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
        await CheckNetworkForGlassAsync().ConfigureAwait(false);
        NotifyChanged();
    }



    public static async Task OpenPendingOpenGlassDownloadAsync()

    {

        var url = GetPendingOpenGlassDownloadUrl();

        if (string.IsNullOrWhiteSpace(url))

            return;



        await Launcher.LaunchUriAsync(new Uri(url));

    }



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



        if (!OpenGlassVersionService.TryGetInstalledVersion(out var installed, out _))

            return;



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

        var installed = PlatformModuleVersions.GetGlassPlatformReleaseVersion();
        if (string.IsNullOrWhiteSpace(installed) || PlatformModuleVersions.IsPlaceholderVersion(installed))
            return;

        var pendingVersion = GlassSettingsRegistry.ReadGlassUpdatePendingVersion();
        if (string.IsNullOrWhiteSpace(pendingVersion))
            return;

        if (GlassReleaseUpdateService.IsInstalledAtLeast(installed, pendingVersion))
            GlassSettingsRegistry.ClearGlassUpdatePending();
    }

    private static async Task CheckNetworkForGlassAsync()
    {
        var installed = PlatformModuleVersions.GetGlassPlatformReleaseVersion();
        if (string.IsNullOrWhiteSpace(installed) || PlatformModuleVersions.IsPlaceholderVersion(installed))
            return;

        var latest = await GlassReleaseUpdateService.TryFetchLatestAsync().ConfigureAwait(false);
        if (latest is null)
            return;

        if (!GlassReleaseUpdateService.IsInstalledOlderThan(installed, latest.Version))
            return;

        if (string.IsNullOrWhiteSpace(latest.HtmlUrl))
            return;

        GlassSettingsRegistry.WriteGlassUpdatePending(latest.Version, latest.HtmlUrl);
    }

    private static async Task CheckNetworkForOpenGlassAsync()

    {

        if (!OpenGlassUiPolicy.IsFeatureExposedInUi())

            return;



        if (!OpenGlassVersionService.TryGetInstalledVersion(out var installed, out _))

            return;



        var latest = await OpenGlassReleaseUpdateService.TryFetchLatestLegacyReleaseAsync().ConfigureAwait(false);

        if (latest is null)

            return;



        if (!OpenGlassReleaseUpdateService.IsInstalledOlderThan(installed, latest.Version))

            return;



        if (string.IsNullOrWhiteSpace(latest.HtmlUrl))

            return;



        GlassSettingsRegistry.WriteOpenGlassUpdatePending(latest.Version, latest.HtmlUrl);

    }



    public static bool ShouldShowGlassNotice =>
        HasPendingGlassUpdate() && !GlassSettingsRegistry.IsGlassUpdateNoticeDismissedForPending();

    public static bool HasPendingGlassUpdate() => GlassSettingsRegistry.HasGlassUpdatePending();

    public static GlassReleaseInfo? GetPendingGlass() =>
        GlassReleaseUpdateService.TryReadPendingRelease();

    public static void DismissGlassNotice()
    {
        var pending = GlassSettingsRegistry.ReadGlassUpdatePendingVersion();
        if (!string.IsNullOrWhiteSpace(pending))
            GlassSettingsRegistry.WriteGlassUpdateNoticeDismissedVersion(pending);

        NotifyChanged();
    }

    public static void DismissVisibleUpdateNotice()
    {
        if (ShouldShowOpenGlassNotice)
            DismissOpenGlassNotice();
        else if (ShouldShowGlassNotice)
            DismissGlassNotice();
    }

    public static async Task OpenPendingGlassDownloadAsync()
    {
        var url = GlassSettingsRegistry.ReadGlassUpdatePendingUrl();
        if (string.IsNullOrWhiteSpace(url))
            return;

        await Launcher.LaunchUriAsync(new Uri(url));
    }

    public static async Task OpenVisibleUpdateDownloadAsync()
    {
        if (ShouldShowOpenGlassNotice)
            await OpenPendingOpenGlassDownloadAsync().ConfigureAwait(false);
        else if (ShouldShowGlassNotice)
            await OpenPendingGlassDownloadAsync().ConfigureAwait(false);
    }

    private static void NotifyChanged() => PendingUpdatesChanged?.Invoke(null, EventArgs.Empty);

}

