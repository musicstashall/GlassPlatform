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

    }



    public static async Task RunStartupAsync()

    {

        if (_startupDone)

            return;



        _startupDone = true;

        MigrateLegacyDismissedTag();

        ReconcileOpenGlassPendingWithInstalled();



        if (OpenGlassReleaseUpdateService.ShouldRefreshNetworkCheck())

            await CheckNetworkForOpenGlassAsync().ConfigureAwait(false);



        NotifyChanged();

    }



    public static async Task RefreshOpenGlassFromNetworkAsync()

    {

        if (!OpenGlassUiPolicy.IsFeatureExposedInUi())

            return;



        await CheckNetworkForOpenGlassAsync().ConfigureAwait(false);

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



    private static void NotifyChanged() => PendingUpdatesChanged?.Invoke(null, EventArgs.Empty);

}

