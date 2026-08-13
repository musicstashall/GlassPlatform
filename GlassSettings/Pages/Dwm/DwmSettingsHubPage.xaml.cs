using GlassSettings.Controls;
using GlassSettings.Core.Customization;
using GlassSettings.Core.Dwm;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Snapshot;
using GlassSettings.Navigation;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;

namespace GlassSettings.Pages.Dwm;

public sealed partial class DwmSettingsHubPage : Page, IExternalRegistrySyncReloadable
{
    private const DwmRegistryHive SystemHive = DwmRegistryHive.Hklm;

    private const uint HookBitCaptionText = 0x1;
    private const uint HookBitAccentOverrider = 0x2;
    private const uint HookBitGlassFrame = 0x4;
    private const uint HookBitGlassReflection = 0x8;
    private const uint HookBitCaptionMetrics = 0x10;

    private static readonly string[] ColorizationKeys =
    [
        "ColorizationColor", "ColorizationColorOverride", "ColorizationColorInactive",
        "ColorizationAfterglow", "ColorizationAfterglowOverride",
        "GlassType", "GlassOverrideAccent", "BlurDeviation",
        "GlassOpacity", "GlassOpacityInactive",
        "ColorizationOpaqueBlend", "ColorizationOpaqueBlendPriority",
        "ColorizationBlurBalance", "ColorizationBlurBalanceOverride",
        "ColorizationAfterglowBalance", "ColorizationAfterglowBalanceOverride",
        "ColorizationColorBalance", "ColorizationColorBalanceOverride",
        "ColorizationBaseTransparent", "ColorizationBaseMaximized", "ColorizationBaseOpaque",
        "ColorizationOpacity", "ColorizationOpacityInactive",
        "ColorizationOpacityMaximized", "ColorizationOpacityInactiveMaximized"
    ];

    private static readonly string[] GlassKeys = DwmSnapshotCatalog.GetKeys(DwmSnapshotPartition.Glass).ToArray();

    private static readonly string[] ThemeKeys =
    [
        "ColorizationGlassReflectionOpacity", "ColorizationGlassReflectionOpacityMaximized",
        "ColorizationGlassReflectionOpacityInactive", "ColorizationGlassReflectionOpacityInactiveMaximized",
        "ColorizationGlassReflectionIntensity", "ColorizationGlassReflectionParallaxIntensity",
        "ColorizationGlassReflectionPolicy", "CustomThemeReflection", "CustomThemeMaterial",
        "MaterialOpacity", "CustomThemeAtlas", "AtlasThemeImage"
    ];

    private sealed class HiveSectionState(
        DwmRegistryHive hive,
        SettingsNavigateRow colorRow,
        SettingsNavigateRow glassRow,
        SettingsNavigateRow themeRow,
        DwmHubOverrideBadge colorOverrideBadge,
        DwmHubOverrideBadge glassOverrideBadge,
        DwmHubOverrideBadge themeOverrideBadge)
    {
        public DwmRegistryHive Hive { get; } = hive;
        public SettingsNavigateRow ColorRow { get; } = colorRow;
        public SettingsNavigateRow GlassRow { get; } = glassRow;
        public SettingsNavigateRow ThemeRow { get; } = themeRow;
        public DwmHubOverrideBadge ColorOverrideBadge { get; } = colorOverrideBadge;
        public DwmHubOverrideBadge GlassOverrideBadge { get; } = glassOverrideBadge;
        public DwmHubOverrideBadge ThemeOverrideBadge { get; } = themeOverrideBadge;
    }

    private sealed class HookToggleEntry(TextBlock label, ToggleSwitch toggle, uint bit)
    {
        public TextBlock Label { get; } = label;
        public ToggleSwitch Toggle { get; } = toggle;
        public uint Bit { get; } = bit;
    }

    private readonly SettingsPivotTabHeader[] _hiveTabHeaders = new SettingsPivotTabHeader[2];
    private readonly Dictionary<DwmRegistryHive, HiveSectionState> _sections = new();
    private readonly CheckBox _batteryCheck = new()
    {
        VerticalAlignment = VerticalAlignment.Center,
        MinWidth = 24
    };
    private readonly CheckBox _safetyZoneCheck = new()
    {
        VerticalAlignment = VerticalAlignment.Center,
        MinWidth = 24
    };
    private readonly HookToggleEntry[] _hookEntries;
    private readonly OpenGlassSymbolDownloadService _symbolDownloadService = OpenGlassSymbolDownloadService.Current;

    private bool _suppressHiveSelectionChanged;
    private int _lastHivePivotIndex;
    private bool _loading;
    private bool _systemGateArmed;
    private CancellationTokenSource? _symbolDownloadCts;
    private CancellationTokenSource? _advancedHiveAnimCts;
    private int? _lastSymbolDownloadPercent;
    private string? _lastSymbolDownloadPdbFileName;

