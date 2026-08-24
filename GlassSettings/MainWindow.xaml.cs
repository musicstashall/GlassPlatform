using System.Runtime.InteropServices;
using GlassSettings.Controls;
using GlassSettings.Core.Dwm;
using GlassSettings.Core.Layout;
using GlassSettings.Core.Placement;
using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Snapshot;
using GlassSettings.Navigation;
using GlassSettings.Pages;
using GlassSettings.Pages.Dwm;
using GlassSettings.Pages.Glass;
using GlassSettings.Pages.Text;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml.Media.Animation;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Navigation;
using Microsoft.UI;
using Windows.Foundation;
using WinRT.Interop;

namespace GlassSettings;

public sealed partial class MainWindow : Window
{
    private readonly SettingsNavigationService _nav = SettingsNavigationService.Current;
    private readonly Dictionary<string, NavigationViewItem> _navItems = new();
    private readonly Dictionary<string, string> _navLabels = new();
    private readonly Dictionary<string, UnsavedIndicatorDot> _navUnsavedDots = new();
    private readonly Dictionary<string, Page> _pages = new();
    private HomePage? _home;
    private string _currentTag = "home";
    private bool _suppressNav;
    /// <summary>Только splash WarmAttach Processes↔Home: не ConsumeGreen до показа стартового green.</summary>
    private bool _suppressHomeNavigatedFrom;
    private bool _initialNavDone;
    private bool _contentReady;
    /// <summary>После fade/MarkReady: shell принимает клики (до этого клики со splash не должны доезжать).</summary>
    private bool _shellInputArmed;
    /// <summary>Home Build/Refresh/Presence/Ensure под заставкой завершены — можно снимать overlay.</summary>
    private bool _shellInteractiveReady;
    private bool _postDismissHooksDone;
    private bool _navigationReady;
    private DateTimeOffset _loadingShownAt = DateTimeOffset.UtcNow;
    private const int LoadingMinimumMs = 800;
    private Task? _attachIconPreload;
    /// <summary>ImageOpened/Failed LoadingIcon — dismiss не раньше decode (x86 async BitmapImage).</summary>
    private TaskCompletionSource<bool> _splashIconReadyTcs =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    private SettingsLayoutMode _layoutMode = SettingsLayoutMode.Medium;
    private DesktopEnvironmentWatcher? _desktopEnvironmentWatcher;
    private DispatcherQueueTimer? _themeBaselineReloadTimer;
    private int _themeBaselineReloadGeneration;
    private GlassSettingsPreferencesWatcher? _preferencesWatcher;
    private RegistrySnapshotWatchService? _registrySnapshotWatch;
    private bool _closeInProgress;
    private bool _allowWindowClose;
    private bool _closeSubclassInstalled;
    private Win32WindowSubclass.Proc? _closeSubclassProc;
    private IntPtr _closeSubclassFn;
    private static readonly UIntPtr CloseSubclassId = new(0x4753_434C); // GSCL
    private const uint WmClose = 0x0010;
    /// <summary>GlassSettingsMsg::WM_GLASS_SETTINGS_ACTIVATE (WM_APP + 0xA03) — ShellExt / second instance.</summary>
    private const uint WmGlassSettingsActivate = 0x8000 + 0xA03;
    private const uint MsgfltAllow = 1;
    private int _windowBusyDepth;
    private TaskCompletionSource<HostedDialogResult>? _hostedDialogTcs;
    private HostedDialogSnapshot? _hostedDialogParentSnapshot;
    private bool _hostedDialogBackdropDismissEnabled;
    private HostedDialogHeader? _hostedDialogHeader;
    private HostedDialogSpec? _hostedDialogRequestedSpec;
    private ScrollViewer? _hostedDialogScrollViewer;
    private bool _hostedDialogBoundsUpdatePending;
    private bool _pendingUiLanguagePageReload;
    private bool _deferredInitDone;
    /// <summary>Последний применённый WindowBackdrop; null — ещё не применяли (startup).</summary>
    private AppWindowBackdropMode? _appliedWindowBackdropMode;
    private bool _transparentChromeApplied;
    private bool _loadingOverlaySizeHooked;
    private bool _deferredWindowChromeApplied;
    private bool _coldStartBootstrapScheduled;
    private bool _glassRuntimeEnsured;
    private bool _bootstrapCoreDone;
    private bool _deferredStartupTasksDone;
    private bool _splashSurfaceActivated;
    private bool _startupCloaked;
    private bool _startupHiddenFromSwitchers;
    private bool _splashFirstPaintReady;
    private int _splashWarmupFramesLeft;
    private const int ShellContentFadeInMs = 500;
    private const int WindowRevealFadeInMs = 400;

