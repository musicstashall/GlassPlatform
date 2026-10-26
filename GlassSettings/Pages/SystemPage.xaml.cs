using GlassSettings.Controls;
using GlassSettings.Core.Platform;
using GlassSettings.Core.Shell;
using GlassSettings.Pages.Dwm;
using GlassSettings.Pages.Glass;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Documents;

namespace GlassSettings.Pages;

public sealed partial class SystemPage : Page
{
    private PlatformSchedulerRow? _ogRow;
    private PlatformSchedulerRow? _glassRow;
    private PlatformSchedulerRow? _glassRowX86;
    private PlatformSchedulerRow? _glassRowX64;
    private SettingsNavigateRow? _ogNav;
    private SettingsNavigateRow? _glassNav;
    private SystemDualGlassPlatformControls? _glassDualControls;
    private bool _shellSuppress;
    private bool _updateNoticeSubscribed;

    public SystemPage()
    {
        InitializeComponent();
        ApplyLocalizedStrings();
        BuildInfoBanner();
        ConfigureUpdateIntervalRow();
        ShellExtensionRow.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.ShellExtension);

        _ogRow = new PlatformSchedulerRow { Kind = PlatformSchedulerKind.OpenGlass };
        _ogNav = NavHost.AddNavigateSection(
            "openglass",
            GlassSettingsResources.Get("System_RowOpenGlass"),
            GlassSettingsResources.Get("System_RowOpenGlassDesc"),
            canNavigate: () => _ogRow.TaskExists,
            onNavigate: h => h.PushPage(typeof(DwmSettingsHubPage), null, GlassSettingsResources.Get("Breadcrumb_DwmSettings")));
        _ogRow.AttachToNavigateRow(_ogNav);
        ApplyOpenGlassUiVisibility();

        if (GlassPlatformNames.ShowDualGlassPlatformUi)
        {
            _glassDualControls = new SystemDualGlassPlatformControls();
            _glassRowX86 = _glassDualControls.RowX86;
            _glassRowX64 = _glassDualControls.RowX64;

            _glassNav = NavHost.AddNavigateSection(
                "glass",
                GlassSettingsResources.Get("System_RowGlassPlatform"),
                GlassSettingsResources.Get("System_RowGlassPlatformDesc"),
                canNavigate: () => _glassRowX86.TaskExists || _glassRowX64.TaskExists,
                onNavigate: PushGlassSettingsHub);
            _glassNav.HeaderActions = _glassDualControls;
            _glassNav.TrailingContent = _glassDualControls.TrailingGates;
            _glassDualControls.StateChanged += (_, _) => RefreshShellRow();
        }
        else
        {
            _glassRow = new PlatformSchedulerRow { Kind = PlatformSchedulerKind.Glass };
            _glassNav = NavHost.AddNavigateSection(
                "glass",
                GlassSettingsResources.Get("System_RowGlassPlatform"),
                GlassSettingsResources.Get("System_RowGlassPlatformDesc"),
                canNavigate: () => _glassRow.TaskExists,
                onNavigate: PushGlassSettingsHub);
            _glassRow.AttachToNavigateRow(_glassNav);
            _glassRow.StateChanged += (_, _) => RefreshShellRow();
        }