    private const int MaxHiveExpanderChromeRetryCount = 5;
    private bool _hiveExpanderChromeRetryPending;
    private bool _hiveExpanderLayoutUpdatedHooked;
    private int _hiveExpanderChromeRetryCount;

    public DwmSettingsHubPage()
    {
        _hookEntries =
        [
            CreateHookEntry(HookBitCaptionText),
            CreateHookEntry(HookBitAccentOverrider),
            CreateHookEntry(HookBitGlassFrame),
            CreateHookEntry(HookBitGlassReflection),
            CreateHookEntry(HookBitCaptionMetrics)
        ];

        InitializeComponent();
        ConfigureHivePivotHeaders();
        BuildSectionsForHive(UserNavHost, DwmRegistryHive.Hkcu);
        BuildSectionsForHive(SystemNavHost, DwmRegistryHive.Hklm);
        ConfigureSystemAdvancedSection();
        HivePivot.SelectedIndex = 0;
        _lastHivePivotIndex = 0;
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    public void ReloadFromExternalRegistrySync()
    {
        if (SettingsPageOperationGate.Current?.ShouldDeferRegistryWatch == true)
            return;

        ReloadSystemAdvancedSection();
        UpdateHooksPanelDefaultState();
        SettingsUnsavedIndicatorRefresh.Refresh(SystemGeneralExpander);
        RefreshHubTabUnsavedIndicators();
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        var context = DwmNavigationContext.FromNavigationParameter(e.Parameter);
        SetSelectedHive(context.Hive);
        ApplySelectedHiveContent(context.Hive);
        SyncHubRootRoute(context.Hive);
        _lastHivePivotIndex = HivePivot.SelectedIndex;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        SettingsSession.Current.DirtyChanged += Session_OnDirtyChanged;
        ReloadFromRegistry();
        ApplySelectedHiveContent(GetSelectedHive());
        RefreshHubTabUnsavedIndicators();
        SyncHubRootRoute(GetSelectedHive());
        _lastHivePivotIndex = HivePivot.SelectedIndex;
        EnsureAdvancedHiveSpecificExpanderChrome(resetRetry: true);
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        SettingsSession.Current.DirtyChanged -= Session_OnDirtyChanged;
        CancelSymbolDownloadIfRunning();
        CancelAdvancedHiveAnimation();
        UnhookHiveExpanderLayoutUpdated();
    }

    private void Session_OnDirtyChanged(object? sender, EventArgs e)
    {
        RefreshHubTabUnsavedIndicators();
        RefreshHklmOverrideBadges();
        SettingsUnsavedIndicatorRefresh.Refresh(SystemGeneralExpander);
        UpdateHooksPanelDefaultState();
    }

    private static HookToggleEntry CreateHookEntry(uint bit)
    {
        var label = new TextBlock();
        var toggle = new ToggleSwitch();
        return new HookToggleEntry(label, toggle, bit);
    }

    private void ConfigureSystemAdvancedSection()
    {
        AdvancedExpander.Title = GlassSettingsResources.Get("Dwm_AdvancedColorizationTitle");

        SystemAdvancedInfoBlock.Title = GlassSettingsResources.Get("Dwm_System_InfoTitle");
        SystemAdvancedInfoBlock.Message = GlassSettingsResources.Get("Dwm_System_InfoMessage");

        SystemGeneralExpander.IconGlyph = SettingsSectionGlyphs.General;
        SystemGeneralExpander.Title = GlassSettingsResources.Get("Dwm_System_GeneralTitle");
        SystemGeneralExpander.Description = GlassSettingsResources.Get("Dwm_System_GeneralSubtitle");
        SystemGeneralExpander.UnsavedScope = DwmSystemScopeCatalog.AllScope(SystemHive);
        SystemGeneralExpander.DimHeaderActionsWithGate = true;

        _batteryCheck.Content = GlassSettingsResources.Get("Dwm_System_BatteryShort");
        ToolTipService.SetToolTip(_batteryCheck, GlassSettingsResources.Get("Dwm_System_BatteryTooltip"));
        _safetyZoneCheck.Content = GlassSettingsResources.Get("Dwm_System_SafetyZoneShort");
        ToolTipService.SetToolTip(_safetyZoneCheck, GlassSettingsResources.Get("Dwm_System_SafetyZoneTooltip"));

        var headerActions = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 12,
            VerticalAlignment = VerticalAlignment.Center
        };
        headerActions.Children.Add(_batteryCheck);
        headerActions.Children.Add(_safetyZoneCheck);
        SystemGeneralExpander.HeaderActions = headerActions;

        _batteryCheck.Checked += BatteryCheck_OnChanged;
        _batteryCheck.Unchecked += BatteryCheck_OnChanged;
        _safetyZoneCheck.Checked += SafetyZoneCheck_OnChanged;
        _safetyZoneCheck.Unchecked += SafetyZoneCheck_OnChanged;

        SystemGeneralExpander.WireExpandedLayoutRefresh(RefreshSystemGateBodyLayout);

        var labelStyle = Application.Current.Resources["GlassSettingsSpoilerSecondaryTextStyle"] as Style;
        var toggleStyle = Application.Current.Resources["GlassSettingsCompactToggleSwitchStyle"] as Style;
        foreach (var entry in _hookEntries)
        {
            entry.Label.Style = labelStyle;
            entry.Toggle.Style = toggleStyle;
            entry.Toggle.Toggled += HookToggle_OnToggled;
        }

        ApplyHookLocalizedStrings();

        var hooksTable = SettingsParameterBodyLayout.CreateBodyTable();
        SettingsParameterBodyLayout.AddDualRow(
            hooksTable,
            _hookEntries[0].Label,
            _hookEntries[0].Toggle,
            _hookEntries[1].Label,
            _hookEntries[1].Toggle);
        SettingsParameterBodyLayout.AddDualRow(
            hooksTable,
            _hookEntries[2].Label,
            _hookEntries[2].Toggle,
            _hookEntries[3].Label,
            _hookEntries[3].Toggle,
            topMargin: true);
        SettingsParameterBodyLayout.AddSingleRow(
            hooksTable,
            _hookEntries[4].Label,
            _hookEntries[4].Toggle,
            topMargin: true);

        HooksPanel.Description = GlassSettingsResources.Get("Dwm_System_HooksCta");
        HooksPanel.BodyContent = hooksTable;
        HooksPanel.DefaultClick += (_, _) => OnHooksDefault();

        ConfigureSymbolsDownloadRow();

        AdvancedHiveSpecificExpander.Loaded += OnAdvancedHiveSpecificExpanderLoaded;
    }