    /// <summary>Планирует cold start на следующий проход dispatcher (не внутри Activate/reentrancy).</summary>
    internal void TryCompleteColdStartBootstrap()
    {
        if (_bootstrapCoreDone || _coldStartBootstrapScheduled)
            return;

        _coldStartBootstrapScheduled = true;
        DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Normal, RunColdStartBootstrapCore);
    }

    private void RunColdStartBootstrapCore()
    {
        if (_bootstrapCoreDone)
            return;

        // Минимум видимости заставки — с ActivateAsSplashSurface.
        if (_loadingShownAt == default)
            _loadingShownAt = DateTimeOffset.UtcNow;

        DesktopWallpaperImageCache.BeginLoad(DispatcherQueue);
        // Recent thumbs для страницы «Фон» — под заставкой, до drill-навигации.
        RecentWallpaperThumbCache.BeginLoad(DispatcherQueue);

        // TitleBar chrome; SystemBackdrop уже на splash-surface (ActivateAsSplashSurface).
        ApplyDeferredWindowChrome();
        EnsureSplashBackdropAndTransparent();

        // x86: Presence/watchers до Frame.Navigate → AV на следующем pump (callback cold-nav не стартует).
        // PreNav (BuildNavigation) → Navigate → PostNav (Presence).
        if (!_initialNavDone)
        {
            _initialNavDone = true;
            DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Normal, RunColdStartHomeNavigation);
        }

        _bootstrapCoreDone = true;
        Activated -= OnFirstActivated;
        TrySetWindowIcon();
    }

    private void RunColdStartHomeNavigation()
    {
        if (!_deferredInitDone)
        {
            FinishDeferredInitializationPreNav();
        }

        _currentTag = "home";
        _nav.CurrentSidebarTag = "home";
        _nav.NavigateToRoot(
            typeof(HomePage),
            "home",
            GlassSettingsResources.Get("NavHome"));
        SelectNavItem("home");

        // Fallback: если pipeline завис — снять заставку через несколько секунд.
        _ = LoadingOverlayFallbackAsync();
    }

    /// <summary>
    /// После HomePage.OnLoaded (уже под заставкой): Presence/Ensure, затем dismiss.
    /// Subclass/InjectRuntimeMonitor — только после снятия заставки.
    /// Повторный заход на Home: без cold-idle wipe — сразу AttachColdStartRuntimeHooks.
    /// </summary>
    internal void ScheduleColdStartPostNavAfterHomeLoaded(HomePage home)
    {

        DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Normal, () =>
        {
            // После первого dismiss: новая HomePage иначе остаётся без InjectRuntimeMonitor
            // (OnNavigatedTo при !_built пропускает StartRuntimeMonitor; cold-idle затирает Latest).
            if (_postDismissHooksDone || _contentReady)
            {
                home.AttachColdStartRuntimeHooks();
                return;
            }

            CompleteColdStartPostNavIfNeeded();
            // Ensure на этом же Normal-pump сразу после PostNav. Low после pump_normal_after
            // на x86 не доживает: между ними успевает чужой Normal (icon preload → UI marshal).
            InjectRuntimeMonitor.Instance.PublishColdStartIdleSnapshot();
            CompleteColdStartEnsureUnderSplash();
            MarkShellInteractiveReady();
        });
    }

    private void CompleteColdStartEnsureUnderSplash()
    {
        if (_glassRuntimeEnsured)
        {
            return;
        }

        _glassRuntimeEnsured = true;
        try
        {
            _ = NativeHostInterop.EnsureGlassRuntime();
        }
        catch (Exception)
        {
            // Ensure best-effort под заставкой; сбой не блокирует UI.
        }
    }

    private void MarkShellInteractiveReady()
    {
        if (_shellInteractiveReady)
            return;

        _shellInteractiveReady = true;
        PlatformPresenceService.Instance.StartPresenceWatchAfterShellReady();
        ScheduleDismissLoadingOverlay(fallback: false);
    }

    /// <summary>Presence/watchers после первого pump вне HomePage.OnLoaded.</summary>
    internal void CompleteColdStartPostNavIfNeeded()
    {
        if (_deferredInitDone)
            return;

        FinishDeferredInitializationPostNav();
        _deferredInitDone = true;
    }

    /// <summary>
    /// Baseline сессии + RegWatch в момент dismiss, до MarkReady (startup green/yellow).
    /// Порядок: стартовые записи → Capture → Watch (не наоборот — иначе ложный dirty).
    /// </summary>
    private void RunDeferredStartupTasks()
    {
        if (_deferredStartupTasksDone)
            return;

        _deferredStartupTasksDone = true;
        try
        {
            Core.Platform.OpenGlassBorderPolicyCoordinator.ApplyStartupPolicy();
            SettingsSession.Current.Capture();
            StartRegistrySnapshotWatch();
            _ = ProductUpdateCoordinator.RunStartupAsync();
        }
        catch { /* реестр может быть недоступен при первом запуске */ }

        RefreshNavUnsavedIndicators();
        // Карточки без notices: стартовый green/yellow — один present после MarkReady (иначе OpenImmediate рвёт slide-in).
        _home?.Refresh(refreshNotices: false);
    }

    /// <summary>
    /// SystemBackdrop + transparent — только под уже размеченной заставкой (ActualSize &gt; 0).
    /// Runtime: backdrop при overlay 0×0 даёт видимую mica до layout splash.
    /// </summary>
    private void EnsureSplashBackdropAndTransparent() =>
        TryApplyBackdropAndTransparentWhenOverlayReady();

    private void TryApplyBackdropAndTransparentWhenOverlayReady()
    {
        if (LoadingOverlay.ActualWidth <= 0 || LoadingOverlay.ActualHeight <= 0)
        {
            if (!_loadingOverlaySizeHooked)
            {
                _loadingOverlaySizeHooked = true;
                LoadingOverlay.SizeChanged += OnLoadingOverlaySizeChangedForStartupChrome;
            }

            return;
        }

        ApplyMainWindowBackdropAndTransparent(keepOverlaySolid: !_splashFirstPaintReady);
    }

    private void OnLoadingOverlaySizeChangedForStartupChrome(object sender, SizeChangedEventArgs e)
    {
        if (e.NewSize.Width <= 0 || e.NewSize.Height <= 0)
            return;

        LoadingOverlay.SizeChanged -= OnLoadingOverlaySizeChangedForStartupChrome;
        TryApplyBackdropAndTransparentWhenOverlayReady();
    }

    /// <summary>
    /// Показать Main как splash: SystemBackdrop + иконка.
    /// Activate под DWM cloak, uncloak когда overlay/icon имеют размер (иначе 1–2 с чёрного кадра).
    /// </summary>
    internal void ActivateAsSplashSurface()
    {
        if (_splashSurfaceActivated)
            return;

        _splashSurfaceActivated = true;
        _loadingShownAt = DateTimeOffset.UtcNow;

        // Solid overlay до первого layout — страховка если cloak недоступен.
        EnsureLoadingOverlaySolidBootstrap();
        ApplyMainWindowBackdropAndTransparent(keepOverlaySolid: true);
        SyncLoadingChrome(loading: true);
        // Иконка уже из ctor ApplyAppBranding — не сбрасывать Source повторно.

        try
        {
            var hwnd = WindowNative.GetWindowHandle(this);
            // UIPI: medium-IL explorer (ShellExt) иначе не может PostMessage WM_APP в elevated GS.
            AllowActivateMessageFromLowerIl(hwnd);
            _startupCloaked = StartupWindowCloakService.TrySetCloaked(hwnd, cloaked: true);
            HideFromSwitchersDuringSplash();
        }
        catch
        {
            _startupCloaked = false;
        }

        try
        {
            Activate();
        }
        catch
        {
            /* ignore */
        }

        ReapplyWindowBackdropIfChanged();
        HideFromSwitchersDuringSplash();

        ScheduleSplashFirstPaintReveal();
        ScheduleColdStartAfterPaintFrames(8);
        _ = StartupDismissFallbackAsync();
    }

    private void HideFromSwitchersDuringSplash()
    {
        if (_contentReady)
            return;

        try
        {
            AppWindow.IsShownInSwitchers = false;
            _startupHiddenFromSwitchers = true;
        }
        catch
        {
            /* ignore */
        }
    }

    private void ShowInSwitchersAfterSplash()
    {
        if (!_startupHiddenFromSwitchers)
            return;

        try
        {
            AppWindow.IsShownInSwitchers = true;
        }
        catch
        {
            /* ignore */
        }

        _startupHiddenFromSwitchers = false;
    }

    /// <summary>
    /// Ждём layout overlay+icon и 2 кадра, затем transparent + uncloak — первый видимый кадр = backdrop+иконка.
    /// </summary>
    private void ScheduleSplashFirstPaintReveal()
    {
        if (_splashFirstPaintReady)
            return;

        _splashWarmupFramesLeft = 2;
        Microsoft.UI.Xaml.Media.CompositionTarget.Rendering += OnSplashFirstPaintRendering;
        _ = SplashFirstPaintFallbackAsync();
    }

    private async Task SplashFirstPaintFallbackAsync()
    {
        try
        {
            await Task.Delay(1500).ConfigureAwait(true);
            if (!_splashFirstPaintReady)
            {
                Microsoft.UI.Xaml.Media.CompositionTarget.Rendering -= OnSplashFirstPaintRendering;
                FinishSplashFirstPaintReveal("fallback-timeout");
            }
        }
        catch
        {
            /* ignore */
        }
    }

    private void OnSplashFirstPaintRendering(object? sender, object e)
    {
        if (_splashFirstPaintReady)
        {
            Microsoft.UI.Xaml.Media.CompositionTarget.Rendering -= OnSplashFirstPaintRendering;
            return;
        }

        if (LoadingOverlay.ActualWidth <= 0 || LoadingOverlay.ActualHeight <= 0
            || LoadingIcon.ActualWidth <= 0 || LoadingIcon.ActualHeight <= 0)
        {
            return;
        }

        _splashWarmupFramesLeft--;
        if (_splashWarmupFramesLeft > 0)
            return;

        Microsoft.UI.Xaml.Media.CompositionTarget.Rendering -= OnSplashFirstPaintRendering;
        FinishSplashFirstPaintReveal("layout-ready");
    }

    private void FinishSplashFirstPaintReveal(string _)
    {
        if (_splashFirstPaintReady)
            return;

        _splashFirstPaintReady = true;

        // Acrylic через transparent; solid bootstrap больше не нужен.
        ApplyTransparentSplashChrome(forceOverlayTransparent: true);
        ReapplyWindowBackdropIfChanged();

        // WinUI fade вместо DWM open-animation: контент 0→1 после uncloak.
        RootGrid.Opacity = 0;

        if (_startupCloaked)
        {
            try
            {
                var hwnd = WindowNative.GetWindowHandle(this);
                StartupWindowCloakService.TrySetCloaked(hwnd, cloaked: false);
            }
            catch
            {
                /* best-effort */
            }

            _startupCloaked = false;
        }

        // Кнопка taskbar — вместе с видимой заставкой.
        ShowInSwitchersAfterSplash();
        BeginOpacityFadeIn(RootGrid, WindowRevealFadeInMs);
    }

    /// <summary>
    /// Только safety dismiss. Reveal без готовности UI запрещён.
    /// </summary>
    private async Task StartupDismissFallbackAsync()
    {
        try
        {
            await Task.Delay(8000).ConfigureAwait(true);
            if (_contentReady)
                return;

            ScheduleDismissLoadingOverlay(fallback: true);
        }
        catch
        {
            /* ignore */
        }
    }

    private void EnsureLoadingOverlaySolidBootstrap()
    {
        LoadingOverlay.Background =
            (Brush)Application.Current.Resources["SolidBackgroundFillColorBaseBrush"];
    }

    private void ApplyTransparentSplashChrome(bool forceOverlayTransparent = false)
    {
        if (!_transparentChromeApplied)
        {
            _transparentChromeApplied = true;
            StartupChromeService.ApplyTransparentLayers(
                RootGrid, WindowBlurSourceHost, forceOverlayTransparent ? LoadingOverlay : null, NavView, NavFrame);
            if (!forceOverlayTransparent)
                EnsureLoadingOverlaySolidBootstrap();
        }
        else if (forceOverlayTransparent)
        {
            LoadingOverlay.Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        }
    }

    private void ApplyMainWindowBackdropAndTransparent(bool keepOverlaySolid = false)
    {
        ReapplyWindowBackdropIfChanged();
        ApplyTransparentSplashChrome(forceOverlayTransparent: !keepOverlaySolid);
    }

    /// <summary>
    /// Применить SystemBackdrop из реестра только при смене режима (startup + live из AppSettings).
    /// </summary>
    private void ReapplyWindowBackdropIfChanged()
    {
        var mode = GlassSettingsRegistry.ReadWindowBackdrop();
        if (_appliedWindowBackdropMode == mode)
            return;

        WindowBackdropService.ApplyFromRegistry(this, AcrylicBackdropLayer);
        _appliedWindowBackdropMode = mode;
    }

    /// <summary>XamlRoot окна — для ContentDialog и полноэкранной шторы.</summary>
    public XamlRoot? WindowXamlRoot => RootGrid.XamlRoot;

    internal FrameworkElement? ActivePageRoot => NavFrame.Content as FrameworkElement;

    public MainWindow()
    {
        InitializeComponent();
        ApplyEarlyWindowPlacement();
        ApplyStartupVisuals();
        Title = SingleInstance.WindowTitle;
        ApplyAppBranding();
        NavFrame.Navigated += NavFrame_OnNavigated;
        _nav.Attach(NavFrame);
        Activated += OnWindowActivated;
        Activated += OnFirstActivated;
        SettingsSession.Current.DirtyChanged += (_, _) =>
        {
            _home?.SyncSummaryFooterFromSession();
            SettingsUnsavedIndicatorCoordinator.RefreshAll(this, _home);
        };
        HomeSessionNoticeState.SummaryFooterRevealedChanged += (_, _) => RefreshDisplayedHomeFooter();
    }

    /// <summary>Обновить footer на текущей отображаемой странице «Обзор» (а не на устаревшем экземпляре).</summary>
    private void RefreshDisplayedHomeFooter()
    {
        var home = NavFrame.Content as HomePage ?? _home;
        if (home is null)
            return;

        DispatcherQueue.TryEnqueue(home.SyncSummaryFooterFromSession);
    }

    /// <summary>
    /// Chrome до Activate: theme + SystemBackdrop + transparent + иконка (рецепт Gallery-splash).
    /// </summary>
    private void ApplyStartupVisuals()
    {
        try
        {
            ExtendsContentIntoTitleBar = true;
            SetTitleBar(AppTitleBar);
        }
        catch { /* TitleBar API может быть недоступен */ }

        AppThemeService.ApplyFromRegistry(RootGrid);
        EnsureLoadingOverlaySolidBootstrap();
        ApplyMainWindowBackdropAndTransparent(keepOverlaySolid: true);
        SyncLoadingChrome(loading: true);
        // После темы — снова выставить иконку (иначе LoadingIcon может остаться пустым).
        ApplyAppBranding();
    }

    private void ApplyDeferredWindowChrome()
    {
        if (_deferredWindowChromeApplied)
            return;

        _deferredWindowChromeApplied = true;
        try
        {
            ExtendsContentIntoTitleBar = true;
            SetTitleBar(AppTitleBar);
        }
        catch { /* TitleBar API может быть недоступен */ }

        TitleBarThemeHelper.Apply(this, extendsContentIntoTitleBar: true);
    }

    private void FinishDeferredInitialization()
    {
        FinishDeferredInitializationPreNav();
        FinishDeferredInitializationPostNav();
    }

    /// <summary>Shell/nav до первого Frame.Navigate (без Presence — на x86 Presence до Navigate роняет следующий pump).</summary>
    private void FinishDeferredInitializationPreNav()
    {
        ApplyWindowBusyOverlayFill();
        ThemeShadowHost.Attach(DialogShadowRect, DialogShadowCastGrid, DialogShadow);
        HostedDialogHost.AddHandler(
            UIElement.PointerPressedEvent,
            (PointerEventHandler)HostedDialogHost_OnPointerPressed,
            true);
        HostedDialogKeyboardBehavior.AttachDismissOnEscape(
            HostedDialogHost,
            () => HostedDialogHost.Visibility == Visibility.Visible && _hostedDialogTcs is not null,
            () => CloseHostedDialog(HostedDialogResult.Dismissed));
        ApplyLocalizedStrings();
        _nav.TrailChanged += OnTrailChanged;
        SettingsContentColumnLayout.AttachHostColumn(TrailColumn);
        SettingsContentColumnLayout.AttachHostColumn(WindowTopNoticeColumn);
        NoticeViewportMirrorService.Initialize();
        ApplyWindowTopNoticeChrome();
        BuildNavigation();
        UpdateBackButton();
        MainWindowNavigation.NavigateSidebar = tag =>
        {
            NavigateTo(tag, force: true);
            SelectNavItem(tag);
        };
        HookTitleBarTheme();
        SyncBreadcrumb();
    }

    /// <summary>Presence/политики под заставкой (без HWND-subclass и без InjectRuntimeMonitor).</summary>
    private void FinishDeferredInitializationPostNav()
    {
        StartGlassSettingsPreferencesWatcher();
        PlatformPresenceService.Instance.Attach(DispatcherQueue);
        OpenGlassBorderPolicyCoordinator.AttachPresenceListener();
        SwcaDwmBackdropNoticeCoordinator.AttachPresenceListener();
        OpenGlassRootBackdropNoticeCoordinator.AttachPresenceListener();
        OpenGlassUiPolicy.AttachPresenceListener();
        OpenGlassUiPolicy.Changed += OnOpenGlassUiPolicyChanged;
        CompositionWallpaperLayersCache.AttachPolicyListener(DispatcherQueue);
        HookWindowChrome();
        // Closing без SetWindowSubclass — subclass ставится после dismiss (нет гонки с cold-start).
        AppWindow.Closing += OnAppWindowClosing;
        // PreloadAttachListAsync — после dismiss: Task.Run→UI marshal на Normal успевал
        // убить x86 между pump_normal_after и Low Ensure.
        ApplyPlacement();
        RootGrid.ActualThemeChanged += (_, _) =>
        {
            ApplyWindowBusyOverlayFill();
            NavigationPaneChromeService.Apply(NavView, _layoutMode);
            RefreshMirrorStripChrome();
        };
        HookNavigationUserCardLayout();
    }

    /// <summary>После снятия заставки: DesktopEnvironmentWatcher, Close-subclass, runtime monitor.</summary>
    private void StartPostDismissShellHooks()
    {
        if (_postDismissHooksDone)
            return;

        _postDismissHooksDone = true;
        StartDesktopEnvironmentWatcher();
        InstallCloseSubclassWhenReady();
        if (NavFrame.Content is HomePage home)
            home.AttachColdStartRuntimeHooks();
        // Names-only preload уже под заставкой (см. DismissLoadingOverlayWhenReadyAsync).
        _shellReadyWorkRequested = true;
        RunDeferredShellReadyWorkAfterFirstProbe();
    }

    private void HookNavigationUserCardLayout()
    {
        void Sync() => SyncNavigationUserCardLayout();

        var lastContentHeight = double.NaN;
        var lastNavHeight = double.NaN;

        NavigationUserCardHost.Loaded += (_, _) => Sync();
        PageContentHost.SizeChanged += (_, _) =>
        {
            var h = PageContentHost.ActualHeight;
            if (!double.IsNaN(lastContentHeight) && Math.Abs(h - lastContentHeight) < 0.5)
                return;

            lastContentHeight = h;
            Sync();
        };
        NavView.SizeChanged += (_, _) =>
        {
            var h = NavView.ActualHeight;
            if (!double.IsNaN(lastNavHeight) && Math.Abs(h - lastNavHeight) < 0.5)
                return;

            lastNavHeight = h;
            Sync();
        };
        NavView.RegisterPropertyChangedCallback(
            NavigationView.IsPaneOpenProperty,
            (_, _) =>
            {
                SyncNavigationUserCardLayout();
                ApplySearchPlacement();
            });
        RootGrid.Loaded += (_, _) => Sync();
    }

    private void SyncNavigationUserCardLayout()
    {
        NavigationPaneHeaderLayout.Apply(NavView, NavigationUserCardHost, PageContentHost);
        NavigationPaneChromeService.Apply(NavView, _layoutMode);
    }

    private bool _firstFrameLogged;

    private void ScheduleColdStartAfterPaintFrames(int frames)
    {
        if (frames <= 0)
        {
            TryCompleteColdStartBootstrap();
            return;
        }

        var left = frames;
        void OnRendering(object? sender, object e)
        {
            left--;
            if (left > 0)
                return;

            if (_firstFrameLogged)
            {
                Microsoft.UI.Xaml.Media.CompositionTarget.Rendering -= OnRendering;
                return;
            }

            _firstFrameLogged = true;
            Microsoft.UI.Xaml.Media.CompositionTarget.Rendering -= OnRendering;
            TryCompleteColdStartBootstrap();
        }

        Microsoft.UI.Xaml.Media.CompositionTarget.Rendering += OnRendering;
    }

    private void OnWindowActivated(object sender, WindowActivatedEventArgs e) =>
        UpdateBackButton();

    private void OnFirstActivated(object sender, WindowActivatedEventArgs e)
    {
        if (e.WindowActivationState == WindowActivationState.Deactivated)
            return;

        TryCompleteColdStartBootstrap();
    }

    /// <summary>Применить тему и backdrop из реестра GlassSettings.</summary>
    public void ApplyAppPreferencesFromRegistry()
    {
        AppThemeService.ApplyFromRegistry(RootGrid);
        NavigationPaneChromeService.Apply(NavView, _layoutMode);
        ApplyWindowBusyOverlayFill();
        // Runtime: backdrop по кэшу режима. Startup splash — ApplyMainWindowBackdropAndTransparent.
        ReapplyWindowBackdropIfChanged();
        TitleBarThemeHelper.Apply(this, extendsContentIntoTitleBar: true);
        ApplyHostedDialogTheme();
        ApplyWindowTopNoticeChrome();
    }

    private readonly List<SettingsUnsavedNoticeStrip> _mirrorStrips = new();

    internal void ApplyWindowTopNoticeChrome() => RefreshMirrorStripChrome();

    /// <summary>Перечитывает оформление зеркальных полос (акрил, severity) и nav-точек после смены темы.</summary>
    internal void RefreshMirrorStripChrome()
    {
        foreach (var dot in _navUnsavedDots.Values)
            dot.RefreshChrome();

        if (!IsTopNoticeMirrorOpen)
            return;

        AppThemeService.ApplyElementThemeOnly(WindowTopNoticeColumn);

        foreach (var child in WindowTopNoticeMirrorStack.Children)
        {
            if (child is not Border border)
                continue;

            border.BorderBrush = AppThemeVisualResolver.ResolveThemeBrush("SurfaceStrokeColorDefaultBrush");
            WindowBackdropService.ApplyMirrorNoticeChromeBackground(border);
        }

        foreach (var strip in _mirrorStrips)
        {
            AppThemeService.ApplyElementThemeOnly(strip);
            strip.RefreshSeverityChrome();
        }
    }

    internal bool IsTopNoticeMirrorOpen => WindowTopNoticeColumn.Visibility == Visibility.Visible;

    internal void SyncTopNoticeMirrorSaveEnabled(bool enabled)
    {
        foreach (var strip in _mirrorStrips)
            strip.IsSaveEnabled = enabled;
    }

    internal async Task ShowTopNoticeMirrorStackAsync(
        IReadOnlyList<TopNoticeMirrorStripSpec> specs,
        bool animate)
    {
        NoticeMirrorTrace.Write($"ShowTopNoticeMirrorStackAsync: specs={specs.Count}, animate={animate}");
        if (specs.Count == 0)
        {
            await HideTopNoticeMirrorAsync(animate);
            return;
        }

        RebuildMirrorStrips(specs);
        WindowTopNoticeColumn.Visibility = Visibility.Visible;

        if (!animate)
        {
            NoticeHostSlideAnimator.ResetState(WindowTopNoticeColumn, WindowTopNoticeTransform);
            return;
        }

        NoticeHostSlideAnimator.PrepareSlideInStart(WindowTopNoticeColumn, WindowTopNoticeTransform);
        WindowTopNoticeMirrorStack.UpdateLayout();
        // Первый показ: только что созданные полосы зеркала ещё не реализовали шаблон/тему — host
        // имеет нулевую высоту, и slide «съедается». Дождёмся layout, чтобы анимация была видна.
        await WaitForMirrorHostLayoutAsync();
        await NoticeHostSlideAnimator.SlideInAsync(WindowTopNoticeColumn, WindowTopNoticeTransform);
    }

    private Task WaitForMirrorHostLayoutAsync()
    {
        var queue = DispatcherQueue;
        if (queue is null)
            return Task.CompletedTask;

        var tcs = new TaskCompletionSource<bool>();
        queue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, () =>
        {
            WindowTopNoticeMirrorStack.UpdateLayout();
            tcs.TrySetResult(true);
        });
        return tcs.Task;
    }

    /// <summary>Пересобрать полосы зеркала, сохранив текущую позицию host (без slide-анимации).</summary>
    internal void UpdateTopNoticeMirrorStack(IReadOnlyList<TopNoticeMirrorStripSpec> specs)
    {
        NoticeMirrorTrace.Write($"UpdateTopNoticeMirrorStack: specs.Count={specs.Count}");
        if (specs.Count == 0)
        {
            HideTopNoticeMirrorImmediate();
            return;
        }

        RebuildMirrorStrips(specs);
        WindowTopNoticeColumn.Visibility = Visibility.Visible;
        NoticeHostSlideAnimator.ResetState(WindowTopNoticeColumn, WindowTopNoticeTransform);
    }

    internal Task ShowTopNoticeMirrorAsync(
        SettingsNoticeSeverity severity,
        string message,
        bool showSave,
        bool animate) =>
        ShowTopNoticeMirrorStackAsync(
            [new TopNoticeMirrorStripSpec(severity, message, showSave, saveEnabled: true)],
            animate);

    internal async Task HideTopNoticeMirrorAsync(bool animate)
    {
        NoticeMirrorTrace.Write($"HideTopNoticeMirrorAsync: animate={animate}, open={IsTopNoticeMirrorOpen}");
        if (!IsTopNoticeMirrorOpen)
        {
            HideTopNoticeMirrorImmediate();
            return;
        }

        if (animate)
            await NoticeHostSlideAnimator.SlideUpOutAsync(WindowTopNoticeColumn, WindowTopNoticeTransform);

        HideTopNoticeMirrorImmediate();
    }

    internal void HideTopNoticeMirrorImmediate()
    {
        NoticeMirrorTrace.Write($"HideTopNoticeMirrorImmediate: strips={_mirrorStrips.Count}");
        NoticeHostSlideAnimator.CancelActive(WindowTopNoticeColumn, WindowTopNoticeTransform);
        WindowTopNoticeColumn.Visibility = Visibility.Collapsed;
        ClearMirrorStrips();
    }

    private void RebuildMirrorStrips(IReadOnlyList<TopNoticeMirrorStripSpec> specs)
    {
        NoticeMirrorTrace.Write($"RebuildMirrorStrips: пересоздание {specs.Count} зеркал (было {_mirrorStrips.Count})");
        ClearMirrorStrips();

        foreach (var spec in specs)
        {
            var strip = new SettingsUnsavedNoticeStrip
            {
                AllowDismiss = true,
                FloatingOverlayChrome = true,
                HorizontalAlignment = HorizontalAlignment.Stretch,
                IsHitTestVisible = true,
                Tag = spec.SourceStrip
            };
            AppThemeService.ApplyThemeToElement(strip);
            strip.SaveRequested += MirrorStrip_OnSaveRequested;
            strip.ActionRequested += MirrorStrip_OnActionRequested;
            strip.DismissRequested += MirrorStrip_OnDismissRequested;
            strip.OpenImmediate(spec.Severity, spec.Message, spec.ShowSave, spec.ShowAction, spec.ActionText);
            strip.IsSaveEnabled = spec.SaveEnabled;

            // Оборачиваем каждую полосу в Border с обводкой, фоном и тенью
            var border = new Border
            {
                BorderBrush = AppThemeVisualResolver.ResolveThemeBrush("SurfaceStrokeColorDefaultBrush"),
                BorderThickness = new Thickness(1),
                CornerRadius = new CornerRadius(4),
                Child = strip
            };
            WindowBackdropService.ApplyMirrorNoticeChromeBackground(border);

            // Применяем тень к каждой полосе
            border.Translation = new System.Numerics.Vector3(0, 0, 12);
            border.Shadow = new ThemeShadow();

            WindowTopNoticeMirrorStack.Children.Add(border);
            _mirrorStrips.Add(strip);
        }

        NoticeMirrorTrace.Write($"RebuildMirrorStrips: создано {_mirrorStrips.Count} зеркал");
    }

    private void ClearMirrorStrips()
    {
        var stackTrace = new System.Diagnostics.StackTrace(true);
        NoticeMirrorTrace.Write($"ClearMirrorStrips: удаление {_mirrorStrips.Count} зеркал\n{stackTrace}");
        foreach (var strip in _mirrorStrips)
        {
            strip.SaveRequested -= MirrorStrip_OnSaveRequested;
            strip.ActionRequested -= MirrorStrip_OnActionRequested;
            strip.DismissRequested -= MirrorStrip_OnDismissRequested;
            strip.CloseImmediate();
        }

        _mirrorStrips.Clear();
        WindowTopNoticeMirrorStack.Children.Clear();
    }

    /// <summary>Удалить одно зеркало из списка по source strip.</summary>
    private void RemoveMirrorStrip(SettingsUnsavedNoticeStrip mirrorStrip)
    {
        var index = _mirrorStrips.IndexOf(mirrorStrip);
        NoticeMirrorTrace.Write($"RemoveMirrorStrip: index={index}, всего зеркал={_mirrorStrips.Count}");
        if (index < 0)
            return;

        mirrorStrip.SaveRequested -= MirrorStrip_OnSaveRequested;
        mirrorStrip.ActionRequested -= MirrorStrip_OnActionRequested;
        mirrorStrip.DismissRequested -= MirrorStrip_OnDismissRequested;
        mirrorStrip.CloseImmediate();

        _mirrorStrips.RemoveAt(index);

        // Удаляем Border, содержащий этот strip
        if (index < WindowTopNoticeMirrorStack.Children.Count)
            WindowTopNoticeMirrorStack.Children.RemoveAt(index);

        NoticeMirrorTrace.Write($"RemoveMirrorStrip: после удаления осталось {_mirrorStrips.Count} зеркал");
    }

    private void MirrorStrip_OnSaveRequested(object? sender, EventArgs e)
    {
        if (sender is SettingsUnsavedNoticeStrip { Tag: SettingsUnsavedNoticeStrip source })
            source.NotifySaveRequested();
    }

    private void MirrorStrip_OnActionRequested(object? sender, EventArgs e)
    {
        if (sender is SettingsUnsavedNoticeStrip { Tag: SettingsUnsavedNoticeStrip source })
            source.NotifyActionRequested();
    }

    private void MirrorStrip_OnDismissRequested(object? sender, EventArgs e)
    {
        NoticeMirrorTrace.Write("MirrorStrip_OnDismissRequested: начало");
        if (sender is not SettingsUnsavedNoticeStrip mirrorStrip)
            return;

        if (mirrorStrip.Tag is not SettingsUnsavedNoticeStrip source)
            return;

        NoticeMirrorTrace.WriteMirror(
            $"MirrorStrip_OnDismissRequested: source open={source.IsVisuallyOpen}, severity={source.Severity}, msg={source.Message}");

        // Green MirrorOnly: slide-out всего host (1000 ms) — без RemoveMirrorStrip/HideMirrorImmediate.
        if (!source.IsVisuallyOpen && NoticeViewportMirrorService.IsInformationalMirrorActive)
        {
            NoticeViewportMirrorService.ReleaseSourceWithoutSync(source);
            NoticeMirrorTrace.Write(
                "MirrorStrip_OnDismissRequested: informational mirror → animated CompleteInformationalDismissFromMirrorAsync");
            _ = NoticeViewportMirrorService.CompleteInformationalDismissFromMirrorAsync(source);
            NoticeMirrorTrace.Write("MirrorStrip_OnDismissRequested: конец (informational animated)");
            return;
        }

        // Pinned Warning/Action/Error: крестик зеркала = hide host + dismiss epoch; штатная полоса жива
        // (как activity-dismiss). Не NotifyDismiss / CloseImmediate / OnUserDismissed на source.
        NoticeMirrorTrace.Write(
            $"MirrorStrip_OnDismissRequested: pinned mirror X → HideMirrorImmediate, source keep severity={source.Severity}");
        NoticeViewportMirrorService.HideMirrorImmediate(manualDismissForPage: true);
        NoticeMirrorTrace.Write("MirrorStrip_OnDismissRequested: конец (pinned keep strip)");
    }

    private void ApplyWindowBusyOverlayFill() =>
        WindowBusyOverlayService.ApplyFill(WindowBusyOverlayFill, RootGrid);

    private void ApplyAppBranding()
    {
        TrySetWindowIcon();

        // Decode: dismiss/reveal ждут ImageOpened (ранний TrySetResult давал uncloak без иконки).
        var iconSource = AppIconSourceFactory.CreateImageSource();
        if (iconSource is not null)
        {
            if (_splashIconReadyTcs.Task.IsCompleted)
            {
                _splashIconReadyTcs =
                    new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
            }

            LoadingIcon.ImageOpened -= LoadingIcon_OnImageOpened;
            LoadingIcon.ImageFailed -= LoadingIcon_OnImageFailed;
            LoadingIcon.ImageOpened += LoadingIcon_OnImageOpened;
            LoadingIcon.ImageFailed += LoadingIcon_OnImageFailed;
            LoadingIcon.Opacity = 1;
            LoadingIcon.Visibility = Visibility.Visible;
            LoadingIcon.Source = iconSource;
            // Уже декодированный BitmapImage — PixelWidth > 0 без ожидания ImageOpened.
            if (iconSource is BitmapImage { PixelWidth: > 0 })
                _splashIconReadyTcs.TrySetResult(true);
        }
        else
        {
            _splashIconReadyTcs.TrySetResult(false);
        }
    }

    private void LoadingIcon_OnImageOpened(object sender, RoutedEventArgs e)
    {
        _splashIconReadyTcs.TrySetResult(true);
    }

    private void LoadingIcon_OnImageFailed(object sender, ExceptionRoutedEventArgs e)
    {
        _splashIconReadyTcs.TrySetResult(false);
    }

    private void TrySetWindowIcon()
    {
        try
        {
            var iconPath = AppIconSourceFactory.ResolveIconPath();
            if (File.Exists(iconPath))
                AppWindow.SetIcon(iconPath);
        }
        catch { /* ignore */ }
    }

    private void TryDismissLoadingOverlay()
    {
        if (_contentReady)
            return;

        _contentReady = true;
        NavView.Visibility = Visibility.Visible;
        if (NavFrame.Content is not Page)
            NavigateTo(_currentTag, force: true);

        ApplyLayoutMode(GetLayoutWidth());
        SyncNavigationUserCardLayout();
        // Prep строго под splash: loading chrome / hit-test off (SyncLoadingChrome ещё true).
        RunDeferredStartupTasks();
        // WarmAttach до снятия overlay — иначе Processes «догоняет» после dismiss.
        WarmAttachListPageIfReady();

        // Контент под fade: host Opacity=0, затем 0→1 за 500 ms после снятия splash.
        // Hit-test остаётся off до MarkReady — иначе клики со splash доезжают как Home GlassDrill.
        WindowBlurSourceHost.Opacity = 0;
        LoadingOverlay.Visibility = Visibility.Collapsed;
        SyncLoadingChrome(loading: false, allowHitTest: false);
        ShowInSwitchersAfterSplash();
        // MarkReady — после fade-in: стартовое green со штатным slide-in, не в первом кадре fade.
        BeginShellContentFadeIn(ShellContentFadeInMs, onCompleted: () =>
        {
            ArmShellInputAfterSplash();
            ShellContentReadyGate.MarkReady();
        });
        DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, StartPostDismissShellHooks);
    }

    /// <summary>Включить hit-test shell только после fade — гасит отложенные клики со splash.</summary>
    private void ArmShellInputAfterSplash()
    {
        if (_shellInputArmed)
            return;

        SyncLoadingChrome(loading: false, allowHitTest: true);
        _shellInputArmed = true;
    }

    private void BeginShellContentFadeIn(int durationMs, Action? onCompleted = null) =>
        BeginOpacityFadeIn(WindowBlurSourceHost, durationMs, onCompleted);

    private static void BeginOpacityFadeIn(UIElement target, int durationMs, Action? onCompleted = null)
    {
        try
        {
            target.Opacity = 0;
            var anim = new DoubleAnimation
            {
                From = 0,
                To = 1,
                Duration = new Duration(TimeSpan.FromMilliseconds(durationMs)),
                EnableDependentAnimation = true
            };
            Storyboard.SetTarget(anim, target);
            Storyboard.SetTargetProperty(anim, "Opacity");
            var sb = new Storyboard();
            sb.Children.Add(anim);
            if (onCompleted is not null)
                sb.Completed += (_, _) => onCompleted();
            sb.Begin();
        }
        catch
        {
            target.Opacity = 1;
            onCompleted?.Invoke();
        }
    }

    private bool _shellReadyWorkRequested;
    private bool _shellReadyWorkDone;

    /// <summary>
    /// Маркер после первой CaptureSnapshot: WarmAttach/composition — только под заставкой.
    /// </summary>
    internal void RunDeferredShellReadyWorkAfterFirstProbe()
    {
        if (_shellReadyWorkDone || !_shellReadyWorkRequested)
            return;

        // WarmAttach / composition prewarm — строго под splash (Dismiss/TryDismiss), не после MarkReady.
        _shellReadyWorkDone = true;
    }

    private void SyncLoadingChrome(bool loading, bool? allowHitTest = null)
    {
        // Не Collapsed: иначе HomePage.OnLoaded не вызывается → dismiss только через 8 с fallback.
        // Opacity=0: shell не виден под transparent splash, Loaded/Ensure идут как раньше.
        // allowHitTest: при dismiss визуал уже loading=false, но hit-test держим false до MarkReady.
        var hit = allowHitTest ?? !loading;
        NavView.Visibility = Visibility.Visible;
        NavView.Opacity = loading ? 0 : 1;
        NavView.IsHitTestVisible = hit;
        PageContentHost.IsHitTestVisible = hit;
        NavFrame.IsHitTestVisible = hit;
        AppTitleBar.Visibility = loading ? Visibility.Collapsed : Visibility.Visible;
        SearchBox.Visibility = loading ? Visibility.Collapsed : Visibility.Visible;
        SearchBox.IsHitTestVisible = hit;
        if (!loading)
            ApplySearchPlacement();
    }

    /// <summary>Клики по заставке не должны доходить до shell / тумблеров Главной.</summary>
    private void LoadingOverlay_OnPointerInput(object sender, PointerRoutedEventArgs e)
    {
        e.Handled = true;
    }

    private void LoadingOverlay_OnTappedInput(object sender, TappedRoutedEventArgs e)
    {
        e.Handled = true;
    }

    private void LoadingOverlay_OnDoubleTappedInput(object sender, DoubleTappedRoutedEventArgs e)
    {
        e.Handled = true;
    }

    private void TitleBarBackButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (!_nav.CanGoBackGlobal)
            return;

        PerformGlobalBack();
    }

    private void PerformGlobalBack()
    {
        if (!_nav.TryGoBackGlobal(out var restored))
            return;

        if (restored?.SidebarTag is { } tag)
            _currentTag = tag;

        if (restored?.SidebarTag is { } navTag)
            SelectNavItem(navTag);

        ScheduleBackButtonRefresh();
    }

    private void ScheduleBackButtonRefresh() =>
        UpdateBackButton();

    private void AppTitleBar_OnPaneToggleRequested(TitleBar sender, object args)
    {
        if (_layoutMode != SettingsLayoutMode.Narrow) return;
        NavView.IsPaneOpen = !NavView.IsPaneOpen;
    }

    private async void AppSettingsPaneButton_OnClick(object sender, RoutedEventArgs e) =>
        await ShowAppSettingsDialogAsync();

    private async Task ShowAppSettingsDialogAsync()
    {
        if (RootGrid.XamlRoot is null)
            return;

        await AppSettingsDialogService.ShowAsync(RootGrid.XamlRoot);
    }

    /// <summary>Hosted-диалог: штора + панель, крестик, клик по шторе.</summary>
    internal Task<HostedDialogResult> ShowHostedDialogAsync(
        HostedDialogSpec spec,
        Action<HostedDialogHostContext>? configure = null) =>
        RunHostedDialogAsync(spec, deferPrimaryClose: false, configure);

    /// <summary>Primary → сразу скрыть панель, штора+progress, procedure, Pop.</summary>
    internal async Task<bool> ShowHostedDialogThenRunAsync(HostedDialogSpec spec, Func<Task> procedure)
    {
        var result = await RunHostedDialogAsync(spec, deferPrimaryClose: true, configure: null)
            .ConfigureAwait(true);

        if (result != HostedDialogResult.Primary)
            return false;

        if (HostedDialogHost.Visibility == Visibility.Visible)
            HideHostedDialogPanelForProcedure();

        await SettingsInteractionDeferral.RunAfterUiSettledAsync(DispatcherQueue).ConfigureAwait(true);

        try
        {
            await procedure().ConfigureAwait(true);
        }
        finally
        {
            WindowBusyOverlayService.Pop();
            RefreshActivePageSessionNoticeIfDirty();
        }

        return true;
    }

    private async Task<HostedDialogResult> RunHostedDialogAsync(
        HostedDialogSpec spec,
        bool deferPrimaryClose,
        Action<HostedDialogHostContext>? configure)
    {
        var dialogPad = GetHostedDialogPadding();
        HostedDialogSurface.Padding = new Thickness(0, dialogPad.Top, 0, spec.Footer is null ? dialogPad.Bottom : 0);
        UpdateHostedDialogSurfaceClip();

        var build = BuildHostedDialogChrome(
            spec,
            deferPrimaryClose,
            configure,
            ShowChildHostedDialogAsync);

        _hostedDialogHeader = build.Header;
        _hostedDialogScrollViewer = build.ScrollViewer;
        HostedDialogContent.Content = build.Chrome;
        _hostedDialogRequestedSpec = spec;
        UpdateHostedDialogVerticalInset();
        ApplyHostedDialogBounds(liveResize: false);

        _hostedDialogBackdropDismissEnabled = false;
        HostedDialogHost.IsHitTestVisible = false;

        if (spec.ManageOverlay)
        {
            WindowBusyOverlayService.Push(showProgress: false);
        }
        else
        {
            WindowBusyProgressRing.Visibility = Visibility.Collapsed;
        }

        HostedDialogHost.Visibility = Visibility.Visible;
        HostedDialogHost.IsHitTestVisible = true;
        ApplyHostedDialogTheme();
        HostedDialogRevealAnimator.PrepareHidden(DialogShadowRect);
        await HostedDialogRevealAnimator.RevealAsync(DialogShadowRect).ConfigureAwait(true);
        UpdateHostedDialogSurfaceClip();
        ApplyHostedDialogBounds(liveResize: true);
        _hostedDialogBackdropDismissEnabled = true;

        _hostedDialogTcs = new TaskCompletionSource<HostedDialogResult>();
        var panelHiddenForProcedure = false;
        var result = HostedDialogResult.Dismissed;
        try
        {
            result = await _hostedDialogTcs.Task;
            panelHiddenForProcedure = deferPrimaryClose && result == HostedDialogResult.Primary;
        }
        finally
        {
            _hostedDialogBackdropDismissEnabled = false;

            if (!panelHiddenForProcedure)
            {
                HostedDialogHost.IsHitTestVisible = false;
                HostedDialogHost.Visibility = Visibility.Collapsed;
                HostedDialogContent.Content = null;
                DialogShadowRect.ClearValue(FrameworkElement.MarginProperty);
                _hostedDialogHeader = null;
                _hostedDialogRequestedSpec = null;
                _hostedDialogScrollViewer = null;
                HostedDialogRevealAnimator.ResetVisible(DialogShadowRect);
                ResetHostedDialogSurfacePadding();
            }

            if (spec.ManageOverlay && !panelHiddenForProcedure)
                WindowBusyOverlayService.Pop();

            _hostedDialogTcs = null;
            _hostedDialogParentSnapshot = null;
            TryReloadPageAfterLanguageChange();
        }

        return result;
    }

    private async Task<HostedDialogResult> ShowChildHostedDialogAsync(HostedDialogSpec spec)
    {
        if (_hostedDialogTcs is null || _hostedDialogParentSnapshot is not null)
            return HostedDialogResult.Dismissed;

        var snapshot = new HostedDialogSnapshot
        {
            Chrome = (Grid)HostedDialogContent.Content!,
            Spec = _hostedDialogRequestedSpec!,
            Header = _hostedDialogHeader,
            ScrollViewer = _hostedDialogScrollViewer,
            SurfacePadding = HostedDialogSurface.Padding,
            BackdropDismissEnabled = _hostedDialogBackdropDismissEnabled,
            ParentTcs = _hostedDialogTcs
        };
        _hostedDialogParentSnapshot = snapshot;

        var childTcs = new TaskCompletionSource<HostedDialogResult>();
        _hostedDialogTcs = childTcs;
        _hostedDialogBackdropDismissEnabled = false;

        var dialogPad = GetHostedDialogPadding();
        HostedDialogSurface.Padding = new Thickness(0, dialogPad.Top, 0, spec.Footer is null ? dialogPad.Bottom : 0);

        var build = BuildHostedDialogChrome(spec, deferPrimaryClose: false, configure: null, showChildDialog: null);
        _hostedDialogHeader = build.Header;
        _hostedDialogScrollViewer = build.ScrollViewer;
        HostedDialogContent.Content = build.Chrome;
        _hostedDialogRequestedSpec = spec;
        UpdateHostedDialogVerticalInset();
        ApplyHostedDialogBounds(liveResize: false);
        ApplyHostedDialogTheme();
        UpdateHostedDialogSurfaceClip();
        ApplyHostedDialogBounds(liveResize: true);
        _hostedDialogBackdropDismissEnabled = true;

        var result = await childTcs.Task.ConfigureAwait(true);

        _hostedDialogBackdropDismissEnabled = false;
        RestoreHostedDialogFromSnapshot(snapshot);
        _hostedDialogTcs = snapshot.ParentTcs;
        _hostedDialogParentSnapshot = null;
        _hostedDialogBackdropDismissEnabled = snapshot.BackdropDismissEnabled;

        return result;
    }

    private void RestoreHostedDialogFromSnapshot(HostedDialogSnapshot snapshot)
    {
        HostedDialogContent.Content = snapshot.Chrome;
        _hostedDialogHeader = snapshot.Header;
        _hostedDialogRequestedSpec = snapshot.Spec;
        _hostedDialogScrollViewer = snapshot.ScrollViewer;
        HostedDialogSurface.Padding = snapshot.SurfacePadding;
        ApplyHostedDialogTheme();
        UpdateHostedDialogSurfaceClip();
        ApplyHostedDialogBounds(liveResize: true);
    }

    private HostedDialogChromeBuild BuildHostedDialogChrome(
        HostedDialogSpec spec,
        bool deferPrimaryClose,
        Action<HostedDialogHostContext>? configure,
        Func<HostedDialogSpec, Task<HostedDialogResult>>? showChildDialog)
    {
        var dialogPad = GetHostedDialogPadding();
        var header = new HostedDialogHeader { Title = spec.Title };
        header.CloseRequested += (_, _) => CloseHostedDialog(HostedDialogResult.Dismissed);
        header.Margin = new Thickness(dialogPad.Left, 0, dialogPad.Right, 4);

        HostedDialogLayout.ApplyBodyStretch(spec.Body);

        var chrome = new Grid
        {
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Stretch
        };
        chrome.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        chrome.RowDefinitions.Add(new RowDefinition
        {
            Height = spec.MaxHeight.HasValue
                ? new GridLength(1, GridUnitType.Star)
                : GridLength.Auto
        });

        Grid.SetRow(header, 0);
        chrome.Children.Add(header);

        var scrollViewer = new ScrollViewer
        {
            Content = spec.Body,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Stretch,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            VerticalScrollBarVisibility = spec.BodyHandlesScroll
                ? ScrollBarVisibility.Disabled
                : ScrollBarVisibility.Auto,
            Margin = new Thickness(0, 8, 0, 0)
        };
        Grid.SetRow(scrollViewer, 1);
        chrome.Children.Add(scrollViewer);

        Button primaryButton = new();
        HostedDialogHostContext? context = null;

        if (spec.Footer is { } footer)
        {
            chrome.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

            primaryButton = new Button
            {
                Content = footer.PrimaryButtonText,
                Width = footer.PrimaryButtonWidth,
                MinWidth = footer.PrimaryButtonWidth,
                MinHeight = 32,
                VerticalAlignment = VerticalAlignment.Center,
                IsEnabled = footer.IsPrimaryEnabled
            };

            if (Application.Current.Resources["AccentButtonStyle"] is Style accentStyle)
                primaryButton.Style = accentStyle;

            var footerButtons = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                Spacing = footer.ButtonSpacing,
                HorizontalAlignment = HorizontalAlignment.Right,
                VerticalAlignment = VerticalAlignment.Center
            };

            if (!string.IsNullOrEmpty(footer.SecondaryButtonText))
            {
                var secondaryButton = new Button
                {
                    Content = footer.SecondaryButtonText,
                    MinWidth = footer.SecondaryButtonWidth,
                    MinHeight = 32,
                    Padding = new Thickness(16, 5, 16, 5),
                    VerticalAlignment = VerticalAlignment.Center,
                    VerticalContentAlignment = VerticalAlignment.Center
                };
                if (Application.Current.Resources["GlassSettingsCommandBarButtonStyle"] is Style secondaryStyle)
                    secondaryButton.Style = secondaryStyle;
                // CommandBar-стиль даёт Padding 20,8 — в полосе 32px обрезает «Отмена» снизу.
                secondaryButton.Padding = new Thickness(16, 5, 16, 5);
                secondaryButton.MinHeight = 32;
                secondaryButton.VerticalContentAlignment = VerticalAlignment.Center;
                secondaryButton.Click += (_, _) => CloseHostedDialog(HostedDialogResult.Dismissed);
                footerButtons.Children.Add(secondaryButton);
            }

            footerButtons.Children.Add(primaryButton);

            var footerDivider = new Border
            {
                Height = 1,
                Margin = new Thickness(
                    spec.FooterDividerFullWidth ? 0 : dialogPad.Left,
                    spec.FooterDividerTopMargin,
                    spec.FooterDividerFullWidth ? 0 : dialogPad.Right,
                    0),
                Background = Application.Current.Resources["DividerStrokeColorDefaultBrush"] as Brush
            };

            const double footerButtonBand = 48;
            const double footerControlBand = 36;
            var footerChromeHeight = footerButtonBand + dialogPad.Bottom;

            var footerBand = new Grid
            {
                Height = footerControlBand,
                VerticalAlignment = VerticalAlignment.Center,
                HorizontalAlignment = HorizontalAlignment.Stretch
            };
            footerBand.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            footerBand.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

            var footerButtonsHost = new Grid
            {
                HorizontalAlignment = HorizontalAlignment.Right,
                VerticalAlignment = VerticalAlignment.Center
            };
            footerButtonsHost.Children.Add(footerButtons);
            footerBand.Children.Add(footerButtonsHost);
            Grid.SetColumn(footerButtonsHost, 1);

            var footerBar = new Grid
            {
                Height = footerChromeHeight,
                Margin = new Thickness(dialogPad.Left, 0, dialogPad.Right, 0)
            };
            footerBar.Children.Add(footerBand);

            var footerChrome = new StackPanel { Spacing = 0 };
            footerChrome.Children.Add(footerDivider);
            footerChrome.Children.Add(footerBar);
            Grid.SetRow(footerChrome, 2);
            chrome.Children.Add(footerChrome);

            if (deferPrimaryClose)
            {
                primaryButton.Click += (_, _) => OnHostedDialogPrimaryForProcedure();
            }
            else if (!spec.DeferPrimaryCloseToHost)
            {
                primaryButton.Click += (_, _) => CloseHostedDialog(HostedDialogResult.Primary);
            }

            context = new HostedDialogHostContext(
                primaryButton,
                footerBand,
                () => ApplyHostedDialogBounds(liveResize: true),
                CloseHostedDialog,
                showChildDialog);
        }
        else if (configure is not null)
        {
            // Footer-less hosted-диалог: programmatic close из body (Enter в compact input).
            primaryButton = new Button { Visibility = Visibility.Collapsed, Width = 0, Height = 0 };
            context = new HostedDialogHostContext(
                primaryButton,
                footerRow: null,
                () => ApplyHostedDialogBounds(liveResize: true),
                CloseHostedDialog,
                showChildDialog);
        }

        if (configure is not null && context is not null)
            configure(context);

        return new HostedDialogChromeBuild
        {
            Chrome = chrome,
            Header = header,
            ScrollViewer = scrollViewer,
            Context = context
        };
    }

    private sealed class HostedDialogSnapshot
    {
        public required Grid Chrome { get; init; }

        public required HostedDialogSpec Spec { get; init; }

        public HostedDialogHeader? Header { get; init; }

        public ScrollViewer? ScrollViewer { get; init; }

        public Thickness SurfacePadding { get; init; }

        public bool BackdropDismissEnabled { get; init; }

        public required TaskCompletionSource<HostedDialogResult> ParentTcs { get; init; }
    }

    private sealed class HostedDialogChromeBuild
    {
        public required Grid Chrome { get; init; }

        public required HostedDialogHeader Header { get; init; }

        public required ScrollViewer ScrollViewer { get; init; }

        public HostedDialogHostContext? Context { get; init; }
    }

    private void TryReloadPageAfterLanguageChange()
    {
        if (!_pendingUiLanguagePageReload || _nav.Trail.Count == 0)
            return;

        _pendingUiLanguagePageReload = false;
        var current = _nav.Trail[^1];
        NavFrame.Navigate(current.PageType, current.Parameter, new SuppressNavigationTransitionInfo());
    }

    private void OnHostedDialogPrimaryForProcedure()
    {
        HideHostedDialogPanelForProcedure();
        DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Normal, () =>
            _hostedDialogTcs?.TrySetResult(HostedDialogResult.Primary));
    }

    private void HideHostedDialogPanelForProcedure()
    {
        _hostedDialogBackdropDismissEnabled = false;
        HostedDialogHost.IsHitTestVisible = false;
        HostedDialogHost.Visibility = Visibility.Collapsed;
        HostedDialogContent.Content = null;
        _hostedDialogHeader = null;
        _hostedDialogRequestedSpec = null;
        _hostedDialogScrollViewer = null;
        HostedDialogRevealAnimator.ResetVisible(DialogShadowRect);
        ResetHostedDialogSurfacePadding();
        SetWindowBusyProgressVisible(true);
    }

    private void ApplyHostedDialogBounds(bool liveResize = false)
    {
        if (_hostedDialogRequestedSpec is null)
            return;

        var availableWidth = DialogBoundsHost.ActualWidth;
        var availableHeight = DialogBoundsHost.ActualHeight;
        if (availableWidth <= 0 || availableHeight <= 0)
        {
            var windowSize = AppWindow.Size;
            availableWidth = windowSize.Width;
            var titleBarHeight = Math.Max(0, AppTitleBar.ActualHeight);
            availableHeight = Math.Max(0, windowSize.Height - titleBarHeight);
        }

        var adaptiveMin = (_hostedDialogScrollViewer?.Content as IHostedDialogAdaptiveBody)?.GetHostedDialogMinWidth();

        var bounds = HostedDialogBoundsHelper.Compute(
            _hostedDialogRequestedSpec,
            availableWidth,
            availableHeight,
            adaptiveMin);

        DialogShadowRect.MinWidth = bounds.EffectiveMinWidth;

        DialogShadowRect.MaxWidth = bounds.EffectiveMaxWidth;
        DialogShadowRect.MaxHeight = bounds.EffectiveMaxHeight;
        DialogShadowRect.Width = bounds.EffectiveWidth;

        if (_hostedDialogRequestedSpec.CenterBottomMargin > 0)
            DialogShadowRect.Margin = new Thickness(0, 0, 0, _hostedDialogRequestedSpec.CenterBottomMargin);
        else
            DialogShadowRect.ClearValue(FrameworkElement.MarginProperty);

        if (double.IsNaN(bounds.EffectiveHeight))
            DialogShadowRect.ClearValue(FrameworkElement.HeightProperty);
        else
            DialogShadowRect.Height = bounds.EffectiveHeight;

        NotifyHostedDialogBodyBounds(bounds);
    }

    private void NotifyHostedDialogBodyBounds(HostedDialogEffectiveBounds bounds)
    {
        if (_hostedDialogScrollViewer?.Content is not IHostedDialogAdaptiveBody adaptive)
            return;

        var height = double.IsNaN(bounds.EffectiveHeight) ? 0 : bounds.EffectiveHeight;
        adaptive.OnHostedDialogBoundsChanged(bounds.EffectiveWidth, height);
    }

    private void OnDialogBoundsHostSizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (HostedDialogHost.Visibility != Visibility.Visible || _hostedDialogRequestedSpec is null)
            return;

        if (_hostedDialogBoundsUpdatePending)
            return;

        _hostedDialogBoundsUpdatePending = true;
        DispatcherQueue.TryEnqueue(() =>
        {
            _hostedDialogBoundsUpdatePending = false;
            if (HostedDialogHost.Visibility == Visibility.Visible && _hostedDialogRequestedSpec is not null)
            {
                UpdateHostedDialogSurfaceClip();
                ApplyHostedDialogBounds(liveResize: true);
            }
        });
    }

    private static Thickness GetHostedDialogPadding() =>
        Application.Current.Resources["ContentDialogPadding"] as Thickness? ?? new Thickness(24);

    private void ResetHostedDialogSurfacePadding()
    {
        HostedDialogSurface.Padding = GetHostedDialogPadding();
        HostedDialogSurface.Clip = null;
    }

    private void UpdateHostedDialogSurfaceClip()
    {
        var width = HostedDialogSurface.ActualWidth;
        var height = HostedDialogSurface.ActualHeight;
        if (width <= 0 || height <= 0)
        {
            HostedDialogSurface.Clip = null;
            return;
        }

        HostedDialogSurface.Clip = new RectangleGeometry
        {
            Rect = new Rect(0, 0, width, height)
        };
    }

    internal void ApplyHostedDialogTheme()
    {
        if (HostedDialogHost.Visibility != Visibility.Visible)
            return;

        AppThemeService.ApplyThemeToElement(HostedDialogHost);
        AppThemeService.ApplyThemeToElement(DialogShadowRect);
        AppThemeService.ApplyThemeToElement(HostedDialogSurface);
        if (HostedDialogContent.Content is FrameworkElement contentRoot)
            AppThemeService.ApplyThemeToElement(contentRoot);

        WindowBackdropService.ApplyAcrylicInAppChromeBackground(HostedDialogSurface);
    }

    internal void RefreshHostedDialogTitle(string title)
    {
        if (_hostedDialogHeader is not null)
            _hostedDialogHeader.Title = title;
    }

    internal void RefreshUiLanguage()
    {
        ApplyLocalizedStrings();
        RefreshNavLabels();
        _nav.RefreshTrailTitles();
        SyncBreadcrumb();

        var dialogOpen = HostedDialogHost.Visibility == Visibility.Visible;

        if (dialogOpen)
        {
            _pendingUiLanguagePageReload = true;
            AppSettingsDialogService.RefreshActiveDialog();
            _hostedDialogHeader?.ApplyLocalizedStrings();
            return;
        }

        if (_nav.Trail.Count > 0)
        {
            var current = _nav.Trail[^1];
            NavFrame.Navigate(current.PageType, current.Parameter, new SuppressNavigationTransitionInfo());
        }
    }

    private void RefreshNavLabels()
    {
        foreach (var (tag, labelKey) in NavLabelKeys())
        {
            if (!_navItems.TryGetValue(tag, out var item))
                continue;

            var label = GlassSettingsResources.Get(labelKey);
            _navLabels[tag] = label;

            if (item.Content is StackPanel { Children.Count: >= 2 } panel
                && panel.Children[1] is TextBlock textBlock)
            {
                textBlock.Text = label;
            }
        }
    }

    private static IEnumerable<(string Tag, string ResourceKey)> NavLabelKeys()
    {
        yield return ("home", "NavHome");
        yield return ("system", "NavSystem");
        yield return ("dwm", "NavDwm");
        yield return ("glass", "NavGlass");
        yield return ("attach", "NavAttach");
        yield return ("themes", "NavThemes");
        yield return ("text", "NavText");
    }

    private void CloseHostedDialog(HostedDialogResult result) =>
        _hostedDialogTcs?.TrySetResult(result);

    private void HostedDialogHost_OnPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        if (!_hostedDialogBackdropDismissEnabled)
            return;

        if (HostedDialogHost.Visibility != Visibility.Visible)
            return;

        if (e.OriginalSource is DependencyObject source
            && IsVisualDescendantOf(source, HostedDialogSurface))
        {
            return;
        }

        CloseHostedDialog(HostedDialogResult.Dismissed);
        e.Handled = true;
    }

    private static bool IsVisualDescendantOf(DependencyObject node, DependencyObject ancestor)
    {
        var current = node;
        while (current is not null)
        {
            if (ReferenceEquals(current, ancestor))
                return true;

            current = VisualTreeHelper.GetParent(current);
        }

        return false;
    }

    private void NavView_OnPaneClosed(NavigationView sender, object args) =>
        NavigationPaneChromeService.Apply(NavView, _layoutMode);

    private void InstallCloseSubclassWhenReady()
    {
        Activated += OnInstallCloseSubclassOnce;
        DispatcherQueue.TryEnqueue(TryInstallCloseSubclass);
    }

    private void OnInstallCloseSubclassOnce(object sender, WindowActivatedEventArgs args)
    {
        Activated -= OnInstallCloseSubclassOnce;
        TryInstallCloseSubclass();
    }

    private void TryInstallCloseSubclass()
    {
        if (_closeSubclassInstalled)
            return;

        var hwnd = WindowNative.GetWindowHandle(this);
        if (hwnd == IntPtr.Zero)
        {
            return;
        }

        _closeSubclassProc ??= OnCloseSubclassMessage;
        if (!Win32WindowSubclass.Install(hwnd, _closeSubclassProc, CloseSubclassId, out _closeSubclassFn))
        {
            return;
        }

        AllowActivateMessageFromLowerIl(hwnd);
        _closeSubclassInstalled = true;
    }

    /// <summary>
    /// Разрешает WM_GLASS_SETTINGS_ACTIVATE от процессов с более низким IL (explorer/ShellExt).
    /// Runtime: PostMessage без фильтра → FALSE (UIPI), окно остаётся iconic.
    /// </summary>
    private static void AllowActivateMessageFromLowerIl(IntPtr hwnd)
    {
        if (hwnd == IntPtr.Zero)
            return;
        _ = ChangeWindowMessageFilterEx(hwnd, WmGlassSettingsActivate, MsgfltAllow, IntPtr.Zero);
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool ChangeWindowMessageFilterEx(
        IntPtr hwnd,
        uint message,
        uint action,
        IntPtr pChangeFilterStruct);

    private IntPtr OnCloseSubclassMessage(
        IntPtr hWnd,
        uint uMsg,
        IntPtr wParam,
        IntPtr lParam,
        UIntPtr uIdSubclass,
        UIntPtr dwRefData)
    {
        if (uMsg == WmClose)
        {
            BeginAppCloseSequence();
            return IntPtr.Zero;
        }

        if (uMsg == WmGlassSettingsActivate)
        {
            _ = DispatcherQueue.TryEnqueue(BringMainWindowToForeground);
            return IntPtr.Zero;
        }

        return Win32WindowSubclass.CallDef(hWnd, uMsg, wParam, lParam);
    }

    /// <summary>
    /// In-process restore/activate для ShellExt и второго инстанса.
    /// Win32 ShowWindow на WinUI AppWindow часто no-op — нужен AppWindow/OverlappedPresenter.
    /// </summary>
    internal void BringMainWindowToForeground()
    {
        try
        {
            if (AppWindow.Presenter is OverlappedPresenter presenter
                && presenter.State == OverlappedPresenterState.Minimized)
            {
                presenter.Restore();
            }

            if (!AppWindow.IsVisible)
                AppWindow.Show();

            Activate();
        }
        catch
        {
            /* best-effort */
        }
    }

    private void OnAppWindowClosing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        if (_allowWindowClose)
            return;

        args.Cancel = true;
        BeginAppCloseSequence();
    }

    private void BeginAppCloseSequence()
    {
        if (_closeInProgress)
            return;

        _closeInProgress = true;
        SettingsSession.Current.BeginShutdown();
        NoticeViewportMirrorService.HideMirrorImmediate();
        _ = RunAppCloseSequenceAsync();
    }

    /// <summary>Close → штора → teardown / revert / native shutdown → мягкое закрытие окна → Exit.</summary>
    private async Task RunAppCloseSequenceAsync()
    {
        ShutdownPhaseTrace.Begin();

        if (OpenGlassSymbolDownloadService.Current.IsRunning)
        {
            var proceed = await OpenGlassSymbolDownloadDialogService.ConfirmCloseWhileDownloadingAsync()
                .ConfigureAwait(true);
            if (!proceed)
            {
                _closeInProgress = false;
                SettingsSession.Current.CancelShutdown();
                return;
            }

            await OpenGlassSymbolDownloadService.Current.CancelAndWaitAsync(TimeSpan.FromSeconds(15))
                .ConfigureAwait(true);
        }

        // Проверка несохранённых изменений перед показом шторы
        if (SettingsSession.Current.HasUnsavedChanges)
        {
            var confirmed = await UnsavedChangesExitDialogService.ConfirmExitAsync().ConfigureAwait(true);
            if (!confirmed)
            {
                // Пользователь отменил закрытие
                _closeInProgress = false;
                SettingsSession.Current.CancelShutdown();
                return;
            }
        }

        WindowBusyOverlayService.Push(showProgress: true);
        ShutdownPhaseTrace.Mark("curtain");

        try
        {
            await SettingsInteractionDeferral.RunAfterUiSettledAsync(DispatcherQueue).ConfigureAwait(true);

            _nav.TrailChanged -= OnTrailChanged;
            StopDesktopEnvironmentWatcher();
            ShutdownPhaseTrace.Mark("StopDesktopEnvironmentWatcher");
            StopRegistrySnapshotWatch();
            _preferencesWatcher?.Dispose();
            _preferencesWatcher = null;

            SavePlacement();
            ShutdownPhaseTrace.Mark("SavePlacement");

            try
            {
                var dirtyAtRevert = SettingsSession.Current.HasUnsavedChanges;
                if (dirtyAtRevert)
                {
                    var revertPlan = SettingsSession.Current.CaptureRevertOnExitPlan();
                    await Task.Run(() => SettingsSession.Current.RevertSnapshotOnly()).ConfigureAwait(true);
                    SettingsSession.Current.ApplyRevertOnExitNotifications(revertPlan);
                    ShutdownPhaseTrace.Mark("RevertOnExit");
                }
            }
            catch (Exception ex)
            {
                ShutdownPhaseTrace.Mark($"RevertOnExitFailed:{ex.GetType().Name}");
            }

            if (NativeHostInterop.IsGlassRuntimeLoaded())
            {
                NativeHostInterop.ShutdownGlassRuntime();
                ShutdownPhaseTrace.Mark("ShutdownGlassRuntime");
            }

            ShutdownPhaseTrace.Mark("HideAppWindow");
            AppWindow.Hide();

            PrepareForProcessExit();
            ShutdownPhaseTrace.Mark("PrepareForProcessExit");

            FinishAppCloseSequence();
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings close sequence failed: {ex}");
            WindowBusyOverlayService.ForceResetIfActive();
            _closeInProgress = false;
        }
    }

    private void FinishAppCloseSequence()
    {
        UnhookWindowCloseInterception();
        _allowWindowClose = true;
        SingleInstance.ReleaseForExit();

        // Окно уже скрыто — жёсткий Exit без «улёта» в угол; Application.Exit оставляет процесс на 5–10 с.
        ShutdownPhaseTrace.Mark("before Environment.Exit");
        Environment.Exit(0);
    }

    private void UnhookWindowCloseInterception()
    {
        AppWindow.Closing -= OnAppWindowClosing;

        if (!_closeSubclassInstalled)
            return;

        var hwnd = WindowNative.GetWindowHandle(this);
        if (hwnd != IntPtr.Zero && _closeSubclassFn != IntPtr.Zero)
        {
            Win32WindowSubclass.Remove(hwnd, _closeSubclassFn, CloseSubclassId);
            _closeSubclassFn = IntPtr.Zero;
        }

        _closeSubclassInstalled = false;
    }

    /// <summary>Отпускает тяжёлое XAML-дерево до teardown процесса.</summary>
    private void PrepareForProcessExit()
    {
        if (_hostedDialogTcs is not null)
            CloseHostedDialog(HostedDialogResult.Dismissed);

        _pages.Clear();
        _home = null;
        _attachIconPreload = null;
        NavFrame.Content = null;
    }

    /// <summary>Полнооконная штора (in-app acrylic blur) на время XAML-диалога или загрузки.</summary>
    public bool IsWindowBusyOverlayActive => WindowBusyOverlay.Visibility == Visibility.Visible;

    public void SetWindowBusyProgressVisible(bool visible) =>
        WindowBusyProgressRing.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;

    public void PushWindowBusyOverlay(bool showProgress = true)
    {
        if (_windowBusyDepth++ > 0)
            return;

        WindowBusyOverlay.Visibility = Visibility.Visible;
        WindowBusyProgressRing.Visibility = showProgress ? Visibility.Visible : Visibility.Collapsed;
        ApplyWindowBusyOverlayFill();
    }

    public void PopWindowBusyOverlay()
    {
        if (_windowBusyDepth <= 0)
            return;

        if (--_windowBusyDepth > 0)
            return;

        WindowBusyOverlay.Visibility = Visibility.Collapsed;
        WindowBusyOverlayService.RaiseOverlayBecameIdle();
    }

    /// <summary>После hosted-диалога / шторы — sync session notice с финальным dirty.</summary>
    internal void RefreshActivePageSessionNoticeIfDirty()
    {
        if (TryGetActivePageShell() is { } shell)
            shell.RefreshSessionNoticeProgrammatic();
    }

    internal static SettingsPageShell? TryGetActivePageShell(Page? page)
    {
        if (page?.Content is SettingsPageShell shell)
            return shell;

        return null;
    }

    private SettingsPageShell? TryGetActivePageShell()
    {
        if (NavFrame.Content is SettingsPageShell shellDirect)
            return shellDirect;

        if (NavFrame.Content is Page page)
            return TryGetActivePageShell(page);

        return null;
    }

    private void HookWindowChrome()
    {
        AppWindow.Changed += (_, args) =>
        {
            if (args.DidSizeChange)
                ApplyLayoutMode(GetLayoutWidth());
        };
        AppTitleBar.SizeChanged += (_, _) => UpdateHostedDialogVerticalInset();
        DialogBoundsHost.SizeChanged += OnDialogBoundsHostSizeChanged;
        RootGrid.SizeChanged += (_, _) => ApplyLayoutMode(GetLayoutWidth());
        RootGrid.Loaded += RootGrid_OnLoaded;
    }

    /// <summary>Верхняя граница зоны hosted-диалога — нижняя кромка TitleBar.</summary>
    private void UpdateHostedDialogVerticalInset()
    {
        var top = Math.Max(0, AppTitleBar.ActualHeight);
        HostedDialogHost.Margin = new Thickness(0, top, 0, 0);

        if (HostedDialogHost.Visibility == Visibility.Visible && _hostedDialogRequestedSpec is not null)
            ApplyHostedDialogBounds(liveResize: true);
    }

    private void RootGrid_OnLoaded(object sender, RoutedEventArgs e)
    {
        if (RootGrid.XamlRoot != null)
            RootGrid.XamlRoot.Changed += (_, _) => ApplyLayoutMode(GetLayoutWidth());
        UpdateHostedDialogVerticalInset();
        ApplyLayoutMode(GetLayoutWidth());
    }

    private double GetLayoutWidth()
    {
        var w = AppWindow.Size.Width;
        return w > 0 ? w : RootGrid.ActualWidth;
    }

    private void ApplyLayoutMode(double width)
    {
        var mode = SettingsWindowLayout.ModeFromWidth(width);
        var prev = _layoutMode;
        var modeChanged = mode != _layoutMode;
        _layoutMode = mode;

        if (modeChanged)
        {
            switch (mode)
            {
                case SettingsLayoutMode.Narrow:
                    if (prev != SettingsLayoutMode.Narrow)
                        NavView.IsPaneOpen = false;
                    NavView.PaneDisplayMode = NavigationViewPaneDisplayMode.LeftMinimal;
                    NavView.IsPaneOpen = false;
                    AppTitleBar.IsPaneToggleButtonVisible = true;
                    break;

                case SettingsLayoutMode.Medium:
                    if (prev == SettingsLayoutMode.Narrow)
                        NavView.IsPaneOpen = false;
                    NavView.PaneDisplayMode = NavigationViewPaneDisplayMode.Left;
                    NavView.IsPaneOpen = true;
                    AppTitleBar.IsPaneToggleButtonVisible = false;
                    break;

                case SettingsLayoutMode.Wide:
                    if (prev == SettingsLayoutMode.Narrow)
                        NavView.IsPaneOpen = false;
                    NavView.PaneDisplayMode = NavigationViewPaneDisplayMode.Left;
                    NavView.IsPaneOpen = true;
                    AppTitleBar.IsPaneToggleButtonVisible = false;
                    break;
            }
        }

        ApplySearchPlacement();
        ApplyPageContentHostLayout(mode);
        if (modeChanged)
            SyncNavigationUserCardLayout();
    }

    private void ApplySearchPlacement()
    {
        var changed = NavigationSearchLayout.Apply(
            _layoutMode,
            NavView.IsPaneOpen,
            SearchBox,
            AppTitleBar,
            NavigationPaneSearchHost);
        if (changed)
            RefreshTitleBarDragRegions();
    }

    private void RefreshTitleBarDragRegions()
    {
        try
        {
            AppTitleBar.RecomputeDragRegions();
        }
        catch { /* TitleBar API может быть недоступен */ }
    }

    private void ApplyPageContentHostLayout(SettingsLayoutMode mode)
    {
        SettingsContentColumnLayout.SetWideCentered(mode == SettingsLayoutMode.Wide);
        ClearPageContentHostConstraints();
        SettingsContentColumnLayout.NotifyViewportChanged();
        HomePageColumnsLayout.NotifyViewportChanged();
    }

    private void ClearPageContentHostConstraints()
    {
        PageContentHost.ClearValue(FrameworkElement.WidthProperty);
        PageContentHost.ClearValue(FrameworkElement.MinWidthProperty);
        PageContentHost.ClearValue(FrameworkElement.MaxWidthProperty);
        PageContentHost.ClearValue(FrameworkElement.HorizontalAlignmentProperty);
    }

    private static void ApplyWindowMinSizeConstraints(AppWindow appWindow)
    {
        if (appWindow.Presenter is not OverlappedPresenter presenter)
            return;

        presenter.PreferredMinimumWidth = WindowPlacementStore.MinWindowW;
        presenter.PreferredMinimumHeight = WindowPlacementStore.MinWindowH;
    }

    private void EnforceMinWindowSize()
    {
        var size = AppWindow.Size;
        var w = Math.Max(size.Width, WindowPlacementStore.MinWindowW);
        var h = Math.Max(size.Height, WindowPlacementStore.MinWindowH);
        if (w != size.Width || h != size.Height)
            AppWindow.Resize(new Windows.Graphics.SizeInt32(w, h));
    }

    private void BuildNavigation()
    {
        NavView.MenuItems.Clear();
        AddNav("home", GlassSettingsResources.Get("NavHome"), "\uE80F");
        AddNav("system", GlassSettingsResources.Get("NavSystem"), "\uE713");
        AddNav("dwm", GlassSettingsResources.Get("NavDwm"), "\uE771");
        AddNav("glass", GlassSettingsResources.Get("NavGlass"), "\uE943");
        AddNav("themes", GlassSettingsResources.Get("NavThemes"), "\uE790");
        AddNav("text", GlassSettingsResources.Get("NavText"), "\uE8D2");
        AddNav("attach", GlassSettingsResources.Get("NavAttach"), "\uE77B");
        NavView.ItemInvoked += NavView_OnItemInvoked;
        ApplyOpenGlassNavVisibility();
    }

    private void OnOpenGlassUiPolicyChanged(object? sender, EventArgs e) =>
        DispatcherQueue.TryEnqueue(ApplyOpenGlassNavVisibility);

    private void ApplyOpenGlassNavVisibility()
    {
        if (!_navItems.TryGetValue("dwm", out var dwmItem))
            return;

        var exposed = OpenGlassUiPolicy.IsFeatureExposedInUi();
        dwmItem.Visibility = exposed ? Visibility.Visible : Visibility.Collapsed;

        if (exposed)
            return;

        if (string.Equals(_currentTag, "dwm", StringComparison.Ordinal)
            || string.Equals(_nav.CurrentSidebarTag, "dwm", StringComparison.Ordinal))
        {
            NavigateTo("home", force: true);
            SelectNavItem("home");
        }
    }

    private static bool IsDwmNavAvailable() => OpenGlassUiPolicy.IsFeatureExposedInUi();

    private void NavView_OnItemInvoked(NavigationView sender, NavigationViewItemInvokedEventArgs args)
    {
        if (_suppressNav || args.IsSettingsInvoked)
            return;

        if (args.InvokedItemContainer is not NavigationViewItem { Tag: string tag })
            return;

        if (!_shellInputArmed)
            return;

        NavigateTo(tag, force: true);
        if (_layoutMode == SettingsLayoutMode.Narrow)
            NavView.IsPaneOpen = false;
    }

    private void AddNav(string tag, string label, string glyph)
    {
        var dot = new UnsavedIndicatorDot();
        var labelBlock = new TextBlock
        {
            Text = label,
            VerticalAlignment = VerticalAlignment.Center
        };
        var content = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 6,
            VerticalAlignment = VerticalAlignment.Center,
            Children = { dot, labelBlock }
        };
        var item = new NavigationViewItem
        {
            Content = content,
            Tag = tag,
            Icon = new FontIcon { Glyph = glyph }
        };
        NavView.MenuItems.Add(item);
        _navItems[tag] = item;
        _navLabels[tag] = label;
        _navUnsavedDots[tag] = dot;
    }

    internal bool TryGetNavUnsavedDot(string tag, out UnsavedIndicatorDot dot) =>
        _navUnsavedDots.TryGetValue(tag, out dot!);

    private void RefreshNavUnsavedIndicators() =>
        SettingsUnsavedIndicatorCoordinator.RefreshNav(this);

    private void SelectNavItem(string tag)
    {
        if (!_navItems.TryGetValue(tag, out var item)) return;
        _suppressNav = true;
        NavView.SelectedItem = item;
        _suppressNav = false;
        UpdateBackButton();
    }

    private void NavFrame_OnNavigated(object sender, NavigationEventArgs e)
    {
        var tag = e.Parameter as string ?? _currentTag;
        NoticeMirrorTrace.Write($"NavFrame_OnNavigated: tag={tag}, page={e.Content?.GetType().Name}");
        if (!_suppressHomeNavigatedFrom && e.Content is not HomePage && _home is not null)
            _home.OnNavigatedFrom();
        NoticeViewportMirrorService.OnPageNavigated();
        WindowBusyOverlayService.ForceResetIfActive();
        if (e.Content is not Page page) return;
        WirePage(tag, page);
        SettingsPageShell.NotifyPageNavigatedTo(page);
        _pages[tag] = page;
        SyncBreadcrumb();
        _navigationReady = true;
        // Dismiss — только после MarkShellInteractiveReady (Home Refresh/Presence/Ensure под заставкой).
    }

    private void ScheduleDismissLoadingOverlay(bool fallback)
    {
        if (_contentReady)
            return;

        _ = DismissLoadingOverlayWhenReadyAsync(fallback);
    }

    private async Task DismissLoadingOverlayWhenReadyAsync(bool fallback)
    {
        try
        {
            if (_contentReady)
                return;

            if (!fallback)
            {
                while (!_shellInteractiveReady)
                {
                    await Task.Delay(50);
                    if (_contentReady)
                        return;
                }
            }
            else
            {
                while (!_navigationReady)
                {
                    await Task.Delay(50);
                    if (_contentReady)
                        return;
                }
            }

            var elapsed = (DateTimeOffset.UtcNow - _loadingShownAt).TotalMilliseconds;
            var minDelayMs = Math.Max(0, (int)(LoadingMinimumMs - elapsed));
            var minDelay = Task.Delay(minDelayMs);

            // Names-only (без native SHGetFileInfo) + обои + composition blur — под заставкой.
            // Раньше wait на icon-preload (~600 ms) маскировал готовность DesktopWallpaperImageCache;
            // после names-only dismiss шёл сразу → пустой слот превью на Главной.
            _attachIconPreload ??= ProcessIconResolver.PreloadAttachListNamesOnlyAsync();
            if (CompositionWallpaperLayersCache.ShouldPrewarm())
                CompositionWallpaperLayersCache.BeginPrewarm(DispatcherQueue);

            const int maxPreviewWaitMs = 800;
            var wallpaperTask = DesktopWallpaperImageCache.LoadTask ?? Task.CompletedTask;
            var wallpaperOrTimeout = Task.WhenAny(wallpaperTask, Task.Delay(maxPreviewWaitMs));
            var compositionTask = CompositionWallpaperLayersCache.PrewarmTask ?? Task.CompletedTask;
            var compositionOrTimeout = Task.WhenAny(compositionTask, Task.Delay(maxPreviewWaitMs));

            // x86: BitmapImage ImageOpened часто после wallpaper — иначе dismiss без иконки.
            const int maxSplashIconWaitMs = 1500;
            var splashIconOrTimeout = Task.WhenAny(
                _splashIconReadyTcs.Task,
                Task.Delay(maxSplashIconWaitMs));

            await Task.WhenAll(
                    minDelay,
                    wallpaperOrTimeout,
                    compositionOrTimeout,
                    _attachIconPreload,
                    splashIconOrTimeout)
                .ConfigureAwait(true);

            if (_contentReady)
                return;

            // Не вызывать TryDismiss из continuation Delay(0) внутри OnNavigated — иначе нет заставки.
            DispatcherQueue.TryEnqueue(() =>
            {
                if (_contentReady)
                    return;
                if (NavFrame.Content is HomePage home)
                    home.RefreshWallpaperPreviewUnderSplash();
                // До TryDismiss: прогреть Processes, пока виден acrylic splash.
                WarmAttachListPageIfReady();
                TryDismissLoadingOverlay();
            });
        }
        catch
        {
            /* dismiss best-effort */
        }
    }

    /// <summary>Создаёт и наполняет ProcessesPage под заставкой — переход потом мгновенный.</summary>
    private void WarmAttachListPageIfReady()
    {
        if (!ProcessIconResolver.IsPreloadComplete)
        {
            return;
        }

        _suppressHomeNavigatedFrom = true;
        try
        {
            NavFrame.Navigate(typeof(ProcessesPage), "attach", new SuppressNavigationTransitionInfo());
            if (string.Equals(_currentTag, "home", StringComparison.Ordinal))
            {
                NavFrame.Navigate(typeof(HomePage), "home", new SuppressNavigationTransitionInfo());
            }
        }
        catch { /* ignore */ }
        finally
        {
            _suppressHomeNavigatedFrom = false;
        }
    }

    private async Task LoadingOverlayFallbackAsync()
    {
        await Task.Delay(8000);
        if (_contentReady)
            return;

        _navigationReady = true;
        _shellInteractiveReady = true;
        await DismissLoadingOverlayWhenReadyAsync(fallback: true);
    }

    private void OnTrailChanged(object? sender, EventArgs e)
    {
        // x86: sync TrailBreadcrumb.ItemsSource внутри ResetTrail (до Frame.Navigate) → AV.
        // Обновляем на следующем кадре dispatcher после навигации.
        DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Normal, () =>
        {
            SyncBreadcrumb();
            UpdateBackButton();
        });
    }

    private void SyncBreadcrumb()
    {
        if (_nav.Trail.Count < 1)
        {
            TrailBreadcrumb.ItemsSource = null;
            TrailColumn.Visibility = Visibility.Collapsed;
            return;
        }

        // Снимок списка: прямая привязка к mutable Trail ломает BreadcrumbBar при Add/Remove.
        var items = _nav.Trail.ToList();
        TrailBreadcrumb.ItemsSource = items;
        TrailColumn.Visibility = Visibility.Visible;
    }

    private void TrailBreadcrumb_OnItemClicked(BreadcrumbBar sender, BreadcrumbBarItemClickedEventArgs args) =>
        _nav.NavigateToIndex(args.Index);

    private void WirePage(string tag, Page page)
    {
        if (page is PlaceholderPage ph)
            ph.SetHeader(_navLabels.TryGetValue(tag, out var label) ? label : tag, GlassSettingsResources.Get("Placeholder_Subtitle"));
        if (page is HomePage hp)
        {
            _home = hp;
            hp.NavigateRequested -= Home_OnNavigateRequested;
            hp.NavigateRequested += Home_OnNavigateRequested;
            hp.OnNavigatedTo();
            if (HomeSessionNoticeState.StartupNoticePending)
                hp.TryScheduleStartupNotice();
            hp.SyncSummaryFooterFromSession();
        }

    }

    internal bool IsCurrentHomePage(HomePage page) =>
        ReferenceEquals(NavFrame.Content, page);

    internal void TryScheduleHomeStartupNotice()
    {
        if (NavFrame.Content is HomePage hp)
            hp.TryScheduleStartupNotice();
    }

    private void Home_OnNavigateRequested(object? sender, HomeNavTarget target)
    {
        if (!_shellInputArmed)
            return;

        NavigateFromHome(target);
    }

    private void NavigateFromHome(HomeNavTarget target)
    {
        switch (target.Kind)
        {
            case HomeNavKind.SectionRoot:
                if (target.SectionTag is not { } tag || !_navItems.ContainsKey(tag))
                    return;
                if (string.Equals(tag, "dwm", StringComparison.Ordinal) && !IsDwmNavAvailable())
                    return;
                NavigateTo(tag, force: true);
                SelectNavItem(tag);
                return;

            case HomeNavKind.GlassDrill:
                _nav.RememberForBack("home");
                NavigateHomeDrill(target, typeof(GlassSettingsHubPage), GlassSettingsResources.Get("Breadcrumb_GlassSettings"));
                return;

            case HomeNavKind.DwmDrill:
                if (!IsDwmNavAvailable())
                    return;
                _nav.RememberForBack("home");
                NavigateHomeDrill(target, typeof(DwmSettingsHubPage), GlassSettingsResources.Get("Breadcrumb_DwmSettings"));
                return;

            case HomeNavKind.RootWindows:
                _nav.RememberForBack("home");
                NavigateHomeRootWindows();
                return;
        }
    }

    private void NavigateHomeDrill(HomeNavTarget target, Type hubType, string hubTitle)
    {
        var sidebar = target.SidebarTag ?? "glass";
        if (!_navItems.ContainsKey(sidebar))
            return;

        _currentTag = sidebar;
        _nav.CurrentSidebarTag = sidebar;
        SelectNavItem(sidebar);
        var hubParameter = ResolveHomeDrillHubParameter(hubType, target.Parameter);
        _nav.NavigateHomeDrill(hubType, hubTitle, hubParameter, target.PageType, target.Parameter, target.BreadcrumbTitle);

        UpdateBackButton();
    }

    private static object? ResolveHomeDrillHubParameter(Type hubType, object? detailParameter)
    {
        if (hubType != typeof(DwmSettingsHubPage))
            return null;

        var detailContext = DwmNavigationContext.FromNavigationParameter(detailParameter);
        return DwmNavigationContext.ForHub(detailContext.Hive);
    }

    private void NavigateHomeRootWindows()
    {
        if (!_navItems.ContainsKey("glass"))
            return;

        _currentTag = "glass";
        _nav.CurrentSidebarTag = "glass";
        SelectNavItem("glass");

        var hubTitle = GlassSettingsResources.Get("Breadcrumb_GlassSettings");
        var detailTitle = GlassSettingsResources.Get("Breadcrumb_RootSwca");

        _nav.NavigateHomeDrill(typeof(GlassSettingsHubPage), hubTitle, null, typeof(RootWindowsPage), null, detailTitle);

        UpdateBackButton();
    }

    internal void NavigateToAttachPage()
    {
        NavigateTo("attach", force: true);
        SelectNavItem("attach");
    }

    private string? _textScrollSectionOverride;

    /// <param name="scrollSection">
    /// Опционально — id секции на <see cref="TextAndFontsPage"/> (см. <see cref="TextPageNavIds"/>).
    /// </param>
    internal void NavigateToTextPage(string? scrollSection = null)
    {
        // Уже на корне «Текст и шрифты» — не пересоздаём страницу, только скролл.
        try
        {
            if (string.Equals(_currentTag, "text", StringComparison.Ordinal)
                && !_nav.IsDrillActive
                && NavFrame.Content is TextAndFontsPage textPage)
            {
                _currentTag = "text";
                _nav.CurrentSidebarTag = "text";
                SelectNavItem("text");
                if (!string.IsNullOrEmpty(scrollSection))
                    textPage.RequestScrollToSection(scrollSection);
                UpdateBackButton();
                return;
            }
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings NavigateToTextPage same-tab: {ex}");
        }

        _textScrollSectionOverride = scrollSection;
        NavigateTo("text", force: true);
        _textScrollSectionOverride = null;
        SelectNavItem("text");
    }

    internal void NavigateToDwmSettings()
    {
        if (!IsDwmNavAvailable())
            return;

        NavigateTo("dwm", force: true);
        SelectNavItem("dwm");
    }

    /// <summary>Визуальные стили → Цвета (breadcrumb drill).</summary>
    internal void NavigateToThemesColorsPage()
    {
        try
        {
            if (string.Equals(_currentTag, "themes", StringComparison.Ordinal)
                && NavFrame.Content is ThemesColorsPage)
            {
                UpdateBackButton();
                return;
            }
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings NavigateToThemesColorsPage: {ex}");
        }

        if (!string.Equals(_currentTag, "themes", StringComparison.Ordinal) || _nav.IsDrillActive)
        {
            NavigateTo("themes", force: true);
            SelectNavItem("themes");
        }

        _nav.CurrentSidebarTag = "themes";
        _currentTag = "themes";
        SelectNavItem("themes");
        _nav.Push(
            typeof(ThemesColorsPage),
            null,
            GlassSettingsResources.Get("Breadcrumb_ThemesColors"));
        UpdateBackButton();
    }

    /// <summary>Визуальные стили → Фон рабочего стола (breadcrumb drill).</summary>
    internal void NavigateToThemesDesktopBackgroundPage()
    {
        try
        {
            if (string.Equals(_currentTag, "themes", StringComparison.Ordinal)
                && NavFrame.Content is ThemesDesktopBackgroundPage)
            {
                UpdateBackButton();
                return;
            }
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings NavigateToThemesDesktopBackgroundPage: {ex}");
        }

        if (!string.Equals(_currentTag, "themes", StringComparison.Ordinal) || _nav.IsDrillActive)
        {
            NavigateTo("themes", force: true);
            SelectNavItem("themes");
        }

        _nav.CurrentSidebarTag = "themes";
        _currentTag = "themes";
        SelectNavItem("themes");
        _nav.Push(
            typeof(ThemesDesktopBackgroundPage),
            null,
            GlassSettingsResources.Get("Breadcrumb_ThemesDesktopBackground"));
        UpdateBackButton();
    }

    internal void NavigateHomeDwmReflectionPicker()
    {
        if (!IsDwmNavAvailable())
            return;

        _nav.RememberForBack(_currentTag);
        _currentTag = "dwm";
        _nav.CurrentSidebarTag = "dwm";
        SelectNavItem("dwm");
        var detailContext = DwmNavigationContext.ForDetail(DwmRegistryHive.Hkcu, DwmRouteIds.Theme);
        _nav.NavigateHomeDrill(
            typeof(DwmSettingsHubPage),
            GlassSettingsResources.Get("Breadcrumb_DwmSettings"),
            DwmNavigationContext.ForHub(DwmRegistryHive.Hkcu),
            typeof(DwmThemeDetailPage),
            detailContext,
            GlassSettingsResources.Get("Breadcrumb_DwmTheme"));
        UpdateBackButton();
    }

    private void NavigateTo(string tag, bool force = false)
    {

        if (string.Equals(tag, "dwm", StringComparison.Ordinal) && !IsDwmNavAvailable())
            tag = "home";

        var tagChanging = !string.Equals(_currentTag, tag, StringComparison.Ordinal);
        var sameTab = !tagChanging;
        var rootPageType = TagToPageType(tag);

        object? frameContent = null;
        try
        {
            // Один раз читаем Content — повторные обращения к NavFrame.Content на x86 давали AV.
            frameContent = NavFrame.Content;
        }
        catch
        {
            throw;
        }

        // Повторный клик по уже выбранному пункту боковой панели: корневая страница раздела
        // уже открыта — не перенавигировать (без slide-анимации и лишнего Navigated).
        if (sameTab
            && !_nav.IsDrillActive
            && frameContent?.GetType() == rootPageType)
        {
            _currentTag = tag;
            _nav.CurrentSidebarTag = tag;
            if (frameContent is HomePage hp)
                hp.OnNavigatedTo();
            UpdateBackButton();
            return;
        }

        var willNavigate = force || tagChanging || frameContent is not Page;

        if (willNavigate && _initialNavDone && tagChanging)
            _nav.RememberForBack(_currentTag);

        _currentTag = tag;
        _nav.CurrentSidebarTag = tag;

        var rootParam = RootParameterForTag(tag);
        var rootTitle = NavTitleForTag(tag);
        _nav.NavigateToRoot(rootPageType, rootParam, rootTitle);
        UpdateBackButton();
    }

    private object? RootParameterForTag(string tag)
    {
        if (string.Equals(tag, DwmRouteIds.Hub, StringComparison.OrdinalIgnoreCase))
            return DwmNavigationContext.ForHub(DwmRegistryHive.Hkcu);

        if (string.Equals(tag, "text", StringComparison.Ordinal)
            && !string.IsNullOrEmpty(_textScrollSectionOverride))
        {
            return _textScrollSectionOverride;
        }

        return tag;
    }

    private static string NavTitleForTag(string tag) => tag switch
    {
        "home" => GlassSettingsResources.Get("NavHome"),
        "system" => GlassSettingsResources.Get("NavSystem"),
        "dwm" => GlassSettingsResources.Get("NavDwm"),
        "glass" => GlassSettingsResources.Get("NavGlass"),
        "attach" => GlassSettingsResources.Get("NavAttach"),
        "themes" => GlassSettingsResources.Get("NavThemes"),
        "text" => GlassSettingsResources.Get("NavText"),
        _ => tag
    };

    private void UpdateBackButton() =>
        TitleBarBackButton.IsEnabled = _nav.CanGoBackGlobal;

    private static Type TagToPageType(string tag) => tag switch
    {
        "home" => typeof(HomePage),
        "system" => typeof(SystemPage),
        "dwm" => typeof(DwmSettingsHubPage),
        "glass" => typeof(GlassSettingsHubPage),
        "flyout" => typeof(GlassSettingsHubPage),
        "taskbar" => typeof(TaskbarFlyoutPage),
        "attach" => typeof(ProcessesPage),
        "themes" => typeof(VisualStylesPage),
        "text" => typeof(TextAndFontsPage),
        _ => typeof(PlaceholderPage)
    };

    private void SearchBox_OnQuerySubmitted(AutoSuggestBox sender, AutoSuggestBoxQuerySubmittedEventArgs args)
    {
        if (string.IsNullOrWhiteSpace(args.QueryText)) return;
        var q = args.QueryText.Trim();
        if (q.Contains("dwm", StringComparison.OrdinalIgnoreCase) ||
            q.Contains("colorization", StringComparison.OrdinalIgnoreCase) ||
            q.Contains("open glass", StringComparison.OrdinalIgnoreCase) ||
            q.Contains("openglass", StringComparison.OrdinalIgnoreCase))
        {
            if (IsDwmNavAvailable())
            {
                NavigateTo("dwm", force: true);
                SelectNavItem("dwm");
            }
        }
        else if (q.Contains("glass", StringComparison.OrdinalIgnoreCase) ||
                 q.Contains("popup", StringComparison.OrdinalIgnoreCase) ||
                 q.Contains("taskbar", StringComparison.OrdinalIgnoreCase))
        {
            NavigateTo("glass", force: true);
            SelectNavItem("glass");
        }
        else if (q.Contains("стил", StringComparison.OrdinalIgnoreCase) ||
                 q.Contains("theme", StringComparison.OrdinalIgnoreCase) ||
                 q.Contains("visual style", StringComparison.OrdinalIgnoreCase))
        {
            NavigateTo("themes", force: true);
            SelectNavItem("themes");
        }
        else if (q.Contains("process", StringComparison.OrdinalIgnoreCase) ||
                 q.Contains("attach", StringComparison.OrdinalIgnoreCase) ||
                 q.Contains("процесс", StringComparison.OrdinalIgnoreCase))
        {
            NavigateTo("attach", force: true);
            SelectNavItem("attach");
        }
    }

    private void HookTitleBarTheme()
    {
        NavView.ActualThemeChanged += (_, _) =>
        {
            TitleBarThemeHelper.Apply(this, extendsContentIntoTitleBar: true);
            UpdateBackButton();
        };
    }

    private void StartDesktopEnvironmentWatcher()
    {
        _desktopEnvironmentWatcher = new DesktopEnvironmentWatcher(DispatcherQueue);
        _desktopEnvironmentWatcher.Attach(this);
        _desktopEnvironmentWatcher.Changed += OnDesktopEnvironmentChanged;
    }

    private void StopDesktopEnvironmentWatcher()
    {
        _themeBaselineReloadTimer?.Stop();

        if (_desktopEnvironmentWatcher is null)
            return;

        _desktopEnvironmentWatcher.Changed -= OnDesktopEnvironmentChanged;
        _desktopEnvironmentWatcher.Dispose();
        _desktopEnvironmentWatcher = null;
    }

    private void StartRegistrySnapshotWatch()
    {
        StopRegistrySnapshotWatch();
        _registrySnapshotWatch = new RegistrySnapshotWatchService(
            DispatcherQueue,
            SettingsSession.Current.RegistrySnapshot);
        _registrySnapshotWatch.Attach();
    }

    private void StopRegistrySnapshotWatch()
    {
        _registrySnapshotWatch?.Detach();
        _registrySnapshotWatch?.Dispose();
        _registrySnapshotWatch = null;
    }

    private void OnDesktopEnvironmentChanged(object? sender, DesktopEnvironmentChangedEventArgs e)
    {
        try
        {
            if (e.WallpaperChanged)
            {
                DesktopWallpaperImageCache.NotifyWallpaperChanged(DispatcherQueue);
                RecentWallpaperThumbCache.NotifyHistoryChanged(DispatcherQueue);
                CompositionWallpaperLayersCache.InvalidateAndPrewarmIfNeeded(DispatcherQueue);
            }

            if (e.AccentChanged)
                CompositionWallpaperLayersCache.Invalidate();

            if (e.ThemeChanged)
            {
                // Сразу suppress: host ещё пишет DWM (300 ms debounce) — не жёлтое Save.
                ThemeDwmBaselineSync.BeginThemeChangeWindow();
                ScheduleThemeBaselineReload();
            }

            if (e.ThemeChanged || e.AccentChanged)
                SyncWindowThemeFromDesktop();
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings OnDesktopEnvironmentChanged: {ex}");
        }
    }

    /// <summary>
    /// Переснимок DWM baseline после смены темы (Параметры Windows / плитка GS / host policy).
    /// Ждём стабилизации fingerprint (анти-гонка с WM_THEMECHANGED host), без жёлтого Save.
    /// </summary>
    private void ScheduleThemeBaselineReload()
    {
        if (SettingsSession.Current.IsShuttingDown)
            return;

        _themeBaselineReloadTimer ??= DispatcherQueue.CreateTimer();
        // Host policy debounce 300 ms + запас на Phase1–3; settle fingerprint — внутри Sync.
        _themeBaselineReloadTimer.Interval = TimeSpan.FromMilliseconds(
            ThemeDwmBaselineSync.HostPolicySettleDelayMs);
        _themeBaselineReloadTimer.IsRepeating = false;
        _themeBaselineReloadGeneration++;
        _themeBaselineReloadTimer.Tick -= OnThemeBaselineReloadTick;
        _themeBaselineReloadTimer.Tick += OnThemeBaselineReloadTick;
        _themeBaselineReloadTimer.Stop();
        _themeBaselineReloadTimer.Start();
    }

    private async void OnThemeBaselineReloadTick(DispatcherQueueTimer sender, object args)
    {
        sender.Tick -= OnThemeBaselineReloadTick;
        if (SettingsSession.Current.IsShuttingDown)
            return;

        var generation = _themeBaselineReloadGeneration;
        try
        {
            await ThemeDwmBaselineSync.SettleAndReloadBaselineAsync().ConfigureAwait(true);
            if (generation != _themeBaselineReloadGeneration || SettingsSession.Current.IsShuttingDown)
                return;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings OnThemeBaselineReloadTick: {ex}");
        }
    }

    private void SyncWindowThemeFromDesktop()
    {
        if (!AppThemeService.ShouldFollowDesktopTheme())
            return;

        AppThemeService.ApplyFromRegistry(RootGrid);
        TitleBarThemeHelper.Apply(this, extendsContentIntoTitleBar: true);
        NavigationPaneChromeService.Apply(NavView, _layoutMode);
        UpdateBackButton();
        RefreshMirrorStripChrome();
    }

    private void StartGlassSettingsPreferencesWatcher()
    {
        _preferencesWatcher?.Dispose();
        _preferencesWatcher = new GlassSettingsPreferencesWatcher(DispatcherQueue);
        _preferencesWatcher.Attach(this);
    }

    private void ApplyLocalizedStrings()
    {
        AppTitleBar.Title = SingleInstance.WindowTitle;
        GlassSettingsToolTipService.SetToolTip(TitleBarBackButton, GlassSettingsResources.Get("Main_BackToolTip"));
        AppSettingsPaneLabel.Text = GlassSettingsResources.Get("Main_AppSettingsToolTip");
        GlassSettingsToolTipService.SetToolTip(AppSettingsPaneButton, GlassSettingsResources.Get("Main_AppSettingsToolTip"));
        SearchBox.PlaceholderText = GlassSettingsResources.Get("Main_SearchPlaceholder");
    }

    /// <summary>Размер и позиция до Activate (сразу на целевые координаты).</summary>
    private void ApplyEarlyWindowPlacement()
    {
        var (_, l, t, w, h) = WindowPlacementStore.LoadOrDefault();
        AppWindow.Resize(new Windows.Graphics.SizeInt32(w, h));
        AppWindow.Move(new Windows.Graphics.PointInt32(l, t));
        AppWindow.SetPresenter(Microsoft.UI.Windowing.AppWindowPresenterKind.Default);
        ApplyWindowMinSizeConstraints(AppWindow);
    }

    private void ApplyTitleBarPlacementOptions()
    {
        try
        {
            AppWindow.TitleBar.PreferredHeightOption = TitleBarHeightOption.Tall;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings ApplyTitleBarPlacementOptions: {ex.Message}");
        }
    }

    private void ApplyPlacement()
    {
        ApplyEarlyWindowPlacement();
        ApplyTitleBarPlacementOptions();
        EnforceMinWindowSize();
        ApplyLayoutMode(GetLayoutWidth());
    }

    private void SavePlacement()
    {
        var pos = AppWindow.Position;
        var size = AppWindow.Size;
        WindowPlacementStore.Save(1, pos.X, pos.Y, size.Width, size.Height);
    }
}
