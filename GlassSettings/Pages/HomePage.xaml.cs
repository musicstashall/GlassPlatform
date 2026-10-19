using GlassSettings.Controls;
using GlassSettings.Core.Customization;
using GlassSettings.Core.Dwm;
using GlassSettings.Core.Layout;
using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Shell;
using GlassSettings.Core.Snapshot;
using GlassSettings.Navigation;
using GlassSettings.Pages.Dwm;
using GlassSettings.Pages.Glass;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Windows.Foundation;
using Windows.UI;

namespace GlassSettings.Pages;

public sealed partial class HomePage : Page
{
    private bool _built;
    private EventHandler<DesktopEnvironmentChangedEventArgs>? _environmentChangedHandler;

    private HomeSettingsCard? _systemCard;
    private HomeSettingsCard? _processesCard;
    private HomeComponentGroup? _runtimeSummaryGroup;
    private (int Active, int Rejected, int Residual)? _runtimeCountsKey;
    private string? _systemCardFingerprint;
    private bool _runtimeMonitorSubscribed;
    private HomeSettingsCard? _glassCard;
    private HomeSettingsCard? _dwmCard;
    private HomeSettingsCard? _dwmLocalCard;
    private HomeSettingsCard? _componentsCard;
    private HomeSettingsCard? _subsystemsCard;
    private HomeSettingsCard? _placeholdersCard;
    private HomeSettingsCard? _donationCard;
    private HomeSettingsRow? _themesRow;
    private HomeSettingsRow? _openGlassRow;
    private HomeSettingsRow? _glassPlatformRow;
    private HomeDualGlassPlatformRow? _glassPlatformDualRow;
    private HomeSettingsRow? _shellRow;
    private HomeComponentGroup? _platformComponentGroup;
    private HomeComponentGroup? _openGlassComponentGroup;
    private bool _startupNoticePending = HomeSessionNoticeState.StartupNoticePending;
    private bool _startupNoticeScheduled;
    private GlassSettingsLabeledToggleSwitch? _shellSwitch;
    private GlassSettingsLabeledToggleSwitch? _openGlassSwitch;
    private GlassSettingsLabeledToggleSwitch? _glassPlatformSwitch;
    private HomeCustomizationFooterLine? _summaryFooter;
    private FrameworkElement? _platformSummaryFooter;
    private Button? _saveFooterButton;
    private bool _yellowRebootActive;
    private bool _saveInProgress;
    private CancellationTokenSource? _footerRevealCts;
    private const int FooterRevealMs = 350;
    private SettingsNoticeCoordinator.HomeNoticePageHost? _noticeHost;
    private DesktopWallpaperPreviewTile? _platformWallpaperTile;
    private readonly List<(HomeSettingsRow Row, Func<HomeRowStatus> Status, SettingsCommitScope Scope)> _customizationRows = new();

    public event EventHandler<HomeNavTarget>? NavigateRequested;

    internal SettingsPageShell PageNoticeHost => PageShell;

    internal StackPanel HomeNoticeHostPanel => HomeNoticeHost;

    /// <summary>Якорь viewport-зеркала для pinned yellow/update — ниже штатных полос.</summary>
    internal FrameworkElement HomeNoticeViewportAnchorElement => HomeNoticeViewportAnchor;

    private SettingsNoticeCoordinator.HomeNoticePageHost NoticeHost =>
        _noticeHost ??= SettingsNoticeCoordinator.CreateHomeHost(this);

    public HomePage()
    {
        InitializeComponent();
        ConfigureSaveButtons();
        HomeSessionNoticeState.SummaryFooterRevealedChanged += OnSummaryFooterRevealedChanged;
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
        SettingsSession.Current.DirtyChanged += (_, _) =>
        {
            if (SettingsSession.Current.HasUnsavedChanges)
            {
                HomeSessionNoticeState.InvalidateDismissOnMutation();
                HomeSessionNoticeState.RequestAnimateNextOpen();
            }

            HomeSessionNoticeState.OnUnsavedChanged(SettingsSession.Current.HasUnsavedChanges);
            NoticeMirrorTrace.WriteHome(
                $"DirtyChanged: hasUnsaved={SettingsSession.Current.HasUnsavedChanges}, yellowOpen={UnsavedNoticeStrip.IsVisuallyOpen}");
            RefreshCustomizationUnsavedIndicators();
            SyncSummaryFooterFromSession();
            // Кэшированный Home не должен гонять coordinator/mirror, пока открыта другая страница
            // (иначе ReattachLegacy срывает session/informational notice hub/detail — notices.md).
            if (App.ShellWindow is { } shell && shell.IsCurrentHomePage(this))
                ScheduleNoticeRefreshSettle();
        };
        ProductUpdateCoordinator.PendingUpdatesChanged += OnPendingUpdatesChanged;
        UpdateNoticeStrip.ActionRequested += async (_, _) => await ProductUpdateCoordinator.OpenPendingOpenGlassDownloadAsync();
    }

    private void OnPendingUpdatesChanged(object? sender, EventArgs e) =>
        DispatcherQueue.TryEnqueue(() => _ = RefreshNoticesAsync(animateUpdate: true));

    private void ConfigureSaveButtons()
    {
        // Жёлтое / update — pinned в layout; зеркало по якорю HomeNoticeViewportAnchor.
        // Стартовое green — MirrorOnly; только оно осаждается в левый footer (не любой Success).
        UnsavedNoticeStrip.DisableInformationalMirror = true;
        UpdateNoticeStrip.DisableInformationalMirror = true;

        UnsavedNoticeStrip.RegisterPropertyChangedCallback(
            SettingsUnsavedNoticeStrip.IsStripOpenProperty,
            (_, _) => SyncHomeNoticeHostGap());
        UpdateNoticeStrip.RegisterPropertyChangedCallback(
            SettingsUnsavedNoticeStrip.IsStripOpenProperty,
            (_, _) => SyncHomeNoticeHostGap());
        SyncHomeNoticeHostGap();

        // СТАРТОВОЕ зелёное — ephemeral mirror: activity/крестик → левый footer.
        SessionNoticeStrip.DismissStarting += (_, _) => HomeSessionNoticeState.StartDismissGreen();
        SessionNoticeStrip.DismissRequested += (_, _) =>
        {
            HomeSessionNoticeState.CompleteDismissGreen();
            SyncSummaryFooterFromSession();
            _ = SettingsNoticeCoordinator.RetractInformationalSuccessAsync(NoticeHost, SessionNoticeStrip);
        };

        // ЖЁЛТОЕ (несохранённые) — «Сохранить» + закрытие руками.
        UnsavedNoticeStrip.SaveRequested += async (_, _) => await SaveAllAsync();
        UnsavedNoticeStrip.ActionRequested += (_, _) =>
        {
            if (_yellowRebootActive)
                _ = RebootSystemFromHomePageAsync();
        };
        UnsavedNoticeStrip.DismissStarting += (_, _) => HomeSessionNoticeState.StartDismissYellow();
        UnsavedNoticeStrip.DismissRequested += (_, _) =>
        {
            HomeSessionNoticeState.CompleteDismissYellow(SettingsSession.Current.HasUnsavedChanges);
            SyncSummaryFooterFromSession();
            _ = SettingsNoticeCoordinator.OnUserDismissedAsync(NoticeHost, UnsavedNoticeStrip);
            // После закрытия жёлтого уведомления о перезагрузке — показать кнопку на карточке.
            if (_yellowRebootActive)
            {
                HomeSessionNoticeState.DismissRebootNotice();
                UpdateRebootButtonVisibility();
            }
        };

        UpdateNoticeStrip.DismissRequested += (_, _) =>
        {
            ProductUpdateCoordinator.DismissOpenGlassNotice();
            _ = SettingsNoticeCoordinator.OnUserDismissedAsync(NoticeHost, UpdateNoticeStrip);
        };
    }