    private void OnAdvancedHiveSpecificExpanderLoaded(object sender, RoutedEventArgs e) =>
        EnsureAdvancedHiveSpecificExpanderChrome(resetRetry: true);

    private void ConfigureSymbolsDownloadRow()
    {
        SymbolsDownloadRow.Title = GlassSettingsResources.Get("Dwm_System_SymbolsTitle");
        SymbolsDownloadRow.IconGlyph = SettingsSectionGlyphs.Symbols;
        SymbolsDownloadRow.Description = GlassSettingsResources.Format(
            "Dwm_System_SymbolsCachePath",
            _symbolDownloadService.GetCacheDirectoryDisplayPath());
        SymbolsDownloadRow.DownloadLabel = GlassSettingsResources.Get("Dwm_System_SymbolsDownload");
        SymbolsDownloadRow.CancelLabel = GlassSettingsResources.Get("Dwm_System_SymbolsCancel");
        SymbolsDownloadRow.DownloadClick += SymbolsDownloadRow_OnDownloadClick;
        SymbolsDownloadRow.CancelClick += SymbolsDownloadRow_OnCancelClick;
        ResetSymbolDownloadUi();
    }

    private void ResetSymbolDownloadUi()
    {
        SymbolsDownloadRow.SetActionButtonsEnabled(true);
        SymbolsDownloadRow.SetActionMode(SettingsProgressActionMode.Download);
        SymbolsDownloadRow.SetProgress(null, SettingsProgressVisualState.Idle);
        _lastSymbolDownloadPercent = null;
        _lastSymbolDownloadPdbFileName = null;
    }

    private async void SymbolsDownloadRow_OnDownloadClick(object? sender, RoutedEventArgs e) =>
        await RunSymbolDownloadFlowAsync().ConfigureAwait(true);

    private async Task RunSymbolDownloadFlowAsync()
    {
        if (_symbolDownloadService.IsRunning)
            return;

        if (!SymbolDownloadPreflightService.TryEnsureSymbolTools(out var missingTools))
        {
            await OpenGlassSymbolDownloadDialogService.ShowMissingSymbolToolsAsync(missingTools)
                .ConfigureAwait(true);
            return;
        }

        if (_symbolDownloadService.HasExistingCacheContent())
        {
            var confirmed = await OpenGlassSymbolDownloadDialogService
                .ConfirmClearExistingSymbolsAsync(_symbolDownloadService.GetCacheDirectoryDisplayPath())
                .ConfigureAwait(true);
            if (!confirmed)
                return;
        }

        var downloadAttemptStarted = false;

        while (true)
        {
            if (!NetworkConnectivityService.IsInternetAvailable())
            {
                var retry = downloadAttemptStarted
                    ? await OpenGlassSymbolDownloadDialogService.ConfirmRetryAfterConnectionLostAsync()
                        .ConfigureAwait(true)
                    : await OpenGlassSymbolDownloadDialogService.ShowNoInternetAsync().ConfigureAwait(true);
                if (!retry)
                    return;

                continue;
            }

            downloadAttemptStarted = true;
            var completion = await ExecuteSymbolDownloadAttemptAsync().ConfigureAwait(true);

            switch (completion.Kind)
            {
                case OpenGlassSymbolDownloadOutcomeKind.Success:
                case OpenGlassSymbolDownloadOutcomeKind.Cancelled:
                    return;

                case OpenGlassSymbolDownloadOutcomeKind.Failed
                    when completion.FailureKind == OpenGlassSymbolDownloadFailureKind.ConnectionLost:
                    ResetSymbolDownloadUi();
                    if (await OpenGlassSymbolDownloadDialogService.ConfirmRetryAfterConnectionLostAsync()
                            .ConfigureAwait(true))
                    {
                        continue;
                    }

                    return;

                default:
                    SymbolsDownloadRow.SetProgress(
                        _lastSymbolDownloadPercent ?? 0,
                        SettingsProgressVisualState.Error,
                        _lastSymbolDownloadPdbFileName);
                    SymbolsDownloadRow.SetActionMode(SettingsProgressActionMode.Download);
                    await OpenGlassSymbolDownloadDialogService.ShowDownloadFailedAsync(completion.Message)
                        .ConfigureAwait(true);
                    ResetSymbolDownloadUi();
                    return;
            }
        }
    }