        PageShell.PageNoticeStack.ActionNotice.ActionRequested += async (_, _) =>
            await ProductUpdateCoordinator.OpenVisibleUpdateDownloadAsync();
        PageShell.PageNoticeStack.ActionNotice.DismissRequested += (_, _) =>
            ProductUpdateCoordinator.DismissVisibleUpdateNotice();

        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
        _ogRow.StateChanged += (_, _) =>
        {
            RefreshShellRow();
            RefreshOpenGlassRowSubtitle();
        };
    }

    private static void PushGlassSettingsHub(SettingsNavigationHost host) =>
        host.PushPage(typeof(GlassSettingsHubPage), null, GlassSettingsResources.Get("Breadcrumb_GlassSettings"));

    private static bool AnyGlassAutorunTaskExists() =>
        GlassPlatformNames.ShowDualGlassPlatformUi
            ? GlassPlatformService.GlassAutorunTaskExistsForUi(GlassPlatformArch.X86)
              || GlassPlatformService.GlassAutorunTaskExistsForUi(GlassPlatformArch.X64)
            : GlassPlatformService.GlassAutorunTaskExistsForUi(GlassPlatformNames.NativeArch);

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        if (!_updateNoticeSubscribed)
        {
            ProductUpdateCoordinator.PendingUpdatesChanged += OnPendingUpdatesChanged;
            OpenGlassUiPolicy.Changed += OnOpenGlassUiPolicyChanged;
            _updateNoticeSubscribed = true;
        }

        // После NavFrame.Navigated / OnPageNavigated — иначе зеркало notice сбрасывается.
        DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, RefreshAfterShellNavigation);
    }

    private void RefreshAfterShellNavigation()
    {
        ApplyOpenGlassUiVisibility();
        RefreshShellRow();
        RefreshOpenGlassRowSubtitle();
        RefreshGlassRowSubtitle();
        RefreshUpdateNotice();
        _ogNav?.RefreshHeaderLayout();
        _glassNav?.RefreshHeaderLayout();
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        if (!_updateNoticeSubscribed)
            return;

        ProductUpdateCoordinator.PendingUpdatesChanged -= OnPendingUpdatesChanged;
        OpenGlassUiPolicy.Changed -= OnOpenGlassUiPolicyChanged;
        _updateNoticeSubscribed = false;
    }

    private void OnPendingUpdatesChanged(object? sender, EventArgs e) =>
        DispatcherQueue.TryEnqueue(RefreshAfterShellNavigation);

    private void ApplyLocalizedStrings()
    {
        ShellExtensionRow.Title = GlassSettingsResources.Get("System_RowShellExtension");
        ShellExtensionRow.Description = GlassSettingsResources.Get("System_RowShellExtensionDesc");
        UpdateIntervalRow.Title = GlassSettingsResources.Get("System_UpdateIntervalTitle");
        UpdateIntervalRow.Description = GlassSettingsResources.Get("System_UpdateIntervalDesc");
    }

    private bool _updateIntervalSuppress;

    private void ConfigureUpdateIntervalRow()
    {
        UpdateIntervalRow.IconGlyph = "\uE895";
        _updateIntervalSuppress = true;
        try
        {
            UpdateIntervalCombo.Items.Clear();
            UpdateIntervalCombo.Items.Add(GlassSettingsResources.Get("System_UpdateDaily"));
            UpdateIntervalCombo.Items.Add(GlassSettingsResources.Get("System_UpdateWeekly"));
            UpdateIntervalCombo.Items.Add(GlassSettingsResources.Get("System_UpdateMonthly"));
            UpdateIntervalCombo.Items.Add(GlassSettingsResources.Get("System_UpdateNever"));
            UpdateIntervalCombo.SelectedIndex = (int)Core.Registry.GlassSettingsRegistry.ReadUpdateCheckInterval();
        }
        finally
        {
            _updateIntervalSuppress = false;
        }
    }

    private void UpdateIntervalCombo_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_updateIntervalSuppress || UpdateIntervalCombo.SelectedIndex < 0)
            return;

        Core.Registry.GlassSettingsRegistry.WriteUpdateCheckInterval(
            (Core.Registry.UpdateCheckInterval)UpdateIntervalCombo.SelectedIndex);

        _ = ProductUpdateCoordinator.RefreshGlassFromNetworkAsync();
        if (OpenGlassUiPolicy.IsFeatureExposedInUi())
            _ = ProductUpdateCoordinator.RefreshOpenGlassFromNetworkAsync();
    }

    private void OnOpenGlassUiPolicyChanged(object? sender, EventArgs e) =>
        DispatcherQueue.TryEnqueue(RefreshAfterShellNavigation);

    private void ApplyOpenGlassUiVisibility()
    {
        var exposed = OpenGlassUiPolicy.IsFeatureExposedInUi();
        if (_ogNav is not null)
            _ogNav.Visibility = exposed ? Visibility.Visible : Visibility.Collapsed;
        BuildInfoBanner();
    }

    private void BuildInfoBanner()
    {
        var text = SystemInfoBlock.MessageTextBlock;
        text.Inlines.Clear();
        AddInlineText(text, GlassSettingsResources.Get("System_NoticeBannerGlass"));
        text.Inlines.Add(new LineBreak());

        AddInlineText(text, GlassSettingsResources.Get("System_NoticeBannerImportantBefore"));
        AddHyperlink(text, GlassSettingsResources.Get("System_NoticeProcessesLink"), NavigateToProcessesBlockList);
        AddInlineText(text, GlassSettingsResources.Get("System_NoticeBannerImportantAfter"));

        if (!OpenGlassUiPolicy.IsFeatureExposedInUi())
            return;

        text.Inlines.Add(new LineBreak());
        AddInlineText(text, GlassSettingsResources.Get("System_NoticeBannerOpenGlassBefore"));
        AddHyperlink(text, GlassSettingsResources.Get("System_NoticeReflectionLink"), NavigateToReflectionPicker);
        AddInlineText(text, GlassSettingsResources.Get("System_NoticeBannerOpenGlassAfter"));
    }

    private static void AddInlineText(TextBlock target, string text) =>
        target.Inlines.Add(new Run { Text = text });

    private static void AddHyperlink(TextBlock target, string linkText, Action navigate)
    {
        var link = new Hyperlink();
        link.Inlines.Add(new Run
        {
            Text = linkText,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold
        });
        link.Click += (_, _) => navigate();
        target.Inlines.Add(link);
    }

    private void NavigateToProcessesBlockList() =>
        App.ShellWindow?.NavigateToAttachPage();

    private void NavigateToReflectionPicker() =>
        App.ShellWindow?.NavigateHomeDwmReflectionPicker();

    private void RefreshOpenGlassRowSubtitle()
    {
        if (_ogNav is null || !OpenGlassUiPolicy.IsFeatureExposedInUi())
            return;

        _ogNav.Description = GlassSettingsResources.Get("System_RowOpenGlassDesc");

        if (!_ogRow!.TaskExists)
        {
            _ogNav.SetUpdateLine(hasUpdate: false, lineText: null, onLineClick: null);
            return;
        }

        var pending = ProductUpdateCoordinator.GetPendingOpenGlass();
        var hasUpdate = pending is not null;
        _ogNav.SetUpdateLine(
            hasUpdate,
            hasUpdate
                ? string.Format(GlassSettingsResources.Get("System_OpenGlassUpdateSpoilerFormat"), pending!.Version)
                : null,
            hasUpdate ? () => _ = ProductUpdateCoordinator.OpenPendingOpenGlassDownloadAsync() : null);
        _ogNav.RefreshHeaderLayout();
    }

    private void RefreshGlassRowSubtitle()
    {
        var pending = ProductUpdateCoordinator.GetPendingGlass();
        var hasUpdate = pending is not null;
        var line = hasUpdate
            ? string.Format(GlassSettingsResources.Get("System_OpenGlassUpdateSpoilerFormat"), pending!.Version)
            : null;
        Action? click = hasUpdate
            ? () => _ = ProductUpdateCoordinator.OpenPendingGlassDownloadAsync()
            : null;

        _glassNav?.SetUpdateLine(hasUpdate, line, click);
        _glassNav?.RefreshHeaderLayout();
    }

    private async void RefreshUpdateNotice()
    {
        if (ProductUpdateCoordinator.ShouldShowOpenGlassNotice
            && ProductUpdateCoordinator.GetPendingOpenGlass() is { } pendingOg)
        {
            if (!OpenGlassVersionService.TryGetInstalledVersion(out var installedOg, out _))
                installedOg = "—";

            await PageShell.PageNoticeStack.ShowActionNoticeAsync(
                SettingsNoticeSeverity.Warning,
                string.Format(
                    GlassSettingsResources.Get("System_OpenGlassUpdateTitle"),
                    pendingOg.Version,
                    installedOg),
                GlassSettingsResources.Get("System_OpenGlassUpdateAction"),
                animate: !PageShell.PageNoticeStack.ActionNotice.IsVisuallyOpen);
            return;
        }

        if (ProductUpdateCoordinator.ShouldShowGlassNotice
            && ProductUpdateCoordinator.GetPendingGlass() is { } pendingGlass)
        {
            var installedGlass = PlatformModuleVersions.GetGlassPlatformReleaseVersion();
            if (PlatformModuleVersions.IsPlaceholderVersion(installedGlass))
                installedGlass = "—";

            await PageShell.PageNoticeStack.ShowActionNoticeAsync(
                SettingsNoticeSeverity.Warning,
                string.Format(
                    GlassSettingsResources.Get("System_GlassUpdateTitle"),
                    pendingGlass.Version,
                    installedGlass),
                GlassSettingsResources.Get("System_OpenGlassUpdateAction"),
                animate: !PageShell.PageNoticeStack.ActionNotice.IsVisuallyOpen);
            return;
        }

        await PageShell.PageNoticeStack.CloseActionNoticeAsync();
    }

    private void RefreshShellRow()
    {
        if (ShellExtensionService.IsShellExtensionOperationInProgress())
        {
            ShellExtensionSwitch.IsEnabled = false;
            return;
        }

        var taskExists = AnyGlassAutorunTaskExists();
        var registered = ShellExtensionService.IsBundledDllRegistered();
        _shellSuppress = true;
        try
        {
            ShellExtensionSwitch.IsEnabled = registered
                || ShellExtensionService.IsBundledShellExtensionPresent()
                || taskExists;
            ShellExtensionSwitch.IsOn = registered;
        }
        finally
        {
            _shellSuppress = false;
        }

        _ogNav!.CanNavigate = _ogRow!.TaskExists;
        if (_glassDualControls is not null && _glassNav is not null)
        {
            // Snapshot dual x64 может отставать — спойлер и gate должны совпадать с планировщиком.
            var anyTask = GlassPlatformService.GlassAutorunTaskExists(GlassPlatformArch.X86)
                || GlassPlatformService.GlassAutorunTaskExists(GlassPlatformArch.X64);
            _glassNav.CanNavigate = anyTask;
        }
        else if (_glassNav is not null)
        {
            _glassNav.CanNavigate = _glassRow!.TaskExists;
        }
    }

    private async void ShellExtensionSwitch_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_shellSuppress)
            return;

        var want = ShellExtensionSwitch.IsOn;
        var taskExists = AnyGlassAutorunTaskExists();
        if (want && !ShellExtensionService.CanRegisterShellExtension(taskExists))
        {
            _shellSuppress = true;
            ShellExtensionSwitch.IsOn = ShellExtensionService.IsBundledDllRegistered();
            _shellSuppress = false;
            return;
        }

        if (ShellExtensionService.IsShellExtensionOperationInProgress())
        {
            _shellSuppress = true;
            ShellExtensionSwitch.IsOn = ShellExtensionService.IsBundledDllRegistered();
            _shellSuppress = false;
            return;
        }

        _shellSuppress = true;
        ShellExtensionSwitch.IsEnabled = false;
        _shellSuppress = false;

        var ok = await ShellExtensionService.SetShellExtensionRegisteredAsync(want).ConfigureAwait(true);
        if (!ok)
        {
            _shellSuppress = true;
            ShellExtensionSwitch.IsOn = ShellExtensionService.IsBundledDllRegistered();
            _shellSuppress = false;
        }

        RefreshShellRow();
    }
}