    public void OnNavigatedTo()
    {
        if (!_built)
            return;

        NoticeMirrorTrace.WriteHome("OnNavigatedTo");
        HomeSessionNoticeState.EnsureFooterLeftAfterStartupLifecycle();
        HomeSessionNoticeState.SuppressAnimateForNavigation();
        HookEnvironmentWatcher();
        StartRuntimeMonitor();
        Refresh();
        DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, ReattachLegacyNoticeMirror);
    }

    private void ReattachLegacyNoticeMirror() =>
        _ = FinalizeLegacyMirrorAfterLayoutAsync();

    private int _legacyMirrorSettleGen;

    private void ScheduleLegacyMirrorSettle()
    {
        var gen = ++_legacyMirrorSettleGen;
        DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, async () =>
        {
            await Task.Delay(48).ConfigureAwait(true);
            if (gen != _legacyMirrorSettleGen)
                return;

            NoticeMirrorTrace.WriteHome("ScheduleLegacyMirrorSettle");
            await FinalizeLegacyMirrorAfterLayoutAsync().ConfigureAwait(true);
        });
    }

    private async Task FinalizeLegacyMirrorAfterLayoutAsync()
    {
        NoticeMirrorTrace.WriteHome("FinalizeLegacyMirrorAfterLayoutAsync: начало");
        var scroll = PageShell.PageScrollViewer;
        for (var i = 0; i < 16; i++)
        {
            await Task.Delay(16).ConfigureAwait(true);
            scroll.UpdateLayout();
            if (scroll.IsLoaded && scroll.ViewportHeight > 0)
                break;
        }

        if (scroll.ViewportHeight <= 0)
        {
            NoticeMirrorTrace.WriteHome("FinalizeLegacyMirrorAfterLayoutAsync: skip — viewport не готов");
            return;
        }

        NoticeMirrorTrace.WriteHome(
            $"FinalizeLegacyMirrorAfterLayoutAsync: viewportH={scroll.ViewportHeight}, green={SessionNoticeStrip.IsVisuallyOpen}, " +
            $"yellow={UnsavedNoticeStrip.IsVisuallyOpen}");
        await NoticeViewportMirrorService.ReattachLegacyAsync(scroll, HomeNoticeViewportAnchor).ConfigureAwait(true);
        NoticeMirrorTrace.WriteHome("FinalizeLegacyMirrorAfterLayoutAsync: конец");
    }

    /// <summary>Уход с «Обзора»: green one-shot → footer (armed/awaiting); без анимаций.</summary>
    public void OnNavigatedFrom()
    {
        HomeSessionNoticeState.ConsumeGreenForNavigation();
        if (SessionNoticeStrip.IsVisuallyOpen)
            SessionNoticeStrip.CloseImmediate();
        if (UnsavedNoticeStrip.IsVisuallyOpen)
            UnsavedNoticeStrip.CloseImmediate();
        if (UpdateNoticeStrip.IsVisuallyOpen)
            UpdateNoticeStrip.CloseImmediate();
        SyncSummaryFooterFromSession();
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        _footerRevealCts?.Cancel();
        HomeSessionNoticeState.SummaryFooterRevealedChanged -= OnSummaryFooterRevealedChanged;
        UnhookEnvironmentWatcher();
        ProductUpdateCoordinator.PendingUpdatesChanged -= OnPendingUpdatesChanged;
        OpenGlassUiPolicy.Changed -= OnOpenGlassUiPolicyChanged;
        OpenGlassBorderPolicyCoordinator.Changed -= OnOpenGlassBorderPolicyChanged;
        PlatformPresenceService.Instance.PresenceChanged -= OnPresenceChanged;
        PlatformPresenceService.Instance.RemoveSubscriber();
        StopRuntimeMonitor();
        SettingsNoticeCoordinator.ClearHost(this);
    }

    private void OnOpenGlassBorderPolicyChanged(object? sender, EventArgs e) =>
        DispatcherQueue.TryEnqueue(() =>
        {
            _ = RefreshNoticesAsync();
            RefreshCustomizationUnsavedIndicators();
        });

    private void OnOpenGlassUiPolicyChanged(object? sender, EventArgs e) =>
        DispatcherQueue.TryEnqueue(() =>
        {
            ApplyOpenGlassUiVisibility();
            RefreshSystemCard();
            _ = RefreshNoticesAsync(animateUpdate: true);
        });

    private void ApplyOpenGlassUiVisibility()
    {
        var exposed = OpenGlassUiPolicy.IsFeatureExposedInUi();
        if (_openGlassRow is not null)
            _openGlassRow.Visibility = exposed ? Visibility.Visible : Visibility.Collapsed;
        if (_dwmCard is not null)
            _dwmCard.Visibility = exposed ? Visibility.Visible : Visibility.Collapsed;
        if (_dwmLocalCard is not null)
            _dwmLocalCard.Visibility = exposed ? Visibility.Visible : Visibility.Collapsed;
        if (_subsystemsCard is not null)
            _subsystemsCard.Visibility = exposed ? Visibility.Visible : Visibility.Collapsed;
    }

    private int _noticeRefreshSettleGen;

    private void ScheduleNoticeRefreshSettle()
    {
        var gen = ++_noticeRefreshSettleGen;
        DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, async () =>
        {
            await Task.Delay(500).ConfigureAwait(true);
            if (gen != _noticeRefreshSettleGen)
                return;

            await RefreshNoticesAsync().ConfigureAwait(true);
        });
    }

    private void OnPresenceChanged(object? sender, EventArgs e)
    {
        DispatcherQueue.TryEnqueue(() =>
        {
            RefreshSystemCard();
            // Force toggle refresh independent of RefreshSystemCard debounce.
            RefreshGlassPlatformToggleDirect();
            if (SettingsSession.Current.HasUnsavedChanges)
                _ = RefreshNoticesAsync();
        });
    }

    /// <summary>Direct toggle refresh bypassing RefreshSystemCard debounce — ensures toggle
    /// updates immediately on presence changes (e.g., menu-based start/stop).</summary>
    private void RefreshGlassPlatformToggleDirect()
    {
        if (_glassPlatformDualRow is not null)
        {
            _glassPlatformDualRow.Refresh();
            return;
        }

        if (_glassPlatformSwitch is null)
            return;

        HomeStatusSummary.GlassPlatformUiSnapshot? ui = null;
        if (_glassPlatformRow is not null)
        {
            ui = HomeStatusSummary.QueryGlassPlatformUi(GlassPlatformNames.NativeArch);
        }

        if (ui is { } snapshot)
        {
            var startInFlight = GlassPlatformService.IsStartInFlight(GlassPlatformNames.NativeArch, snapshot.Life);
            RefreshPlatformToggle(
                _glassPlatformSwitch,
                ref _glassPlatformSuppress,
                _glassPlatformBusy || snapshot.LifecycleBusy || startInFlight,
                snapshot.Autorun != GlassAutorunUiState.NoTask || startInFlight,
                startInFlight || snapshot.Autorun == GlassAutorunUiState.Running);
        }
    }

    private void OnSummaryFooterRevealedChanged(object? sender, EventArgs e) =>
        DispatcherQueue.TryEnqueue(SyncSummaryFooterFromSession);

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        if (!_built)
        {
            BuildCards();
            _built = true;
            HomePageColumnsLayout.Attach(
                PageShell.PageScrollViewer,
                ColumnsHost,
                LeftColumn,
                RightColumn,
                ColGap,
                ColRight);
        }
        Refresh();
        HookEnvironmentWatcher();
        ApplyOpenGlassUiVisibility();
        OpenGlassUiPolicy.Changed += OnOpenGlassUiPolicyChanged;
        OpenGlassBorderPolicyCoordinator.Changed += OnOpenGlassBorderPolicyChanged;
        // Presence subscription on every load — ensures toggle updates when navigating back.
        PlatformPresenceService.Instance.PresenceChanged -= OnPresenceChanged;
        PlatformPresenceService.Instance.PresenceChanged += OnPresenceChanged;
        PlatformPresenceService.Instance.AddSubscriber();
        // RuntimeMonitor/PostNav — после выхода из Loaded (иначе x86 AV до следующего pump).
        App.ShellWindow?.ScheduleColdStartPostNavAfterHomeLoaded(this);
    }

    /// <summary>После PostNav Attach — подписка RuntimeMonitor.</summary>
    internal void AttachColdStartRuntimeHooks()
    {
        StartRuntimeMonitor();
    }

    /// <summary>После готовности DesktopWallpaperImageCache — до снятия заставки (иначе пустой слот).</summary>
    internal void RefreshWallpaperPreviewUnderSplash()
    {
        _platformWallpaperTile?.ForceRefreshPreview();
    }

    private void HookEnvironmentWatcher()
    {
        if (_environmentChangedHandler is not null ||
            DesktopEnvironmentWatcher.Current is not { } watcher)
        {
            return;
        }

        _environmentChangedHandler = (_, e) => OnDesktopEnvironmentChanged(e);
        watcher.Changed += _environmentChangedHandler;
    }

    private void UnhookEnvironmentWatcher()
    {
        if (_environmentChangedHandler is null ||
            DesktopEnvironmentWatcher.Current is not { } watcher)
        {
            return;
        }

        watcher.Changed -= _environmentChangedHandler;
        _environmentChangedHandler = null;
    }

    private void OnDesktopEnvironmentChanged(DesktopEnvironmentChangedEventArgs e)
    {
        if (e.WallpaperChanged)
            _platformWallpaperTile?.ForceRefreshPreview();

        if (e.ThemeChanged || e.AccentChanged)
        {
            RefreshPlaceholdersCard();
            RefreshSystemCard();
        }
    }

    private void BuildCards()
    {
        BuildPlatformSummary();
        BuildSystemCard();
        BuildProcessesCard();
        BuildGlassCard();
        BuildDwmCards();
        BuildComponentsCard();
        BuildSubsystemsCard();
        BuildPlaceholdersCard();
        BuildDonationCard();

        LeftColumn.Children.Add(_systemCard!);
        LeftColumn.Children.Add(_processesCard!);
        LeftColumn.Children.Add(_componentsCard!);
        LeftColumn.Children.Add(_subsystemsCard!);
        LeftColumn.Children.Add(_donationCard!);
        RightColumn.Children.Add(_glassCard!);
        RightColumn.Children.Add(_dwmCard!);
        RightColumn.Children.Add(_dwmLocalCard!);
        RightColumn.Children.Add(_placeholdersCard!);
    }

    private void BuildPlatformSummary()
    {
        PlatformSummaryCard.CardTitle = Environment.MachineName;
        PlatformSummaryCard.CardDescription = HomeStatusSummary.DeviceHeaderBody();

        _platformWallpaperTile = new DesktopWallpaperPreviewTile();
        PlatformSummaryCard.SetLeadContent(_platformWallpaperTile);

        _summaryFooter = new HomeCustomizationFooterLine();
        _summaryFooter.CustomLinkClicked += (_, _) => ScrollToFirstCustomization();
        _summaryFooter.UnsavedLinkClicked += (_, _) => ScrollToFirstUnsaved();

        _saveFooterButton = new Button
        {
            Content = GlassSettingsResources.Get("CmdSave"),
            MinHeight = 28,
            Padding = new Thickness(12, 2, 12, 2),
            VerticalAlignment = VerticalAlignment.Center
        };
        if (Application.Current.Resources["GlassSettingsNoticeSaveButtonStyle"] is Style saveStyle)
            _saveFooterButton.Style = saveStyle;
        _saveFooterButton.Click += async (_, _) => await SaveAllAsync();
        _summaryFooter.TrailingContent = _saveFooterButton;

        PlatformSummaryCard.SetupRebootPrompt(
            GlassSettingsResources.Get("Home_FooterRebootRequired"),
            GlassSettingsResources.Get("Themes_SignatureRebootButton"),
            async (_, _) =>
            {
                try
                {
                    await Task.Run(() =>
                    {
                        using var process = new System.Diagnostics.Process();
                        process.StartInfo = new System.Diagnostics.ProcessStartInfo
                        {
                            FileName = "shutdown",
                            Arguments = "/r /t 0 /d p:0:0",
                            CreateNoWindow = true,
                            UseShellExecute = false
                        };
                        process.Start();
                    });
                }
                catch (Exception ex)
                {
                    System.Diagnostics.Debug.WriteLine($"GlassSettings Reboot failed: {ex}");
                }
            });

        _summaryFooter.HorizontalAlignment = HorizontalAlignment.Stretch;

        _platformSummaryFooter = _summaryFooter;
        PlatformSummaryCard.SetFooter(_summaryFooter);
    }

    private void UpdateRebootButtonVisibility()
    {
        var requiresRestart = SecureUxThemeService.Query().State is SecureUxThemeState.ActivationPendingRestart
            or SecureUxThemeState.DeactivationPendingRestart;
        // Кнопка видна только если требование перезагрузки актуально И жёлтое уведомление закрыто.
        var shouldShow = requiresRestart && HomeSessionNoticeState.RebootNoticeDismissed;
        PlatformSummaryCard.SetRebootPromptVisible(shouldShow);
    }

    private async Task RebootSystemFromHomePageAsync()
    {
        try
        {
            await Task.Run(() =>
            {
                using var process = new System.Diagnostics.Process();
                process.StartInfo = new System.Diagnostics.ProcessStartInfo
                {
                    FileName = "shutdown",
                    Arguments = "/r /t 0 /d p:0:0",
                    CreateNoWindow = true,
                    UseShellExecute = false
                };
                process.Start();
            });
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings Reboot failed: {ex}");
        }
    }

    private void BuildSystemCard()
    {
        _systemCard = new HomeSettingsCard { CardTitle = GlassSettingsResources.Get("Home_CardSystem") };

        _openGlassRow = AddNavRow(_systemCard, GlassSettingsResources.Get("Home_RowOpenGlass"), "\uEB9F",
            () => HomeStatusSummary.OpenGlassLine(),
            HomeNavTarget.Section("dwm"));
        _openGlassSwitch = new GlassSettingsLabeledToggleSwitch();
        _openGlassSwitch.Toggled += OpenGlassSwitch_OnToggled;
        _openGlassRow.TrailingContent = _openGlassSwitch;

        if (GlassPlatformNames.ShowDualGlassPlatformUi)
        {
            _glassPlatformDualRow = new HomeDualGlassPlatformRow();
            _glassPlatformDualRow.NavigateRequested += (_, _) => RaiseNav(HomeNavTarget.Section("glass"));
            _glassPlatformDualRow.GateToggledFinished += (_, _) =>
            {
                if (SettingsSession.Current.HasUnsavedChanges)
                    HomeSessionNoticeState.InvalidateDismissOnMutation();
                _ = RefreshNoticesAsync();
                ScheduleNoticeRefreshSettle();
            };
            _systemCard.AddCustomRow(_glassPlatformDualRow);
        }
        else
        {
            _glassPlatformRow = AddNavRow(_systemCard, GlassSettingsResources.Get("Home_RowGlassPlatform"), "\uE943",
                () => HomeStatusSummary.GlassPlatformLine(),
                HomeNavTarget.Section("glass"));
            _glassPlatformSwitch = new GlassSettingsLabeledToggleSwitch();
            _glassPlatformSwitch.Toggled += GlassPlatformSwitch_OnToggled;
            _glassPlatformRow.TrailingContent = _glassPlatformSwitch;
        }

        _shellSwitch = new GlassSettingsLabeledToggleSwitch();
        _shellSwitch.Toggled += ShellSwitch_OnToggled;
        _shellRow = _systemCard.AddRow();
        _shellRow.Title = GlassSettingsResources.Get("Home_RowShellExtension");
        _shellRow.Subtitle = GlassSettingsResources.Get("Home_RowShellExtensionSub");
        _shellRow.IconGlyphChar = "\uE8A7";
        _shellRow.ShowChevron = false;
        _shellRow.TrailingContent = _shellSwitch;

        var manageRow = _systemCard.AddRow();
        manageRow.Title = GlassSettingsResources.Get("Home_RowSystemManagement");
        manageRow.ShowDivider = false;
        manageRow.NavigateRequested += (_, _) => RaiseNav(HomeNavTarget.Section("system"));
        _systemCard.UpdateLastDivider();
    }

    private void BuildProcessesCard()
    {
        _processesCard = new HomeSettingsCard { CardTitle = GlassSettingsResources.Get("Home_CardProcesses") };

        var main = _processesCard.AddRow();
        main.Title = GlassSettingsResources.Get("Home_RowProcessList");
        main.IconGlyphChar = SettingsSectionGlyphs.Processes;
        main.ShowChevron = false;

        _runtimeSummaryGroup = _processesCard.AddComponentGroup();
        _runtimeSummaryGroup.SetRowsScrollable(false);
        _runtimeSummaryGroup.SetFooterVisible(false);
        ApplyRuntimeSnapshot(InjectRuntimeMonitor.Instance.Latest);

        var footer = _processesCard.AddRow();
        footer.Title = GlassSettingsResources.Get("Home_RowViewAllProcesses");
        footer.ShowDivider = false;
        footer.NavigateRequested += (_, _) => RaiseNav(HomeNavTarget.Section("attach"));
        _processesCard.UpdateLastDivider();
    }

    private void StartRuntimeMonitor()
    {
        if (_runtimeMonitorSubscribed)
        {
            // Уже подписаны (повторный OnNavigatedTo) — синхронизировать карточку с Latest.
            ApplyRuntimeSnapshot(InjectRuntimeMonitor.Instance.Latest);
            return;
        }
        _runtimeMonitorSubscribed = true;
        InjectRuntimeMonitor.Instance.SnapshotChanged += OnRuntimeSnapshotChanged;
        InjectRuntimeMonitor.Instance.Subscribe(this, DispatcherQueue);
        // Всегда красим Latest (в т.ч. 0 после stop) — иначе карточка залипает на старых Active.
        ApplyRuntimeSnapshot(InjectRuntimeMonitor.Instance.Latest);
    }

    private void StopRuntimeMonitor()
    {
        if (!_runtimeMonitorSubscribed)
            return;
        _runtimeMonitorSubscribed = false;
        InjectRuntimeMonitor.Instance.SnapshotChanged -= OnRuntimeSnapshotChanged;
        InjectRuntimeMonitor.Instance.Unsubscribe(this);
    }

    private void OnRuntimeSnapshotChanged(InjectRuntimeSnapshot snapshot) =>
        DispatcherQueue.TryEnqueue(() => ApplyRuntimeSnapshot(snapshot));

    private void ApplyRuntimeSnapshot(InjectRuntimeSnapshot snapshot)
    {
        if (_runtimeSummaryGroup is not null)
        {
            var key = (snapshot.ActiveCount, snapshot.RejectedCount, snapshot.ResidualCount);
            if (_runtimeCountsKey != key)
            {
                _runtimeCountsKey = key;
                var culture = System.Globalization.CultureInfo.CurrentCulture;
                _runtimeSummaryGroup.SetTable(
                    GlassSettingsResources.Get("Home_TableStatus"),
                    GlassSettingsResources.Get("Home_TableCount"),
                    [
                        (GlassSettingsResources.Get("Home_RuntimeActive"), snapshot.ActiveCount.ToString(culture)),
                        (GlassSettingsResources.Get("Home_RuntimeRejected"), snapshot.RejectedCount.ToString(culture)),
                        (GlassSettingsResources.Get("Home_RuntimeResidual"), snapshot.ResidualCount.ToString(culture)),
                    ]);
                _runtimeSummaryGroup.SetFooterVisible(false);
            }
        }

        // Residual/Stopping после stop вне GS — тумблеры Главной как на Системе (per-arch SoT).
        RefreshSystemCard();
    }

    private void BuildGlassCard()
    {
        _glassCard = new HomeSettingsCard { CardTitle = GlassSettingsResources.Get("Home_CardGlassPlatform") };

        AddCustomizationRow(_glassCard, GlassSettingsResources.Get("Home_RowRootWindows"), "\uE7F4",
            HomeCustomizationSummary.Root,
            SettingsCommitScope.ForArea(CustomizationAreaId.Root),
            HomeNavTarget.RootWindows());

        AddCustomizationRow(_glassCard, GlassSettingsResources.Get("Home_RowPopupWindows"), "\uE8A7",
            HomeCustomizationSummary.Popup,
            SettingsCommitScope.ForArea(CustomizationAreaId.Popup),
            HomeNavTarget.GlassPage(typeof(PopupHubPage), null, GlassSettingsResources.Get("Breadcrumb_PopupWindows")));

        AddCustomizationRow(_glassCard, GlassSettingsResources.Get("Home_RowTaskbar"), "\uE7C3",
            HomeCustomizationSummary.Taskbar,
            SettingsCommitScope.ForArea(CustomizationAreaId.Taskbar),
            HomeNavTarget.GlassPage(typeof(TaskbarFlyoutPage), null, GlassSettingsResources.Get("Breadcrumb_Taskbar")));

        _glassCard.UpdateLastDivider();
    }

    private void BuildDwmCards()
    {
        _dwmCard = new HomeSettingsCard { CardTitle = GlassSettingsResources.Get("Home_CardDwm") };
        BuildDwmCardRows(_dwmCard, DwmRegistryHive.Hkcu);

        _dwmLocalCard = new HomeSettingsCard { CardTitle = GlassSettingsResources.Get("Home_CardDwmLocal") };
        BuildDwmCardRows(_dwmLocalCard, DwmRegistryHive.Hklm);
    }

    private void BuildDwmCardRows(HomeSettingsCard card, DwmRegistryHive hive)
    {
        AddCustomizationRow(card, GlassSettingsResources.Get("Home_RowColorization"), "\uE790",
            () => HomeCustomizationSummary.Colorization(hive),
            SettingsCommitScope.ForDwmHiveArea(CustomizationAreaId.DwmColorization, hive),
            HomeNavTarget.DwmPage(
                typeof(DwmColorizationDetailPage),
                hive,
                DwmRouteIds.Colorization,
                GlassSettingsResources.Get("Breadcrumb_Colorization")));

        AddCustomizationRow(card, GlassSettingsResources.Get("Home_RowDwmGlass"), "\uE8B9",
            () => HomeCustomizationSummary.DwmGlass(hive),
            SettingsCommitScope.ForDwmHiveArea(CustomizationAreaId.DwmGlass, hive),
            HomeNavTarget.DwmPage(
                typeof(DwmGlassDetailPage),
                hive,
                DwmRouteIds.Glass,
                GlassSettingsResources.Get("Breadcrumb_DwmGlass")));

        AddCustomizationRow(card, GlassSettingsResources.Get("Home_RowDwmTheme"), "\uE8B3",
            () => HomeCustomizationSummary.DwmTheme(hive),
            SettingsCommitScope.ForDwmHiveArea(CustomizationAreaId.DwmTheme, hive),
            HomeNavTarget.DwmPage(
                typeof(DwmThemeDetailPage),
                hive,
                DwmRouteIds.Theme,
                GlassSettingsResources.Get("Breadcrumb_DwmTheme")));

        card.UpdateLastDivider();
    }

    private void BuildComponentsCard()
    {
        _componentsCard = new HomeSettingsCard { CardTitle = GlassSettingsResources.Get("Home_CardComponents") };
        _platformComponentGroup = _componentsCard.AddComponentGroup();
        _platformComponentGroup.SetRowsScrollable(true);
        RefreshComponentsCard();
        _componentsCard.UpdateLastDivider();
    }

    private void BuildSubsystemsCard()
    {
        _subsystemsCard = new HomeSettingsCard { CardTitle = GlassSettingsResources.Get("Home_CardSubsystems") };
        _openGlassComponentGroup = _subsystemsCard.AddComponentGroup();
        _openGlassComponentGroup.SetRowsScrollable(false);
        RefreshSubsystemsCard();
        _subsystemsCard.UpdateLastDivider();
    }

    private void BuildDonationCard()
    {
        _donationCard = new HomeSettingsCard
        {
            CardTitle = GlassSettingsResources.Get("Home_CardDonation"),
            CardDescription = GlassSettingsResources.Get("Home_CardDonationDesc")
        };

        var row = _donationCard.AddRow();
        row.Title = GlassSettingsResources.Get("Home_RowDonate");
        row.Subtitle = GlassSettingsResources.Get("Home_RowDonateSub");
        row.IconGlyphChar = "\uEB52";
        row.NavigateRequested += async (_, _) =>
        {
            if (XamlRoot is null)
                return;
            await DonationDialogService.ShowAsync(XamlRoot);
        };
        _donationCard.UpdateLastDivider();
    }

    private void BuildPlaceholdersCard()
    {
        _placeholdersCard = new HomeSettingsCard { CardTitle = GlassSettingsResources.Get("Home_CardExtra") };

        var themes = _placeholdersCard.AddRow();
        _themesRow = themes;
        themes.Title = GlassSettingsResources.Get("Home_RowVisualStyles");
        themes.Subtitle = VisualStyleCatalog.HomeLine();
        themes.IconGlyphChar = "\uE790";
        themes.NavigateRequested += (_, _) => RaiseNav(HomeNavTarget.Section("themes"));

        AddCustomizationRow(
            _placeholdersCard,
            GlassSettingsResources.Get("Home_RowTextFonts"),
            "\uE8D2",
            HomeCustomizationSummary.Text,
            SettingsCommitScope.ForArea(CustomizationAreaId.Text),
            HomeNavTarget.Section("text"));
        _placeholdersCard.UpdateLastDivider();
    }

    private HomeSettingsRow AddNavRow(
        HomeSettingsCard card,
        string title,
        string glyph,
        Func<string> subtitle,
        HomeNavTarget target)
    {
        var row = card.AddRow();
        row.Title = title;
        row.IconGlyphChar = glyph;
        row.Subtitle = subtitle();
        row.NavigateRequested += (_, _) => RaiseNav(target);
        return row;
    }

    private void AddCustomizationRow(
        HomeSettingsCard card,
        string title,
        string glyph,
        Func<HomeRowStatus> statusFactory,
        SettingsCommitScope scope,
        HomeNavTarget target)
    {
        var row = card.AddRow();
        row.Title = title;
        row.IconGlyphChar = glyph;
        ApplyRowStatus(row, statusFactory());
        row.NavigateRequested += (_, _) => RaiseNav(target);
        row.UnsavedScope = scope;
        _customizationRows.Add((row, statusFactory, scope));
    }

    private static void ApplyRowStatus(HomeSettingsRow row, HomeRowStatus status)
    {
        row.ApplyStatus(status.Subtitle, status.AccentValue, status.HasCustomizations);
        row.IsCustomizationHighlighted = status.HasCustomizations;
    }

    /// <summary>Точки несохранённости на строках кастомизации (Glass/DWM) без полного Refresh карточек.</summary>
    internal void RefreshCustomizationUnsavedIndicators()
    {
        if (!_built)
            return;

        if (_glassCard is not null)
            SettingsUnsavedIndicatorRefresh.Refresh(_glassCard);
        if (_dwmCard is not null && _dwmCard.Visibility == Visibility.Visible)
            SettingsUnsavedIndicatorRefresh.Refresh(_dwmCard);
        if (_dwmLocalCard is not null && _dwmLocalCard.Visibility == Visibility.Visible)
            SettingsUnsavedIndicatorRefresh.Refresh(_dwmLocalCard);
    }

    public void Refresh(bool refreshNotices = true)
    {
        if (!_built || SettingsSession.Current.IsShuttingDown)
            return;

        PlatformSummaryCard.CardDescription = HomeStatusSummary.DeviceHeaderBody();
        _platformWallpaperTile?.RefreshPreview();
        // После Capture на dismiss notices покажет MarkReady/ShowStartupNotice — не дублировать present.
        if (refreshNotices)
            _ = RefreshNoticesAsync();
        RefreshPlatformSummary();

        RefreshSystemCard();
        RefreshProcessesCard();
        RefreshGlassCard();
        RefreshDwmCard();
        RefreshComponentsCard();
        RefreshSubsystemsCard();
        RefreshPlaceholdersCard();
        ApplyOpenGlassUiVisibility();
    }

    /// <summary>Показывать ли левую часть футера (кастомизация) — после закрытия зелёного notice.</summary>
    private static bool ShouldShowFooterLeft(int customCount) =>
        HomeSessionNoticeState.FooterLeftRevealed && customCount > 0;

    /// <summary>Показывать ли правую часть футера (несохранённые) — после закрытия жёлтого notice.</summary>
    private static bool ShouldShowFooterRight(bool hasUnsaved) =>
        HomeSessionNoticeState.FooterRightRevealed && hasUnsaved;

    internal void SyncSummaryFooterFromSession()
    {
        if (!_built)
            return;

        HomeSessionNoticeState.EnsureFooterLeftAfterStartupLifecycle();
        ApplySummaryFooterContent();
        SyncSummaryFooterVisibility();
        RefreshFooterSaveButton(SettingsSession.Current.HasUnsavedChanges);
    }

    private void RefreshPlatformSummary() =>
        SyncSummaryFooterFromSession();

    private void ApplySummaryFooterContent()
    {
        if (_summaryFooter is null)
            return;

        var hasUnsaved = SettingsSession.Current.HasUnsavedChanges;
        var customCount = CustomizationTracker.CountCustomizedAreas(CustomizationCatalog.HomeSummaryAreas);

        var showLeft = ShouldShowFooterLeft(customCount);
        var showRight = ShouldShowFooterRight(hasUnsaved);

        var (_, leftPrefix, leftLink, _, _) = HomeCustomizationSummary.DeviceFooter(hasUnsavedChanges: false);
        var (_, rightPrefix, rightLink, rightPlain, _) = HomeCustomizationSummary.DeviceFooter(hasUnsavedChanges: true);

        if (string.IsNullOrEmpty(rightLink) && string.IsNullOrEmpty(rightPlain))
            rightPlain = GlassSettingsResources.Get("Home_FooterUnsavedPlain");

        _summaryFooter.SetParts(
            showLeft, leftPrefix, leftLink,
            showRight, rightPrefix, rightLink, rightPlain);
    }

    private bool ShouldShowSummaryFooter()
    {
        if (_saveInProgress)
            return false;

        var hasUnsaved = SettingsSession.Current.HasUnsavedChanges;
        var customCount = CustomizationTracker.CountCustomizedAreas(CustomizationCatalog.HomeSummaryAreas);
        return ShouldShowFooterLeft(customCount) || ShouldShowFooterRight(hasUnsaved);
    }

    private void SyncSummaryFooterVisibility()
    {
        if (_platformSummaryFooter is null)
            return;

        var wantVisible = ShouldShowSummaryFooter();
        var isVisible = _platformSummaryFooter.Visibility == Visibility.Visible;

        if (wantVisible == isVisible)
        {
            PlatformSummaryCard.RefreshLeadChrome();
            return;
        }

        if (!wantVisible)
        {
            _footerRevealCts?.Cancel();
            _platformSummaryFooter.Visibility = Visibility.Collapsed;
            _platformSummaryFooter.ClearValue(FrameworkElement.MaxHeightProperty);
            _platformSummaryFooter.Opacity = 1;
            PlatformSummaryCard.RefreshLeadChrome();
            return;
        }

        // Первая строка в футере — раскрытие по высоте после скрытия green.
        _ = RevealSummaryFooterAnimatedAsync();
    }

    private async Task RevealSummaryFooterAnimatedAsync()
    {
        if (_platformSummaryFooter is null)
            return;

        _footerRevealCts?.Cancel();
        _footerRevealCts = new CancellationTokenSource();
        var ct = _footerRevealCts.Token;
        var footer = _platformSummaryFooter;
        var scroll = PageShell.PageScrollViewer;

        // Off-screen (карточка устройства вне кадра): мгновенный reveal + scroll-compensate.
        // Иначе анимация MaxHeight + Reposition даёт двойной дёрганье без ChangeView.
        if (NoticeViewportMirrorService.TryRevealHomeFooterOffScreenPreservingScroll(
                scroll,
                PlatformSummaryCard,
                () =>
                {
                    footer.Visibility = Visibility.Visible;
                    footer.Opacity = 1;
                    footer.ClearValue(FrameworkElement.MaxHeightProperty);
                    PlatformSummaryCard.RefreshLeadChrome();
                }))
        {
            return;
        }

        // On-screen: снять Reposition + не делать «−12 MainPanel / +footer» двумя скачками.
        // Сразу MaxHeight ≥ прежнего нижнего padding (12), затем анимация до target.
        const double mainPanelBottomPad = 12;
        TransitionCollection? savedCardTransitions = null;
        TransitionCollection? savedRootTransitions = null;
        if (PlatformSummaryCard.CardRootPanel is { } cardRoot)
        {
            savedCardTransitions = cardRoot.ChildrenTransitions;
            cardRoot.ChildrenTransitions = null;
        }

        if (RootStack.ChildrenTransitions is not null)
        {
            savedRootTransitions = RootStack.ChildrenTransitions;
            RootStack.ChildrenTransitions = null;
        }

        try
        {
            footer.MaxHeight = 0;
            footer.Visibility = Visibility.Visible;
            footer.Opacity = 1;
            PlatformSummaryCard.RefreshLeadChrome(); // MainPanel bottom 12→0

            var target = MeasureSummaryFooterHeight(footer);

            if (target <= 0.5 || !ExpanderBodyTransition.ShouldAnimate())
            {
                footer.ClearValue(FrameworkElement.MaxHeightProperty);
                PlatformSummaryCard.RefreshLeadChrome();
                return;
            }

            // Заместить снятый padding мгновенно — один layout-step вместо двух дёрганий.
            var startH = Math.Min(mainPanelBottomPad, target);
            footer.MaxHeight = startH;
            PlatformSummaryCard.UpdateLayout();

            if (target <= startH + 0.5)
            {
                footer.ClearValue(FrameworkElement.MaxHeightProperty);
                PlatformSummaryCard.RefreshLeadChrome();
                return;
            }

            var started = Environment.TickCount64;
            try
            {
                while (Environment.TickCount64 - started < FooterRevealMs)
                {
                    ct.ThrowIfCancellationRequested();
                    var p = (Environment.TickCount64 - started) / (double)FooterRevealMs;
                    var eased = 1.0 - Math.Pow(1.0 - Math.Clamp(p, 0, 1), 3);
                    footer.MaxHeight = startH + (target - startH) * eased;
                    await Task.Delay(16, ct).ConfigureAwait(true);
                }

                footer.ClearValue(FrameworkElement.MaxHeightProperty);
                PlatformSummaryCard.RefreshLeadChrome();
            }
            catch (OperationCanceledException)
            {
                /* superseded */
            }
        }
        finally
        {
            if (PlatformSummaryCard.CardRootPanel is { } restoreCard)
                restoreCard.ChildrenTransitions = savedCardTransitions;
            RootStack.ChildrenTransitions = savedRootTransitions;
        }
    }

    private static double MeasureSummaryFooterHeight(FrameworkElement footer)
    {
        var width = footer.ActualWidth;
        if (width <= 0 && footer.Parent is FrameworkElement parent)
            width = parent.ActualWidth;
        if (width <= 0)
            width = 400;

        footer.ClearValue(FrameworkElement.MaxHeightProperty);
        footer.Measure(new Size(width, double.PositiveInfinity));
        var h = footer.DesiredSize.Height;
        if (h > 0.5)
            return h;

        // divider (1+12) + row min (52) + top pad (12); bottom pad 0
        return 1 + 12 + 52 + 12;
    }

    private void RefreshFooterSaveButton(bool hasUnsaved)
    {
        if (_saveFooterButton is null)
            return;

        var showFooterSave = !_saveInProgress && ShouldShowFooterRight(hasUnsaved);
        _saveFooterButton.Visibility = showFooterSave ? Visibility.Visible : Visibility.Collapsed;
        _saveFooterButton.IsEnabled = !_saveInProgress;
    }

    private async Task SaveAllAsync()
    {
        if (_saveInProgress)
            return;

        _saveInProgress = true;
        UnsavedNoticeStrip.IsSaveEnabled = false;
        NoticeViewportMirrorService.SyncSaveEnabled(false);
        RefreshFooterSaveButton(SettingsSession.Current.HasUnsavedChanges);
        await SettingsNoticeCoordinator.RetractOnSaveAsync(
            NoticeHost,
            UnsavedNoticeStrip,
            SettingsNoticeCoordinator.NoticeSlotColor.Yellow).ConfigureAwait(true);
        try
        {
            await SettingsSession.Current.ApplyCommitAsync().ConfigureAwait(true);

            // Сохранили: сброс жёлтого + скрытие правой строки футера.
            HomeSessionNoticeState.OnUnsavedChanged(false);

            SyncSummaryFooterVisibility();
            RefreshPlatformSummary();
            RefreshSystemCard();
            RefreshProcessesCard();
            RefreshGlassCard();
            RefreshDwmCard();
            RefreshComponentsCard();
            RefreshPlaceholdersCard();
        }
        finally
        {
            _saveInProgress = false;
            _ = RefreshNoticesAsync();
            RefreshFooterSaveButton(SettingsSession.Current.HasUnsavedChanges);
        }
    }

    private void ScrollToFirstCustomization() =>
        ScrollToFirstMatchingCard(CardHasCustomizations);

    private void ScrollToFirstUnsaved() =>
        ScrollToFirstMatchingCard(CardHasUnsaved);

    private void ScrollToFirstMatchingCard(Func<HomeSettingsCard?, bool> predicate)
    {
        var scroll = PageShell.PageScrollViewer;
        if (scroll.Content is not UIElement contentRoot)
            return;

        HomeSettingsCard? targetCard = null;
        var minY = double.MaxValue;

        foreach (var card in EnumerateScrollableHomeCards())
        {
            if (!predicate(card))
                continue;

            var y = GetTopInScrollContent(card, contentRoot);
            if (y >= minY)
                continue;

            minY = y;
            targetCard = card;
        }

        if (targetCard is null || minY == double.MaxValue)
            return;

        var offset = Math.Max(0, minY - 12);
        scroll.ChangeView(null, offset, null, disableAnimation: false);
    }

    private IEnumerable<HomeSettingsCard> EnumerateScrollableHomeCards()
    {
        if (_systemCard is not null)
            yield return _systemCard;
        if (_processesCard is not null)
            yield return _processesCard;
        if (_glassCard is not null)
            yield return _glassCard;
        if (_dwmCard is not null && _dwmCard.Visibility == Visibility.Visible)
            yield return _dwmCard;
        if (_dwmLocalCard is not null && _dwmLocalCard.Visibility == Visibility.Visible)
            yield return _dwmLocalCard;
    }

    private bool CardHasCustomizations(HomeSettingsCard? card)
    {
        if (card is null)
            return false;

        if (ReferenceEquals(card, _systemCard) || ReferenceEquals(card, _processesCard))
            return false;

        foreach (var (row, statusFactory, _) in _customizationRows)
        {
            if (!IsRowInCard(row, card))
                continue;

            if (statusFactory().HasCustomizations)
                return true;
        }

        return false;
    }

    private bool CardHasUnsaved(HomeSettingsCard? card)
    {
        if (card is null)
            return false;

        if (ReferenceEquals(card, _systemCard))
            return false;

        if (ReferenceEquals(card, _processesCard))
            return false;

        foreach (var (row, _, scope) in _customizationRows)
        {
            if (!IsRowInCard(row, card))
                continue;

            if (SettingsSession.Current.HasUnsavedInScope(scope))
                return true;
        }

        return false;
    }

    private static bool IsRowInCard(HomeSettingsRow row, HomeSettingsCard card)
    {
        var node = row as DependencyObject;
        while (node is not null)
        {
            if (ReferenceEquals(node, card))
                return true;

            node = VisualTreeHelper.GetParent(node);
        }

        return false;
    }

    private static double GetTopInScrollContent(FrameworkElement element, UIElement contentRoot)
    {
        try
        {
            var transform = element.TransformToVisual(contentRoot);
            return transform.TransformPoint(new Point(0, 0)).Y;
        }
        catch
        {
            return double.MaxValue;
        }
    }

    private bool _shellSuppress;
    private bool _openGlassSuppress;
    private bool _glassPlatformSuppress;
    private bool _openGlassBusy;
    private bool _glassPlatformBusy;
    private DateTime _lastRefreshSystemCardUtc = DateTime.MinValue;

    private void RefreshSystemCard()
    {
        if (_systemCard is null) return;

        if ((DateTime.UtcNow - _lastRefreshSystemCardUtc).TotalMilliseconds < 2000)
            return;
        _lastRefreshSystemCardUtc = DateTime.UtcNow;

        var ogLine = HomeStatusSummary.OpenGlassLine();
        var ogColor = HomeStatusSummary.OpenGlassStatusColor();
        var presence = PlatformPresenceService.Instance.CurrentSnapshot;
        bool ogTaskExists;
        bool ogRunning;
        if (presence.Revision > 0)
        {
            ogTaskExists = presence.OgLegacyTaskExists != 0 || presence.OgCurrentTaskExists != 0;
            ogRunning = presence.OpenGlassOrchestration
                == (int)NativeHostInterop.NativeOpenGlassHostUiState.Running;
        }
        else
        {
            ogTaskExists = OpenGlassHostService.LegacyHostTaskExists();
            ogRunning = OpenGlassHostService.QueryOpenGlassHostUiState() == OpenGlassHostUiState.Running;
        }

        HomeStatusSummary.GlassPlatformUiSnapshot? singleUi = null;
        HomeStatusSummary.GlassPlatformUiSnapshot? dualX86 = null;
        HomeStatusSummary.GlassPlatformUiSnapshot? dualX64 = null;
        if (_glassPlatformDualRow is not null)
        {
            dualX86 = HomeStatusSummary.QueryGlassPlatformUi(GlassPlatformArch.X86);
            dualX64 = HomeStatusSummary.QueryGlassPlatformUi(GlassPlatformArch.X64);
        }
        else if (_glassPlatformRow is not null)
        {
            // Один snapshot: иначе subtitle «Выгружается» при уже разблокированном тумблере.
            singleUi = HomeStatusSummary.QueryGlassPlatformUi(GlassPlatformNames.NativeArch);
        }

        var shellInProgress = ShellExtensionService.IsShellExtensionOperationInProgress();
        var shellTaskExists = false;
        var shellRegistered = false;
        if (!shellInProgress)
        {
            shellTaskExists = GlassPlatformNames.ShowDualGlassPlatformUi
                ? GlassPlatformService.GlassAutorunTaskExistsForUi(GlassPlatformArch.X86)
                  || GlassPlatformService.GlassAutorunTaskExistsForUi(GlassPlatformArch.X64)
                : GlassPlatformService.GlassAutorunTaskExistsForUi(GlassPlatformNames.NativeArch);
            shellRegistered = ShellExtensionService.IsBundledDllRegistered();
        }

        var fingerprint = BuildSystemCardFingerprint(
            ogLine,
            ogColor,
            ogTaskExists,
            ogRunning,
            singleUi,
            dualX86,
            dualX64,
            shellInProgress,
            shellTaskExists,
            shellRegistered,
            _glassPlatformDualRow is not null
                && GlassPlatformUiPolicy.IsX86DualUiControlsLocked());
        if (string.Equals(_systemCardFingerprint, fingerprint, StringComparison.Ordinal))
        {
            return;
        }

        _systemCardFingerprint = fingerprint;

        if (_openGlassRow is not null)
        {
            if (!string.Equals(_openGlassRow.Subtitle, ogLine, StringComparison.Ordinal))
                _openGlassRow.Subtitle = ogLine;
            if (_openGlassRow.StatusIndicatorColor != ogColor)
                _openGlassRow.StatusIndicatorColor = ogColor;
        }

        if (_glassPlatformDualRow is not null)
            _glassPlatformDualRow.Refresh();

        RefreshPlatformToggle(
            _openGlassSwitch,
            ref _openGlassSuppress,
            _openGlassBusy,
            ogTaskExists,
            ogRunning);

        if (_glassPlatformDualRow is null && _glassPlatformRow is not null && singleUi is { } ui)
        {
            if (!string.Equals(_glassPlatformRow.Subtitle, ui.Line, StringComparison.Ordinal))
                _glassPlatformRow.Subtitle = ui.Line;
            if (_glassPlatformRow.StatusIndicatorColor != ui.DotColor)
                _glassPlatformRow.StatusIndicatorColor = ui.DotColor;
            var startInFlight = GlassPlatformService.IsStartInFlight(GlassPlatformNames.NativeArch, ui.Life);
            RefreshPlatformToggle(
                _glassPlatformSwitch,
                ref _glassPlatformSuppress,
                _glassPlatformBusy || ui.LifecycleBusy || startInFlight,
                ui.Autorun != GlassAutorunUiState.NoTask || startInFlight,
                startInFlight || ui.Autorun == GlassAutorunUiState.Running);
        }

        if (_shellSwitch is not null)
        {
            if (shellInProgress)
            {
                if (_shellSwitch.IsEnabled)
                    _shellSwitch.IsEnabled = false;
            }
            else
            {
                var enabled = shellTaskExists || shellRegistered;
                _shellSuppress = true;
                try
                {
                    if (_shellSwitch.IsEnabled != enabled)
                        _shellSwitch.IsEnabled = enabled;
                    if (_shellSwitch.IsOn != shellRegistered)
                        _shellSwitch.IsOn = shellRegistered;
                }
                finally
                {
                    _shellSuppress = false;
                }
            }
        }

        if (_shellRow is not null)
            _shellRow.IsUnsavedPending = false;
    }

    private string BuildSystemCardFingerprint(
        string ogLine,
        Color ogColor,
        bool ogTaskExists,
        bool ogRunning,
        HomeStatusSummary.GlassPlatformUiSnapshot? singleUi,
        HomeStatusSummary.GlassPlatformUiSnapshot? dualX86,
        HomeStatusSummary.GlassPlatformUiSnapshot? dualX64,
        bool shellInProgress,
        bool shellTaskExists,
        bool shellRegistered,
        bool x86Locked)
    {
        var sb = new System.Text.StringBuilder(192);
        sb.Append(ogLine).Append('|').Append(PackColor(ogColor)).Append('|')
            .Append(ogTaskExists ? '1' : '0').Append(ogRunning ? '1' : '0')
            .Append(_openGlassBusy ? '1' : '0').Append('|');

        if (dualX86 is { } x86 && dualX64 is { } x64)
        {
            AppendPlatformUi(sb, x86);
            sb.Append('|');
            AppendPlatformUi(sb, x64);
        }
        else if (singleUi is { } ui)
        {
            AppendPlatformUi(sb, ui);
            sb.Append('|').Append(_glassPlatformBusy ? '1' : '0');
        }

        sb.Append('|').Append(shellInProgress ? '1' : '0')
            .Append(shellTaskExists ? '1' : '0')
            .Append(shellRegistered ? '1' : '0')
            .Append(x86Locked ? '1' : '0');
        return sb.ToString();
    }

    private static void AppendPlatformUi(System.Text.StringBuilder sb, HomeStatusSummary.GlassPlatformUiSnapshot ui)
    {
        sb.Append((int)ui.Life).Append('|')
            .Append((int)ui.Autorun).Append('|')
            .Append(ui.IsUnloading ? '1' : '0')
            .Append(ui.LifecycleBusy ? '1' : '0').Append('|')
            .Append(ui.Line).Append('|')
            .Append(PackColor(ui.DotColor));
    }

    private static uint PackColor(Color c) =>
        ((uint)c.A << 24) | ((uint)c.R << 16) | ((uint)c.G << 8) | c.B;

    private static void RefreshPlatformToggle(
        GlassSettingsLabeledToggleSwitch? toggle,
        ref bool suppress,
        bool busy,
        bool taskExists,
        bool isRunning)
    {
        if (toggle is null)
            return;

        var enabled = taskExists && !busy;
        if (toggle.IsEnabled == enabled && toggle.IsOn == isRunning)
            return;

        suppress = true;
        try
        {
            if (toggle.IsEnabled != enabled)
                toggle.IsEnabled = enabled;
            if (toggle.IsOn != isRunning)
                toggle.IsOn = isRunning;
        }
        finally
        {
            suppress = false;
        }
    }

    private static bool IsInjectLifecycleBusy(GlassPlatformArch arch) =>
        NativeHostInterop.QueryInjectLifecycleUi(arch) is NativeHostInterop.InjectLifecycleUi.Stopping
            or NativeHostInterop.InjectLifecycleUi.Residual
            or NativeHostInterop.InjectLifecycleUi.Starting;

    private async void OpenGlassSwitch_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_openGlassSwitch is null || _openGlassSuppress || _openGlassBusy)
            return;

        var want = _openGlassSwitch.IsOn;
        var st = OpenGlassHostService.QueryOpenGlassHostUiState();

        if (!OpenGlassHostService.LegacyHostTaskExists())
        {
            _openGlassSuppress = true;
            _openGlassSwitch.IsOn = st == OpenGlassHostUiState.Running;
            _openGlassSuppress = false;
            return;
        }

        if (want)
        {
            if (st == OpenGlassHostUiState.Running)
                return;
            if (st != OpenGlassHostUiState.Ready)
            {
                _openGlassSuppress = true;
                _openGlassSwitch.IsOn = false;
                _openGlassSuppress = false;
                return;
            }
        }
        else if (st != OpenGlassHostUiState.Running)
        {
            _openGlassSuppress = true;
            _openGlassSwitch.IsOn = false;
            _openGlassSuppress = false;
            return;
        }

        _openGlassBusy = true;
        _openGlassSwitch.IsEnabled = false;
        try
        {
            if (want)
            {
                if (!await SystemPlatformRunActions.StartOpenGlassAsync().ConfigureAwait(true))
                {
                    _openGlassSuppress = true;
                    _openGlassSwitch.IsOn = false;
                    _openGlassSuppress = false;
                }
            }
            else
            {
                await SystemPlatformRunActions.StopOpenGlassAsync().ConfigureAwait(true);
            }
        }
        finally
        {
            _openGlassBusy = false;
            RefreshSystemCard();
            if (SettingsSession.Current.HasUnsavedChanges)
                HomeSessionNoticeState.InvalidateDismissOnMutation();
            NoticeMirrorTrace.WriteHome("OpenGlassSwitch_OnToggled: finally — RefreshNotices");
            _ = RefreshNoticesAsync();
            ScheduleNoticeRefreshSettle();
        }
    }

    private async void GlassPlatformSwitch_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_glassPlatformSwitch is null || _glassPlatformSuppress || _glassPlatformBusy)
            return;

        var want = _glassPlatformSwitch.IsOn;
        var st = GlassPlatformService.QueryGlassAutorunUiState();

        if (!GlassPlatformService.GlassAutorunTaskExists())
        {
            _glassPlatformSuppress = true;
            _glassPlatformSwitch.IsOn = st == GlassAutorunUiState.Running;
            _glassPlatformSuppress = false;
            return;
        }

        // SoT как PlatformSchedulerRow / dual Home: Stopping/Residual — не Start/Stop.
        if (IsInjectLifecycleBusy(GlassPlatformNames.NativeArch))
        {
            _glassPlatformSuppress = true;
            try
            {
                _glassPlatformSwitch.IsOn = st == GlassAutorunUiState.Running;
                _glassPlatformSwitch.IsEnabled = false;
            }
            finally
            {
                _glassPlatformSuppress = false;
            }

            return;
        }

        if (want)
        {
            if (st == GlassAutorunUiState.Running)
                return;
            if (st != GlassAutorunUiState.Ready)
            {
                _glassPlatformSuppress = true;
                _glassPlatformSwitch.IsOn = false;
                _glassPlatformSuppress = false;
                return;
            }
        }
        else if (st != GlassAutorunUiState.Running)
        {
            _glassPlatformSuppress = true;
            _glassPlatformSwitch.IsOn = false;
            _glassPlatformSuppress = false;
            return;
        }

        _glassPlatformBusy = true;
        _glassPlatformSwitch.IsEnabled = false;
        if (!want)
        {
            // Stop: immediately show OFF — don't wait for stop to complete.
            _glassPlatformSuppress = true;
            _glassPlatformSwitch.IsOn = false;
            _glassPlatformSuppress = false;
        }
        try
        {
            if (want)
            {
                if (!await SystemPlatformRunActions.StartGlassAsync().ConfigureAwait(true))
                {
                    _glassPlatformSuppress = true;
                    _glassPlatformSwitch.IsOn = false;
                    _glassPlatformSuppress = false;
                }
            }
            else
            {
                await SystemPlatformRunActions.StopGlassAsync().ConfigureAwait(true);
            }
        }
        finally
        {
            _glassPlatformBusy = false;
            RefreshSystemCard();
            if (SettingsSession.Current.HasUnsavedChanges)
                HomeSessionNoticeState.InvalidateDismissOnMutation();
            _ = RefreshNoticesAsync();
            ScheduleNoticeRefreshSettle();
        }
    }

    private void RefreshProcessesCard()
    {
        if (_processesCard is null) return;
        var main = _processesCard.EnumerateRows().FirstOrDefault();
        if (main is null) return;

        var stats = AttachListRegistry.GetStats();
        var summary = AttachListRegistry.SummarizeLine();
        var detail = AttachListRegistry.DetailLine();
        var subtitle = string.IsNullOrEmpty(detail) ? summary : $"{summary} · {detail}";
        main.ApplyStatus(subtitle, accentValue: null, highlighted: stats.Total > 0);
        main.IsUnsavedPending = false;
    }

    private void RefreshGlassCard()
    {
        RefreshCustomizationCard(_glassCard,
        [
            HomeCustomizationSummary.Root,
            HomeCustomizationSummary.Popup,
            HomeCustomizationSummary.Taskbar
        ]);
    }

    private void RefreshDwmCard()
    {
        if (!OpenGlassUiPolicy.IsFeatureExposedInUi())
            return;

        RefreshCustomizationCard(_dwmCard,
        [
            () => HomeCustomizationSummary.Colorization(DwmRegistryHive.Hkcu),
            () => HomeCustomizationSummary.DwmGlass(DwmRegistryHive.Hkcu),
            () => HomeCustomizationSummary.DwmTheme(DwmRegistryHive.Hkcu)
        ]);
        RefreshCustomizationCard(_dwmLocalCard,
        [
            () => HomeCustomizationSummary.Colorization(DwmRegistryHive.Hklm),
            () => HomeCustomizationSummary.DwmGlass(DwmRegistryHive.Hklm),
            () => HomeCustomizationSummary.DwmTheme(DwmRegistryHive.Hklm)
        ]);
    }

    private void RefreshCustomizationCard(HomeSettingsCard? card, Func<HomeRowStatus>[] factories)
    {
        if (card is null) return;

        var idx = 0;
        foreach (var row in card.EnumerateRows())
        {
            if (idx >= factories.Length)
                break;

            ApplyRowStatus(row, factories[idx]());
            idx++;
        }

        SettingsUnsavedIndicatorRefresh.Refresh(card);
    }

    private void RefreshComponentsCard() => RefreshPlatformComponentGroup();

    private void RefreshSubsystemsCard()
    {
        if (!OpenGlassUiPolicy.IsFeatureExposedInUi())
            return;

        RefreshOpenGlassComponentGroup();
    }

    private void RefreshPlatformComponentGroup()
    {
        if (_platformComponentGroup is null)
            return;

        var detailed = PlatformModuleVersions.GetAllDetailed();
        var rows = detailed
            .Select(item => (item.Name, item.FileVersion))
            .ToList();
        var product = PlatformModuleVersions.GetProductVersion();

        _platformComponentGroup.SetTable(
            GlassSettingsResources.Get("Home_TableModule"),
            GlassSettingsResources.Get("Home_TableVersion"),
            rows);
        _platformComponentGroup.SetProductFooter(
            string.Format(
                GlassSettingsResources.Get("Home_ComponentsProductVersion"),
                product),
            hasUpdate: false,
            linkText: null,
            onLinkClick: null);
    }

    private void RefreshOpenGlassComponentGroup()
    {
        if (_openGlassComponentGroup is null)
            return;

        var openGlassVersion = OpenGlassVersionService.TryGetInstalledVersion(out var version, out _)
            ? version ?? string.Empty
            : GlassSettingsResources.Get("Modules_NotFound");

        var pending = ProductUpdateCoordinator.GetPendingOpenGlass();
        var hasUpdate = pending is not null;

        _openGlassComponentGroup.SetTable(
            GlassSettingsResources.Get("Home_TableModule"),
            GlassSettingsResources.Get("Home_TableVersion"),
            new List<(string Name, string Value)> { ("OpenGlass", openGlassVersion) });
        _openGlassComponentGroup.SetProductFooter(
            string.Format(GlassSettingsResources.Get("Home_ComponentsProductVersion"), openGlassVersion),
            hasUpdate,
            hasUpdate ? GetOpenGlassUpdateLinkText(pending!.Version) : null,
            hasUpdate ? () => _ = ProductUpdateCoordinator.OpenPendingOpenGlassDownloadAsync() : null);
    }

    private static string? GetOpenGlassUpdateLinkText(string pendingVersion) =>
        string.Format(GlassSettingsResources.Get("Home_OpenGlassUpdateLinkFormat"), pendingVersion);

    private async Task RefreshNoticesAsync(bool forceAnimateGreen = false, bool animateUpdate = false)
    {
        if (_saveInProgress)
            return;

        // Не текущий «Обзор» — только состояние; без RefreshHome/ReattachLegacy (не ломать notice hub/detail).
        if (App.ShellWindow is { } shell && !shell.IsCurrentHomePage(this))
            return;

        var hasUnsaved = SettingsSession.Current.HasUnsavedChanges;
        NoticeMirrorTrace.WriteHome(
            $"RefreshNoticesAsync: hasUnsaved={hasUnsaved}, showGreen={HomeSessionNoticeState.ShouldShowGreen}, " +
            $"showYellow={HomeSessionNoticeState.ShouldShowYellow(hasUnsaved)}, startupPending={_startupNoticePending}");
        HomeSessionNoticeState.OnUnsavedChanged(hasUnsaved);

        // Стартовые green/yellow — после ShellContentReadyGate (notices.md); до MarkReady не презентуем.
        if (_startupNoticePending)
        {
            ScheduleStartupNoticeAfterShellReady();

            // Update OpenGlass можно показать до green; yellow/green — только после MarkReady.
            var showUpdateWhilePending = OpenGlassUiPolicy.IsFeatureExposedInUi()
                && ProductUpdateCoordinator.ShouldShowOpenGlassNotice
                && ProductUpdateCoordinator.GetPendingOpenGlass() is not null;
            if (!showUpdateWhilePending)
            {
                SyncSummaryFooterFromSession();
                return;
            }
        }

        var showGreen = HomeSessionNoticeState.ShouldShowGreen && !_startupNoticePending;
        var showYellow = HomeSessionNoticeState.ShouldShowYellow(hasUnsaved) && !_startupNoticePending;
        // Желтое уведомление о необходимости перезагрузки SecureUxTheme.
        var requiresRestart = SecureUxThemeService.Query().State is SecureUxThemeState.ActivationPendingRestart
            or SecureUxThemeState.DeactivationPendingRestart;

        // Сбросить dismiss только когда требование перезагрузки ОТМЕНЕНО (true→false).
        // При появлении требования (false→true) — dismiss НЕ сбрасывать, уведомление одноразовое.
        if (!requiresRestart)
            HomeSessionNoticeState.ResetRebootNoticeIfInactive(requiresRestart);

        // Если уведомление уже закрыто пользователем — не показывать повторно.
        var showRebootNotice = requiresRestart && !HomeSessionNoticeState.RebootNoticeDismissed;

        var showUpdate = false;
        string? updateMessage = null;
        string? updateActionText = null;

        if (OpenGlassUiPolicy.IsFeatureExposedInUi()
            && ProductUpdateCoordinator.ShouldShowOpenGlassNotice
            && ProductUpdateCoordinator.GetPendingOpenGlass() is { } pending)
        {
            showUpdate = true;
            OpenGlassVersionService.TryGetInstalledVersion(out var installed, out _);
            updateMessage = string.Format(
                GlassSettingsResources.Get("Home_OpenGlassUpdateNotice"),
                pending.Version,
                installed ?? "—");
            updateActionText = GlassSettingsResources.Get("System_OpenGlassUpdateAction");
            if (!animateUpdate)
                animateUpdate = !UpdateNoticeStrip.IsVisuallyOpen;
        }

        UnsavedNoticeStrip.IsSaveEnabled = !_saveInProgress;

        string? yellowMessage = showYellow ? BuildYellowNoticeMessage() : null;
        // Если требуется показать уведомление о перезагрузке — используем его текст.
        if (showRebootNotice)
            yellowMessage = GlassSettingsResources.Get("Home_FooterRebootRequired");

        await SettingsNoticeCoordinator.RefreshHomeAsync(new SettingsNoticeCoordinator.HomeNoticeRefreshRequest
        {
            Host = NoticeHost,
            GreenStrip = SessionNoticeStrip,
            YellowStrip = UnsavedNoticeStrip,
            UpdateStrip = UpdateNoticeStrip,
            SaveInProgress = _saveInProgress,
            ForceAnimateGreen = forceAnimateGreen,
            AnimateUpdate = animateUpdate,
            ShowGreen = showGreen,
            GreenMessage = showGreen ? BuildGreenNoticeMessage() : null,
            ShowYellow = showYellow || showRebootNotice,
            YellowMessage = yellowMessage,
            ShowUpdate = showUpdate,
            UpdateMessage = updateMessage,
            UpdateActionText = updateActionText,
            IsRebootNotice = showRebootNotice,
            RebootActionText = showRebootNotice
                ? GlassSettingsResources.Get("Themes_SignatureRebootButton")
                : null
        }).ConfigureAwait(true);

        _yellowRebootActive = showRebootNotice;
        UpdateRebootButtonVisibility();

        NoticeViewportMirrorService.SyncSaveEnabled(!_saveInProgress);
        SyncSummaryFooterFromSession();
        SyncHomeNoticeHostGap();
        // После present — догнать top-mirror (иначе после старта платформы зеркало может остаться скрытым).
        ScheduleLegacyMirrorSettle();
    }

    /// <summary>
    /// Пустой HomeNoticeHost — Collapsed, иначе лишний зазор до колонок
    /// (якорь зеркала вынесен overlay на ColumnsHost).
    /// </summary>
    private void SyncHomeNoticeHostGap()
    {
        var anyPinnedOpen = UnsavedNoticeStrip.IsVisuallyOpen || UpdateNoticeStrip.IsVisuallyOpen;
        HomeNoticeHost.Visibility = anyPinnedOpen ? Visibility.Visible : Visibility.Collapsed;
        HomeNoticeHost.ClearValue(FrameworkElement.MarginProperty);
    }

    private static string BuildGreenNoticeMessage()
    {
        var (clickable, prefix, linkText, _, _) =
            HomeCustomizationSummary.DeviceFooter(hasUnsavedChanges: false);

        return clickable && !string.IsNullOrEmpty(linkText)
            ? $"{prefix}{linkText}"
            : GlassSettingsResources.Get("Home_NoticeDefaultMessage");
    }

    private static string BuildYellowNoticeMessage()
    {
        var (_, prefix, linkText, plainText, _) =
            HomeCustomizationSummary.DeviceFooter(hasUnsavedChanges: true);

        return !string.IsNullOrEmpty(linkText)
            ? $"{prefix}{linkText}"
            : string.IsNullOrEmpty(plainText)
                ? $"{prefix}{GlassSettingsResources.Get("Home_FooterUnsavedPlain")}"
                : $"{prefix}{plainText}";
    }

    private void RefreshPlaceholdersCard()
    {
        if (_themesRow is null || _placeholdersCard is null)
            return;

        _themesRow.Subtitle = VisualStyleCatalog.HomeLine();
        _themesRow.IsCustomizationHighlighted = false;
        _themesRow.IsUnsavedPending = false;

        foreach (var (row, statusFactory, scope) in _customizationRows)
        {
            if (!IsRowInCard(row, _placeholdersCard))
                continue;

            ApplyRowStatus(row, statusFactory());
        }

        SettingsUnsavedIndicatorRefresh.Refresh(_placeholdersCard);
    }

    internal void TryScheduleStartupNotice()
    {
        _startupNoticePending = HomeSessionNoticeState.StartupNoticePending;
        if (!_startupNoticePending)
            return;

        _startupNoticeScheduled = false;
        ScheduleStartupNoticeAfterShellReady();
    }

    private void ScheduleStartupNoticeAfterShellReady()
    {
        if (_startupNoticeScheduled || HomeSessionNoticeState.StartupNoticeDispatchScheduled)
            return;

        _startupNoticeScheduled = true;
        HomeSessionNoticeState.StartupNoticeDispatchScheduled = true;
        ShellContentReadyGate.SubscribeOnce((_, _) =>
        {
            DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Normal, ShowStartupNoticeAfterShellReady);
        });
    }

    private void ShowStartupNoticeAfterShellReady()
    {
        if (App.ShellWindow is { } shell && !shell.IsCurrentHomePage(this))
        {
            HomeSessionNoticeState.StartupNoticeDispatchScheduled = false;
            _startupNoticeScheduled = false;
            if (HomeSessionNoticeState.StartupNoticePending)
                shell.TryScheduleHomeStartupNotice();
            return;
        }

        HomeSessionNoticeState.StartupNoticeDispatchScheduled = false;
        _startupNoticeScheduled = false;

        if (!_startupNoticePending || _saveInProgress)
        {
            // Стартовый notice уже обработан другим экземпляром — синхронизируем по persisted-флагам.
            RefreshPlatformSummary();
            return;
        }

        _startupNoticePending = false;
        HomeSessionNoticeState.StartupNoticePending = false;
        // Arm до present: footer только после реального показа зеркала (не из WarmAttach/Consume).
        HomeSessionNoticeState.ArmStartupGreenFooterDeposit();

        _ = RefreshNoticesAsync(forceAnimateGreen: true);
    }

    private async void ShellSwitch_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_shellSwitch is null || _shellSuppress)
            return;

        var want = _shellSwitch.IsOn;
        var taskExists = Core.Platform.GlassPlatformService.GlassAutorunTaskExistsForUi(
            GlassPlatformNames.NativeArch);
        if (want && !taskExists)
        {
            _shellSuppress = true;
            _shellSwitch.IsOn = ShellExtensionService.IsBundledDllRegistered();
            _shellSuppress = false;
            return;
        }

        if (ShellExtensionService.IsShellExtensionOperationInProgress())
        {
            _shellSuppress = true;
            _shellSwitch.IsOn = ShellExtensionService.IsBundledDllRegistered();
            _shellSuppress = false;
            return;
        }

        _shellSuppress = true;
        _shellSwitch.IsEnabled = false;
        _shellSuppress = false;

        var ok = await ShellExtensionService.SetShellExtensionRegisteredAsync(want).ConfigureAwait(true);
        if (!ok)
        {
            _shellSuppress = true;
            _shellSwitch.IsOn = ShellExtensionService.IsBundledDllRegistered();
            _shellSuppress = false;
        }

        RefreshSystemCard();
    }

    private void RaiseNav(HomeNavTarget target) =>
        NavigateRequested?.Invoke(this, target);
}