    private async Task<OpenGlassSymbolDownloadCompletion> ExecuteSymbolDownloadAttemptAsync()
    {
        _symbolDownloadCts?.Dispose();
        _symbolDownloadCts = new CancellationTokenSource();

        try
        {
            SymbolsDownloadRow.SetActionMode(SettingsProgressActionMode.Cancel);
            SymbolsDownloadRow.SetProgress(0, SettingsProgressVisualState.Downloading);

            var progress = new Progress<SymbolDownloadProgress>(update =>
            {
                _lastSymbolDownloadPercent = update.Percent;
                _lastSymbolDownloadPdbFileName = update.PdbFileName;
                DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Normal, () =>
                    SymbolsDownloadRow.SetProgress(
                        update.Percent,
                        SettingsProgressVisualState.Downloading,
                        update.PdbFileName));
            });

            var completion = await _symbolDownloadService.StartDownloadAsync(
                progress,
                _symbolDownloadCts.Token).ConfigureAwait(true);

            switch (completion.Kind)
            {
                case OpenGlassSymbolDownloadOutcomeKind.Success:
                    SymbolsDownloadRow.SetProgress(100, SettingsProgressVisualState.Success);
                    await OpenGlassSymbolDownloadDialogService.ShowDownloadSucceededAsync()
                        .ConfigureAwait(true);
                    ResetSymbolDownloadUi();
                    break;

                case OpenGlassSymbolDownloadOutcomeKind.Cancelled:
                    ResetSymbolDownloadUi();
                    break;
            }

            return completion;
        }
        catch (OperationCanceledException)
        {
            ResetSymbolDownloadUi();
            return new OpenGlassSymbolDownloadCompletion(
                OpenGlassSymbolDownloadOutcomeKind.Cancelled,
                unchecked((int)0x800704C7),
                "Symbol download cancelled.");
        }
        catch (Exception ex)
        {
            ResetSymbolDownloadUi();
            return new OpenGlassSymbolDownloadCompletion(
                OpenGlassSymbolDownloadOutcomeKind.Failed,
                unchecked((int)0x80004005),
                ex.Message);
        }
        finally
        {
            SymbolsDownloadRow.SetActionButtonsEnabled(true);
            _symbolDownloadCts?.Dispose();
            _symbolDownloadCts = null;
        }
    }

    private void SymbolsDownloadRow_OnCancelClick(object? sender, RoutedEventArgs e)
    {
        CancelSymbolDownloadIfRunning();
    }

    private void CancelSymbolDownloadIfRunning()
    {
        SymbolsDownloadRow.SetActionButtonsEnabled(false);
        _symbolDownloadCts?.Cancel();
        _symbolDownloadService.CancelDownload();
    }

    private void ApplyHookLocalizedStrings()
    {
        _hookEntries[0].Label.Text = GlassSettingsResources.Get("Dwm_System_Hook_CaptionText");
        _hookEntries[1].Label.Text = GlassSettingsResources.Get("Dwm_System_Hook_AccentOverrider");
        _hookEntries[2].Label.Text = GlassSettingsResources.Get("Dwm_System_Hook_GlassFrame");
        _hookEntries[3].Label.Text = GlassSettingsResources.Get("Dwm_System_Hook_GlassReflection");
        _hookEntries[4].Label.Text = GlassSettingsResources.Get("Dwm_System_Hook_CaptionMetrics");
    }

    private void ConfigureHivePivotHeaders()
    {
        _hiveTabHeaders[0] = SettingsPivotTabHeader.Create(GlassSettingsResources.Get("Dwm_TabUser"));
        _hiveTabHeaders[1] = SettingsPivotTabHeader.Create(GlassSettingsResources.Get("Dwm_TabSystem"));

        if (HivePivot.Items.Count >= 2)
        {
            ((PivotItem)HivePivot.Items[0]).Header = _hiveTabHeaders[0].Root;
            ((PivotItem)HivePivot.Items[1]).Header = _hiveTabHeaders[1].Root;
        }
    }

    private void BuildSectionsForHive(SettingsNavigationHost host, DwmRegistryHive hive)
    {
        var colorRouteId = DwmRouteIds.ForHive(DwmRouteIds.Colorization, hive);
        var glassRouteId = DwmRouteIds.ForHive(DwmRouteIds.Glass, hive);
        var themeRouteId = DwmRouteIds.ForHive(DwmRouteIds.Theme, hive);

        var colorOverrideBadge = CreateOverrideBadge();
        var glassOverrideBadge = CreateOverrideBadge();
        var themeOverrideBadge = CreateOverrideBadge();

        var colorRow = host.AddNavigateSection(
            colorRouteId,
            GlassSettingsResources.Get("Dwm_HubColorization"),
            GlassSettingsResources.Get("Dwm_HubColorizationDesc"),
            onNavigate: h =>
            {
                h.PushPage(
                    typeof(DwmColorizationDetailPage),
                    DwmNavigationContext.ForDetail(hive, DwmRouteIds.Colorization),
                    GlassSettingsResources.Get("Breadcrumb_Colorization"));
            });
        colorRow.HeaderActions = colorOverrideBadge;
        colorRow.UnsavedScope = SettingsCommitScope.ForDwmHiveArea(CustomizationAreaId.DwmColorization, hive);

        var glassRow = host.AddNavigateSection(
            glassRouteId,
            GlassSettingsResources.Get("Dwm_HubGlass"),
            GlassSettingsResources.Get("Dwm_HubGlassDesc"),
            onNavigate: h =>
            {
                h.PushPage(
                    typeof(DwmGlassDetailPage),
                    DwmNavigationContext.ForDetail(hive, DwmRouteIds.Glass),
                    GlassSettingsResources.Get("Breadcrumb_DwmGlass"));
            });
        glassRow.HeaderActions = glassOverrideBadge;
        glassRow.UnsavedScope = SettingsCommitScope.ForDwmHiveArea(CustomizationAreaId.DwmGlass, hive);

        var themeRow = host.AddNavigateSection(
            themeRouteId,
            GlassSettingsResources.Get("Dwm_HubTheme"),
            GlassSettingsResources.Get("Dwm_HubThemeDesc"),
            onNavigate: h =>
            {
                h.PushPage(
                    typeof(DwmThemeDetailPage),
                    DwmNavigationContext.ForDetail(hive, DwmRouteIds.Theme),
                    GlassSettingsResources.Get("Breadcrumb_DwmTheme"));
            });
        themeRow.HeaderActions = themeOverrideBadge;
        themeRow.UnsavedScope = SettingsCommitScope.ForDwmHiveArea(CustomizationAreaId.DwmTheme, hive);

        _sections[hive] = new HiveSectionState(
            hive,
            colorRow,
            glassRow,
            themeRow,
            colorOverrideBadge,
            glassOverrideBadge,
            themeOverrideBadge);
    }

    private static DwmHubOverrideBadge CreateOverrideBadge() =>
        new()
        {
            VerticalAlignment = VerticalAlignment.Center,
            Visibility = Visibility.Collapsed
        };

    private void RefreshHubTabUnsavedIndicators()
    {
        _hiveTabHeaders[0].SetUnsavedVisible(
            SettingsSession.Current.HasUnsavedInScope(SettingsCommitScope.ForDwmHive(DwmRegistryHive.Hkcu)));
        _hiveTabHeaders[1].SetUnsavedVisible(
            SettingsSession.Current.HasUnsavedInScope(SettingsCommitScope.ForDwmHive(DwmRegistryHive.Hklm)));
    }

    private void ReloadFromRegistry()
    {
        RefreshHklmOverrideBadges();
        RefreshHubTabUnsavedIndicators();
        ReloadSystemAdvancedSection();
    }

    private void ReloadSystemAdvancedSection()
    {
        _loading = true;
        try
        {
            if (_systemGateArmed && IsSystemGateOn(fromRegistryOnly: true))
                _systemGateArmed = false;

            SystemGeneralExpander.IsGateOn = IsSystemGateOn();
            _batteryCheck.IsChecked = (ReadSystemDword("DisableGlassOnBattery") ?? 1) != 0;
            _safetyZoneCheck.IsChecked = (ReadSystemDword("GlassSafetyZoneMode") ?? 1) == 0;

            var hookMask = ReadSystemDword("DisabledHooks") ?? 0;
            foreach (var entry in _hookEntries)
                entry.Toggle.IsOn = (hookMask & entry.Bit) != 0;
        }
        finally
        {
            _loading = false;
        }

        UpdateHooksPanelDefaultState();
    }

    private void UpdateHooksPanelDefaultState()
    {
        if (_loading)
            return;

        var hooksAtDefault = !DwmRegistryStore.ValueExists(SystemHive, "DisabledHooks")
                             || (ReadSystemDword("DisabledHooks") ?? 0) == 0;
        HooksPanel.SetDefaultActionState(SystemGeneralExpander.IsGateOn, hooksAtDefault);
    }

    private bool IsSystemGateOn(bool fromRegistryOnly = false) =>
        DwmRegistryStore.HasAnyPresent(SystemHive, DwmSystemScopeCatalog.AllKeys)
        || (!fromRegistryOnly && _systemGateArmed);

    private static uint? ReadSystemDword(string valueName) =>
        DwmRegistryStore.ReadDword(SystemHive, valueName);

    private void RefreshHklmOverrideBadges()
    {
        if (!_sections.TryGetValue(DwmRegistryHive.Hklm, out var systemSection))
            return;

        systemSection.ColorOverrideBadge.Visibility = DwmRegistryStore.CountHkcuOverrides(ColorizationKeys) > 0
            ? Visibility.Visible
            : Visibility.Collapsed;
        systemSection.GlassOverrideBadge.Visibility = DwmRegistryStore.CountHkcuOverrides(GlassKeys) > 0
            ? Visibility.Visible
            : Visibility.Collapsed;
        systemSection.ThemeOverrideBadge.Visibility = DwmRegistryStore.CountHkcuOverrides(ThemeKeys) > 0
            ? Visibility.Visible
            : Visibility.Collapsed;
    }

    private async void HivePivot_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        RefreshHubTabUnsavedIndicators();
        if (_suppressHiveSelectionChanged)
        {
            _lastHivePivotIndex = HivePivot.SelectedIndex;
            return;
        }

        var newIndex = HivePivot.SelectedIndex;
        var previousIndex = _lastHivePivotIndex;
        if (newIndex == previousIndex)
            return;

        var selectedHive = GetSelectedHive();
        var previousHive = previousIndex == 1 ? DwmRegistryHive.Hklm : DwmRegistryHive.Hkcu;
        var advancedExpanded = AdvancedExpander.IsExpanded;
        var hiveChanged = previousHive != selectedHive;

        if (advancedExpanded && hiveChanged)
        {
            var hiveSpecificTask = PrepareAdvancedHiveSpecificForPivotAsync(selectedHive, animate: true);
            await Task.WhenAll(
                PivotTabContentTransition.AnimateTabChangeAsync(
                    HiveTabContentHost,
                    previousIndex,
                    newIndex,
                    () => ApplyNavHostsForHive(selectedHive)),
                hiveSpecificTask).ConfigureAwait(true);
            ApplyAdvancedPanelForHiveInstant(selectedHive);
            RefreshSystemDividerChromeDeferred();
        }
        else
        {
            await PivotTabContentTransition.AnimateTabChangeAsync(
                HiveTabContentHost,
                previousIndex,
                newIndex,
                () => ApplySelectedHiveContent(selectedHive)).ConfigureAwait(true);
        }

        SettingsNavigationService.Current.SwitchRootRoute(
            typeof(DwmSettingsHubPage),
            DwmNavigationContext.ForHub(selectedHive),
            GlassSettingsResources.Get("Breadcrumb_DwmSettings"));
        _lastHivePivotIndex = newIndex;
    }

    private void ApplyNavHostsForHive(DwmRegistryHive hive)
    {
        UserNavHost.Visibility = hive == DwmRegistryHive.Hkcu
            ? Visibility.Visible
            : Visibility.Collapsed;
        SystemNavHost.Visibility = hive == DwmRegistryHive.Hklm
            ? Visibility.Visible
            : Visibility.Collapsed;
    }

    private DwmRegistryHive GetSelectedHive() =>
        HivePivot.SelectedIndex == 1
            ? DwmRegistryHive.Hklm
            : DwmRegistryHive.Hkcu;

    private void SetSelectedHive(DwmRegistryHive hive)
    {
        var index = hive == DwmRegistryHive.Hklm ? 1 : 0;
        if (HivePivot.SelectedIndex == index)
            return;

        _suppressHiveSelectionChanged = true;
        try
        {
            HivePivot.SelectedIndex = index;
        }
        finally
        {
            _suppressHiveSelectionChanged = false;
        }

        ApplySelectedHiveContent(hive);
        _lastHivePivotIndex = index;
    }

    private void ApplySelectedHiveContent(DwmRegistryHive hive)
    {
        ApplyNavHostsForHive(hive);
        ApplyAdvancedPanelForHiveInstant(hive);
        RefreshSystemDividerChromeDeferred();
    }

    private void ApplyAdvancedPanelForHiveInstant(DwmRegistryHive hive)
    {
        CancelAdvancedHiveAnimation();

        SystemAdvancedInfoBlock.Visibility = Visibility.Visible;
        SystemGeneralExpander.Visibility = Visibility.Visible;
        SymbolsDownloadRow.Visibility = Visibility.Visible;
        AdvancedHiveSpecificExpander.IsExpanded = hive == DwmRegistryHive.Hklm;
        EnsureAdvancedHiveSpecificExpanderChrome();
    }

    private Task PrepareAdvancedHiveSpecificForPivotAsync(DwmRegistryHive hive, bool animate)
    {
        var expand = hive == DwmRegistryHive.Hklm;
        if (!animate)
        {
            AdvancedHiveSpecificExpander.IsExpanded = expand;
            return Task.CompletedTask;
        }

        CancelAdvancedHiveAnimation();
        _advancedHiveAnimCts = new CancellationTokenSource();
        var token = _advancedHiveAnimCts.Token;
        return ExpanderRevealTransition.SetExpandedAsync(AdvancedHiveSpecificExpander, expand, token);
    }

    private void CancelAdvancedHiveAnimation()
    {
        if (_advancedHiveAnimCts is null)
            return;

        _advancedHiveAnimCts.Cancel();
        _advancedHiveAnimCts.Dispose();
        _advancedHiveAnimCts = null;
    }

    private void RefreshSystemDividerChromeDeferred()
    {
        AdvancedExpander.RefreshDividerChrome();
        DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Normal, () =>
            AdvancedExpander.RefreshDividerChrome());
    }

    private void EnsureAdvancedHiveSpecificExpanderChrome(bool resetRetry = false)
    {
        if (resetRetry)
            _hiveExpanderChromeRetryCount = 0;

        SpoilerExpanderChrome.ApplyHeaderlessExpanderChrome(AdvancedHiveSpecificExpander);

        if (SpoilerExpanderChrome.IsHeaderlessChromeApplied(AdvancedHiveSpecificExpander))
        {
            _hiveExpanderChromeRetryCount = 0;
            UnhookHiveExpanderLayoutUpdated();
            return;
        }

        ScheduleHiveExpanderChromeRetry();
    }

    private void ScheduleHiveExpanderChromeRetry()
    {
        EnsureHiveExpanderLayoutUpdatedHook();

        if (_hiveExpanderChromeRetryPending || _hiveExpanderChromeRetryCount >= MaxHiveExpanderChromeRetryCount)
            return;

        _hiveExpanderChromeRetryPending = true;
        _hiveExpanderChromeRetryCount++;
        DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Normal, () =>
        {
            _hiveExpanderChromeRetryPending = false;
            EnsureAdvancedHiveSpecificExpanderChrome();
        });
    }

    private void EnsureHiveExpanderLayoutUpdatedHook()
    {
        if (_hiveExpanderLayoutUpdatedHooked
            || SpoilerExpanderChrome.IsHeaderlessChromeApplied(AdvancedHiveSpecificExpander))
            return;

        _hiveExpanderLayoutUpdatedHooked = true;
        AdvancedHiveSpecificExpander.LayoutUpdated += OnAdvancedHiveSpecificExpanderLayoutUpdated;
    }

    private void UnhookHiveExpanderLayoutUpdated()
    {
        if (!_hiveExpanderLayoutUpdatedHooked)
            return;

        AdvancedHiveSpecificExpander.LayoutUpdated -= OnAdvancedHiveSpecificExpanderLayoutUpdated;
        _hiveExpanderLayoutUpdatedHooked = false;
    }

    private void OnAdvancedHiveSpecificExpanderLayoutUpdated(object? sender, object e)
    {
        if (SpoilerExpanderChrome.IsHeaderlessChromeApplied(AdvancedHiveSpecificExpander))
        {
            _hiveExpanderChromeRetryCount = 0;
            UnhookHiveExpanderLayoutUpdated();
            return;
        }

        EnsureAdvancedHiveSpecificExpanderChrome();
    }

    private void AdvancedExpander_OnExpanded(object sender, RoutedEventArgs e)
    {
        if (!AdvancedExpander.IsExpanded)
            return;

        ApplyAdvancedPanelForHiveInstant(GetSelectedHive());

        if (SystemGeneralExpander.Visibility == Visibility.Visible
            && AdvancedHiveSpecificExpander.IsExpanded)
        {
            EnsureExpanderHeaderChrome(SystemGeneralExpander);
            SystemGeneralExpander.RefreshHeaderActionsLayout();
        }
    }

    private void RefreshSystemGateBodyLayout()
    {
        EnsureExpanderHeaderChrome(SystemGeneralExpander);
        HooksPanel.RefreshLayout();
    }

    private async void SystemGeneralExpander_OnGateToggled(object sender, RoutedEventArgs e)
    {
        if (_loading || PageShell.Operations.ShouldBlockPageMutation)
            return;

        await PageShell.Operations.RunSequenceAsync(async seq =>
        {
            await seq.RegistryMutationAsync(() =>
            {
                if (SystemGeneralExpander.IsGateOn)
                {
                    _systemGateArmed = true;
                }
                else
                {
                    _systemGateArmed = false;
                    foreach (var key in DwmSystemScopeCatalog.AllKeys)
                        SettingsSession.Current.DeleteDwmValue(SystemHive, key, liveDwm: true);
                }

                _loading = true;
                try { ReloadSystemAdvancedSection(); }
                finally { _loading = false; }
                return Task.CompletedTask;
            }).ConfigureAwait(true);

            await seq.NoticeSyncAsync(() => PageShell.FlushSessionNotice()).ConfigureAwait(true);
        }).ConfigureAwait(true);
    }

    private async void BatteryCheck_OnChanged(object sender, RoutedEventArgs e)
    {
        if (_loading || !SystemGeneralExpander.IsGateOn)
            return;

        var useDefault = _batteryCheck.IsChecked == true;
        await RunSystemRegistryMutationAsync(() =>
        {
            if (useDefault)
                SettingsSession.Current.DeleteDwmValue(SystemHive, "DisableGlassOnBattery", liveDwm: true);
            else
                SettingsSession.Current.WriteDwmDword(SystemHive, "DisableGlassOnBattery", 0, liveDwm: true, dwmNotifyImmediate: true);
        }).ConfigureAwait(true);
    }

    private async void SafetyZoneCheck_OnChanged(object sender, RoutedEventArgs e)
    {
        if (_loading || !SystemGeneralExpander.IsGateOn)
            return;

        var disableSafetyZone = _safetyZoneCheck.IsChecked == true;
        await RunSystemRegistryMutationAsync(() =>
        {
            if (disableSafetyZone)
                SettingsSession.Current.WriteDwmDword(SystemHive, "GlassSafetyZoneMode", 0, liveDwm: true, dwmNotifyImmediate: true);
            else
                SettingsSession.Current.DeleteDwmValue(SystemHive, "GlassSafetyZoneMode", liveDwm: true);
        }).ConfigureAwait(true);
    }

    private async void HookToggle_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading || !SystemGeneralExpander.IsGateOn)
            return;

        await RunSystemRegistryMutationAsync(CommitDisabledHooksFromUi).ConfigureAwait(true);
    }

    private async void OnHooksDefault()
    {
        if (_loading || !SystemGeneralExpander.IsGateOn)
            return;

        await PageShell.Operations.RunSequenceAsync(async seq =>
        {
            await seq.RegistryMutationAsync(() =>
            {
                SettingsSession.Current.DeleteDwmValue(SystemHive, "DisabledHooks", liveDwm: true);

                _loading = true;
                try
                {
                    var hookMask = ReadSystemDword("DisabledHooks") ?? 0;
                    foreach (var entry in _hookEntries)
                        entry.Toggle.IsOn = (hookMask & entry.Bit) != 0;
                }
                finally
                {
                    _loading = false;
                }

                UpdateHooksPanelDefaultState();
                return Task.CompletedTask;
            }).ConfigureAwait(true);

            await seq.NoticeSyncAsync(() => PageShell.FlushSessionNotice()).ConfigureAwait(true);
        }).ConfigureAwait(true);
    }

    private void CommitDisabledHooksFromUi()
    {
        uint mask = 0;
        foreach (var entry in _hookEntries)
        {
            if (entry.Toggle.IsOn)
                mask |= entry.Bit;
        }

        if (mask == 0)
            SettingsSession.Current.DeleteDwmValue(SystemHive, "DisabledHooks", liveDwm: true);
        else
            SettingsSession.Current.WriteDwmDword(SystemHive, "DisabledHooks", mask, liveDwm: true, dwmNotifyImmediate: true);

        UpdateHooksPanelDefaultState();
    }

    private async Task RunSystemRegistryMutationAsync(Action mutate)
    {
        await PageShell.Operations.RunSequenceAsync(async seq =>
        {
            await seq.RegistryMutationAsync(() =>
            {
                mutate();
                UpdateHooksPanelDefaultState();
                return Task.CompletedTask;
            }).ConfigureAwait(true);

            await seq.NoticeSyncAsync(() => PageShell.FlushSessionNotice()).ConfigureAwait(true);
        }).ConfigureAwait(true);
    }

    private static void EnsureExpanderHeaderChrome(SettingsSystemGateExpander expander)
    {
        void Apply() => expander.RefreshThemeChrome();

        if (expander.IsLoaded)
            Apply();
        else
            expander.Loaded += OnLoaded;

        void OnLoaded(object sender, RoutedEventArgs e)
        {
            expander.Loaded -= OnLoaded;
            Apply();
        }
    }

    private static void SyncHubRootRoute(DwmRegistryHive hive) =>
        SettingsNavigationService.Current.UpdateRootTrail(
            typeof(DwmSettingsHubPage),
            DwmNavigationContext.ForHub(hive),
            GlassSettingsResources.Get("Breadcrumb_DwmSettings"));
}
