using GlassSettings.Controls;
using GlassSettings.Core.Customization;
using GlassSettings.Core.Dwm;
using GlassSettings.Core.Layout;
using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Snapshot;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;

namespace GlassSettings.Pages.Dwm;

public sealed partial class DwmColorizationDetailPage : Page, ISettingsCommitScopeProvider, IExternalRegistrySyncReloadable
{
    private const uint ThemeColorSentinel = 0xFFFFFFFFu;
    private const uint AutoColorSentinel = 0xFFFFFFFEu;
    private const uint ThemeOpacitySentinel = 0xFFFFFFFFu;

    private sealed record PivotTabSpec(string RegistryKey, string TitleKey, string HeaderDescKey, string BodyDescKey);

    private sealed record BaseOpacityTabSpec(
        string ActiveRegistryKey,
        string ActiveDescKey,
        string InactiveRegistryKey,
        string InactiveDescKey);

    private static readonly PivotTabSpec[] BaseColorTabs =
    [
        new("ColorizationBaseTransparent", "Dwm_BaseTransparent", "Dwm_BaseTransparentDesc", "Dwm_BaseTransparentBodyDesc"),
        new("ColorizationBaseMaximized", "Dwm_BaseMaximized", "Dwm_BaseMaximizedDesc", "Dwm_BaseMaximizedBodyDesc"),
        new("ColorizationBaseOpaque", "Dwm_BaseOpaque", "Dwm_BaseOpaqueDesc", "Dwm_BaseOpaqueBodyDesc")
    ];

    private static readonly string[] BaseColorPivotHeaders =
    [
        "Dwm_PivotBaseColorTransparent",
        "Dwm_PivotBaseColorMaximized",
        "Dwm_PivotBaseColorOpaque"
    ];

    private static readonly string[] GlassTypePivotHeaders =
    [
        "Dwm_GlassTypeVista",
        "Dwm_GlassTypeWin7"
    ];

    private static readonly string[] BaseOpacityPivotHeaders =
    [
        "Dwm_PivotNormalWindow",
        "Dwm_PivotMaximizedWindow"
    ];

    private static readonly BaseOpacityTabSpec[] BaseOpacityTabSpecs =
    [
        new(
            "ColorizationOpacity",
            "Dwm_BaseOpacityActiveDesc",
            "ColorizationOpacityInactive",
            "Dwm_BaseOpacityInactiveDesc"),
        new(
            "ColorizationOpacityMaximized",
            "Dwm_BaseOpacityMaximizedDesc",
            "ColorizationOpacityInactiveMaximized",
            "Dwm_BaseOpacityInactiveMaximizedDesc")
    ];

    /// <summary>Chrome строки карточки композиции: иконка/заголовок + точка-индикатор + подстрока конфликта.</summary>
    private Slider? _blurRadiusSlider;
    private TextBlock? _blurRadiusValueText;
    private Slider? _blurBalanceSlider;
    private TextBlock? _blurBalanceValueText;
    private Grid? _activeColorBlurFooter;
    private TextBlock? _blurBalanceLabel;
    private uint _lastBaseColorArgb;

    private uint _lastActiveRgb;
    private uint _lastActivePercent;
    private uint _lastBlurBalanceLive;
    private uint _lastSecondaryRgb;
    private uint _lastSecondaryPercent;
    private bool _inactiveVistaGateArmed;

    private ComboBox? _baseColorModeCombo;
    private int _lastBaseColorPivotIndex;
    private int _lastGlassTypePivotIndex;
    private string _baseColorRegKey = BaseColorTabs[0].RegistryKey;

    private DwmOpacityGroupRow? _baseOpacityActiveRow;
    private DwmOpacityGroupRow? _baseOpacityInactiveRow;
    private int _lastBaseOpacityPivotIndex;

    private readonly SettingsPivotTabHeader[] _glassTypeTabHeaders = new SettingsPivotTabHeader[2];
    private readonly SettingsPivotTabHeader[] _baseColorTabHeaders = new SettingsPivotTabHeader[3];
    private readonly SettingsPivotTabHeader[] _baseOpacityTabHeaders = new SettingsPivotTabHeader[2];

    private ComboBox? _priorityCombo;

    private const double QuickActionColumnGap = 8;
    private const double CompositionHeroColumnGap = 24;
    private double _quickActionsCol0Width;
    private double _quickActionsCol1Width;
    private double _quickActionsMinCol0Width;
    private double _quickActionsMinCol1Width;
    private double _quickActionsMinTableWidth;
    private double _quickActionsPreferredTableWidth;
    private bool _quickActionsWidthsDirty = true;

    private const uint BlurDeviationDefault = 30;
    private const int BlurRadiusMin = 0;
    private const int BlurRadiusMax = 30;
    private const int BlurRadiusD3DValue = 3;

    private const string BlurDeviationKey = "BlurDeviation";
    private const string OverrideAccentKey = "GlassOverrideAccent";
    private const string OpaqueBlendKey = "ColorizationOpaqueBlend";

    private DwmRegistryHive _hive = DwmRegistryHive.Hkcu;
    private bool _loading;
    private bool _deferredPreviewRefresh;
    private bool _compositionHeroSideBySide = true;
    private bool _quickActionCompactMode;
    private bool _compositionLayoutInitialized;
    private EventHandler<DesktopEnvironmentChangedEventArgs>? _environmentChangedHandler;
    private DispatcherQueueTimer? _wallpaperRefreshTimer;
    private DispatcherQueueTimer? _themeReloadTimer;
    private DispatcherQueueTimer? _externalPreviewTimer;
    private int _themeReloadGeneration;

    private readonly UiElementCrossfade _compositionCrossfade;
    private bool _visibleWin7Preview;
    private bool _hasVisiblePreviewMode;

    public DwmColorizationDetailPage()
    {
        InitializeComponent();
        Win7CompositionPreview.Opacity = 0;
        _compositionCrossfade = new UiElementCrossfade(
            VistaCompositionPreview,
            Win7CompositionPreview,
            fadeDurationMs: (int)PivotTabContentTransition.TabChangeTotalDuration.TotalMilliseconds);
        ConfigureStaticControls();
        BuildCompositionTopControls();
        BuildActiveColorBlurSlider();
        BuildBaseColorHeader();
        BuildBaseOpacityRows();
        BuildPriorityRow();
        ConfigurePivotHeaders();
        CompositionHeroLayoutHost.SizeChanged += (_, _) => UpdateCompositionHeroLayout();
        Loaded += OnPageLoaded;
        Unloaded += OnPageUnloaded;
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        _hive = DwmNavigationContext.FromNavigationParameter(e.Parameter).Hive;
        _quickActionsWidthsDirty = true;
        AdvancedExpander.UnsavedScope = DwmColorizationScopeCatalog.AdvancedScope(_hive);
        PageShell.UpdateCommitScope(GetCommitScope());
        HiveScopeBanner.Hive = _hive;
        if (_baseOpacityActiveRow is not null)
            _baseOpacityActiveRow.Hive = _hive;
        if (_baseOpacityInactiveRow is not null)
            _baseOpacityInactiveRow.Hive = _hive;
        ApplyExpanderUnsavedScopesForTab(GlassTypePivot.SelectedIndex);
        ApplyBaseColorExpanderUnsavedScope(BaseColorPivot.SelectedIndex);
        AdvancedExpander.UnsavedScope = DwmColorizationScopeCatalog.AdvancedScope(_hive);
        AdvancedExpander.RefreshUnsavedIndicator();
        RefreshParallelBranchWarnings();
        _ = RefreshHkcuConflictNoticeAsync();
    }

    private void OnPageLoaded(object sender, RoutedEventArgs e)
    {
        SettingsSession.Current.DirtyChanged += Session_OnDirtyChanged;
        _quickActionsWidthsDirty = true;
        Reload();
        UpdateCompositionHeroLayout();
        RefreshPivotTabUnsavedIndicators();
        AdvancedExpander.RefreshUnsavedIndicator();
        HookEnvironmentWatcher();
        _ = RefreshHkcuConflictNoticeAsync();
    }

    private void OnPageUnloaded(object sender, RoutedEventArgs e)
    {
        SettingsSession.Current.DirtyChanged -= Session_OnDirtyChanged;
        UnhookEnvironmentWatcher();
        _wallpaperRefreshTimer?.Stop();
        _themeReloadTimer?.Stop();
        _externalPreviewTimer?.Stop();
    }

    private void Session_OnDirtyChanged(object? sender, EventArgs e)
    {
        RefreshPivotTabUnsavedIndicators();
        ActiveColorExpander.RefreshUnsavedIndicator();
        SecondaryColorExpander.RefreshUnsavedIndicator();
        BaseColorExpander.RefreshUnsavedIndicator();
        AdvancedExpander.RefreshUnsavedIndicator();
        RefreshParallelBranchWarnings();
        if (SettingsPageOperationGate.Current?.ShouldDeferNotices == true
            || SettingsPageOperationGate.Current?.ShouldBlockPageMutation == true)
        {
            _deferredPreviewRefresh = true;
        }
        else
        {
            ScheduleExternalPreviewRefresh();
        }

        _ = RefreshHkcuConflictNoticeAsync();
    }

    private void FlushDeferredExternalPreviewRefresh()
    {
        if (!_deferredPreviewRefresh)
            return;

        if (_compositionCrossfade.IsAnimating)
            return;

        _deferredPreviewRefresh = false;
        if (!IsLoaded || _loading || SettingsInteractionDeferral.IsSliderGestureActive)
            return;

        RefreshCompositionPreviewStatesFromRegistry();
    }

    private void HookEnvironmentWatcher()
    {
        if (_environmentChangedHandler is not null)
            return;

        if (DesktopEnvironmentWatcher.Current is not { } watcher)
        {
            DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, HookEnvironmentWatcher);
            return;
        }

        _environmentChangedHandler = (_, e) =>
            DispatcherQueue.TryEnqueue(() => OnDesktopEnvironmentChanged(e));
        watcher.Changed += _environmentChangedHandler;
    }

    private void UnhookEnvironmentWatcher()
    {
        if (_environmentChangedHandler is null || DesktopEnvironmentWatcher.Current is not { } watcher)
            return;

        watcher.Changed -= _environmentChangedHandler;
        _environmentChangedHandler = null;
    }

    private void OnDesktopEnvironmentChanged(DesktopEnvironmentChangedEventArgs e)
    {
        if (!IsLoaded)
            return;

        if (e.WallpaperChanged)
            ScheduleWallpaperRefresh();
        if (e.ThemeChanged)
            ScheduleThemeReload();
        if (e.AccentChanged)
            ScheduleExternalPreviewRefresh();
    }

    private void ScheduleWallpaperRefresh()
    {
        _wallpaperRefreshTimer ??= DispatcherQueue.CreateTimer();
        _wallpaperRefreshTimer.Interval = TimeSpan.FromMilliseconds(120);
        _wallpaperRefreshTimer.IsRepeating = false;
        _wallpaperRefreshTimer.Tick -= OnWallpaperRefreshTick;
        _wallpaperRefreshTimer.Tick += OnWallpaperRefreshTick;
        _wallpaperRefreshTimer.Stop();
        _wallpaperRefreshTimer.Start();
    }

    private void OnWallpaperRefreshTick(DispatcherQueueTimer sender, object args)
    {
        sender.Tick -= OnWallpaperRefreshTick;
        _ = RefreshCompositionWallpaperAsync();
    }

    private async Task RefreshCompositionWallpaperAsync()
    {
        if (!IsLoaded || _loading)
            return;

        DesktopWallpaperImageCache.NotifyWallpaperChanged(DispatcherQueue);
        var load = DesktopWallpaperImageCache.LoadTask;
        if (load is not null)
            await load.ConfigureAwait(true);

        CompositionWallpaperLayersCache.InvalidateAndPrewarmIfNeeded(DispatcherQueue);
        var prewarm = CompositionWallpaperLayersCache.PrewarmTask;
        if (prewarm is not null)
            await prewarm.ConfigureAwait(true);

        if (!IsLoaded || _loading)
            return;

        VistaCompositionPreview.RefreshWallpaperLayer();
        Win7CompositionPreview.RefreshWallpaperLayer();
    }

    private void ScheduleThemeReload()
    {
        _themeReloadTimer ??= DispatcherQueue.CreateTimer();
        _themeReloadTimer.Interval = TimeSpan.FromMilliseconds(1000);
        _themeReloadTimer.IsRepeating = false;
        _themeReloadGeneration++;
        _themeReloadTimer.Tick -= OnThemeReloadTick;
        _themeReloadTimer.Tick += OnThemeReloadTick;
        _themeReloadTimer.Stop();
        _themeReloadTimer.Start();
    }

    private void OnThemeReloadTick(DispatcherQueueTimer sender, object args)
    {
        sender.Tick -= OnThemeReloadTick;
        var generation = _themeReloadGeneration;
        if (!IsLoaded || generation != _themeReloadGeneration)
            return;

        if (_loading || SettingsInteractionDeferral.IsSliderGestureActive)
        {
            ScheduleThemeReload();
            return;
        }

        // Политика DWM (delete Override → Primary sync) — только в GlassCompat host.
        Reload();
        UpdateCompositionHeroLayout();
    }

    private void ScheduleExternalPreviewRefresh()
    {
        if (!IsLoaded)
            return;

        _externalPreviewTimer ??= DispatcherQueue.CreateTimer();
        _externalPreviewTimer.Interval = TimeSpan.FromMilliseconds(450);
        _externalPreviewTimer.IsRepeating = false;
        _externalPreviewTimer.Tick -= OnExternalPreviewTick;
        _externalPreviewTimer.Tick += OnExternalPreviewTick;
        _externalPreviewTimer.Stop();
        _externalPreviewTimer.Start();
    }

    private void CancelExternalPreviewRefresh()
    {
        if (_externalPreviewTimer is null)
            return;

        _externalPreviewTimer.Stop();
        _externalPreviewTimer.Tick -= OnExternalPreviewTick;
    }

    private void OnExternalPreviewTick(DispatcherQueueTimer sender, object args)
    {
        sender.Tick -= OnExternalPreviewTick;
        if (!IsLoaded || _loading || SettingsInteractionDeferral.IsSliderGestureActive)
            return;

        // Host уже применил политику; GS — только reload пикеров/слайдеров из реестра.
        Reload();
    }

    private void ConfigureStaticControls()
    {
        var colorGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.GlassColor);
        var opacityGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.Opacity);

        ActiveColorExpander.IconGlyph = colorGlyph;
        ActiveColorPicker.UseLivePreview = true;
        ActiveColorPicker.ColorLivePreview += ActiveColorPicker_OnColorLivePreview;
        ActiveColorPicker.ColorChanged += ActiveColorPicker_OnColorChanged;

        SecondaryColorExpander.IconGlyph = colorGlyph;
        SecondaryColorPicker.UseLivePreview = true;
        SecondaryColorPicker.ColorLivePreview += SecondaryColorPicker_OnColorLivePreview;
        SecondaryColorPicker.ColorChanged += SecondaryColorPicker_OnColorChanged;

        BaseColorExpander.IconGlyph = colorGlyph;
        BaseColorExpander.Title = GlassSettingsResources.Get("Dwm_BaseColorSpoilerTitle");
        BaseColorPicker.UseLivePreview = true;
        BaseColorPicker.ColorLivePreview += BaseColorPicker_OnColorLivePreview;
        BaseColorPicker.ColorChanged += BaseColorPicker_OnColorChanged;

        AdvancedExpander.Title = GlassSettingsResources.Get("Dwm_AdvancedColorizationTitle");
        AdvancedExpander.UnsavedScope = DwmColorizationScopeCatalog.AdvancedScope(_hive);
        ApplyExpanderUnsavedScopesForTab(GlassTypePivot.SelectedIndex);
        ApplyBaseColorExpanderUnsavedScope(BaseColorPivot.SelectedIndex);
    }

    private void ConfigurePivotHeaders()
    {
        for (var i = 0; i < GlassTypePivotHeaders.Length && i < GlassTypePivot.Items.Count; i++)
        {
            var header = SettingsPivotTabHeader.Create(GlassSettingsResources.Get(GlassTypePivotHeaders[i]));
            _glassTypeTabHeaders[i] = header;
            ((PivotItem)GlassTypePivot.Items[i]).Header = header.Root;
        }

        for (var i = 0; i < BaseColorTabs.Length && i < BaseColorPivot.Items.Count; i++)
        {
            var header = SettingsPivotTabHeader.Create(GlassSettingsResources.Get(BaseColorPivotHeaders[i]));
            _baseColorTabHeaders[i] = header;
            ((PivotItem)BaseColorPivot.Items[i]).Header = header.Root;
        }

        for (var i = 0; i < BaseOpacityTabSpecs.Length && i < BaseOpacityPivot.Items.Count; i++)
        {
            var header = SettingsPivotTabHeader.Create(GlassSettingsResources.Get(BaseOpacityPivotHeaders[i]));
            _baseOpacityTabHeaders[i] = header;
            ((PivotItem)BaseOpacityPivot.Items[i]).Header = header.Root;
        }

        _lastGlassTypePivotIndex = GlassTypePivot.SelectedIndex;
        _lastBaseColorPivotIndex = BaseColorPivot.SelectedIndex;
        _lastBaseOpacityPivotIndex = BaseOpacityPivot.SelectedIndex;
        ApplyBaseColorTabLabels();
        ApplyBaseOpacityTabLabels();
        ApplyActiveColorLabels(GlassTypePivot.SelectedIndex);
        ApplySecondaryColorLabels(GlassTypePivot.SelectedIndex);
        ApplyActiveBlurSliderVisibility(GlassTypePivot.SelectedIndex);
        RefreshPivotTabUnsavedIndicators();
    }

    private void RefreshPivotTabUnsavedIndicators()
    {
        RefreshGlassTypePivotUnsavedIndicators();
        RefreshGlassTypePivotParallelBranchMarkers();

        for (var i = 0; i < _baseColorTabHeaders.Length; i++)
        {
            _baseColorTabHeaders[i].SetUnsavedVisible(
                SettingsSession.Current.HasUnsavedInScope(DwmColorizationScopeCatalog.BaseColorTabScope(i, _hive)));
        }

        for (var i = 0; i < _baseOpacityTabHeaders.Length; i++)
        {
            _baseOpacityTabHeaders[i].SetUnsavedVisible(
                SettingsSession.Current.HasUnsavedInScope(DwmColorizationScopeCatalog.BaseOpacityTabScope(i, _hive)));
        }
    }

    private void RefreshGlassTypePivotUnsavedIndicators()
    {
        var glassTypeDirty = SettingsSession.Current.HasUnsavedInScope(DwmColorizationScopeCatalog.GlassTypeScope(_hive));
        var win7Effective = IsWin7GlassType();
        for (var i = 0; i < _glassTypeTabHeaders.Length; i++)
        {
            var tabIsWin7 = i == 1;
            _glassTypeTabHeaders[i].SetUnsavedVisible(glassTypeDirty && tabIsWin7 == win7Effective);
        }
    }

    private void RefreshGlassTypePivotParallelBranchMarkers()
    {
        var parallelHive = DwmRegistryStore.Opposite(_hive);
        var hasParallelValue = DwmRegistryStore.ValueExists(parallelHive, "GlassType");
        var parallelWin7 = hasParallelValue
            && (DwmRegistryStore.ReadDword(parallelHive, "GlassType") ?? 0) == 1;
        var marker = DwmParallelBranchUi.ResolveMarker(_hive);
        var tooltip = DwmParallelBranchUi.ResolveTooltip(_hive);

        for (var i = 0; i < _glassTypeTabHeaders.Length; i++)
        {
            var tabIsWin7 = i == 1;
            _glassTypeTabHeaders[i].SetParallelBranchWarning(
                hasParallelValue && tabIsWin7 == parallelWin7,
                marker,
                tooltip);
        }
    }

    private void ApplyExpanderUnsavedScopesForTab(int glassTypeTabIndex)
    {
        if (IsWin7Tab(glassTypeTabIndex))
        {
            ActiveColorExpander.UnsavedScope = DwmColorizationScopeCatalog.ActiveColorWin7Scope(_hive);
            SecondaryColorExpander.UnsavedScope = DwmColorizationScopeCatalog.SecondaryColorWin7Scope(_hive);
        }
        else
        {
            ActiveColorExpander.UnsavedScope = DwmColorizationScopeCatalog.ActiveColorVistaScope(_hive);
            SecondaryColorExpander.UnsavedScope = DwmColorizationScopeCatalog.SecondaryColorVistaScope(_hive);
        }

        ActiveColorExpander.RefreshUnsavedIndicator();
        SecondaryColorExpander.RefreshUnsavedIndicator();
    }

    private void ApplyBaseColorExpanderUnsavedScope(int tabIndex)
    {
        BaseColorExpander.UnsavedScope = DwmColorizationScopeCatalog.BaseColorTabScope(tabIndex, _hive);
        BaseColorExpander.RefreshUnsavedIndicator();
    }

    private void BuildCompositionTopControls()
    {
        OverrideAccentToggle.Toggled += (_, _) => OnOverrideAccentToggleChanged(OverrideAccentToggle.IsOn);
        OverrideAccentCard.IconGlyphChar = SettingsSectionGlyphs.GlassColor;
        OverrideAccentCard.Title = GlassSettingsResources.Get("Dwm_OverrideAccentTitle");
        OverrideAccentCard.Description = GlassSettingsResources.Get("Dwm_OverrideAccentDesc");
        OverrideAccentCard.Click += (_, _) => OverrideAccentToggle.IsOn = !OverrideAccentToggle.IsOn;

        OpaqueBlendToggle.Toggled += (_, _) => OnOpaqueBlendToggleChanged(OpaqueBlendToggle.IsOn);
        OpaqueBlendCard.IconGlyphChar = SettingsSectionGlyphs.Opacity;
        OpaqueBlendCard.Title = GlassSettingsResources.Get("Dwm_OpaqueBlendTitle");
        OpaqueBlendCard.Description = GlassSettingsResources.Get("Dwm_OpaqueBlendDesc");
        OpaqueBlendCard.Click += (_, _) => OpaqueBlendToggle.IsOn = !OpaqueBlendToggle.IsOn;
    }

    private void BuildActiveColorBlurSlider()
    {
        var labelStyle = Application.Current.Resources["GlassSettingsSpoilerSecondaryTextStyle"] as Style;
        _activeColorBlurFooter = SettingsPickerBodyLayout.CreateBodyTable();

        _blurRadiusSlider = SettingsSliderFactory.Create(BlurRadiusMin, BlurRadiusMax, stretch: true);
        _blurRadiusSlider.StepFrequency = 1;
        _blurRadiusSlider.SmallChange = 1;
        _blurRadiusSlider.LargeChange = 2;
        _blurRadiusSlider.LostFocus += BlurRadiusSlider_OnLostFocus;

        _blurRadiusValueText = new TextBlock
        {
            Style = labelStyle,
            VerticalAlignment = VerticalAlignment.Center
        };

        SettingsPickerBodyLayout.AddLabeledRow(
            _activeColorBlurFooter,
            GlassSettingsResources.Get("Dwm_BlurRadiusTitle"),
            _blurRadiusSlider,
            labelStyle);
        SettingsPickerBodyLayout.AddTrailingContent(
            _activeColorBlurFooter,
            row: 0,
            trailing: _blurRadiusValueText,
            leftGap: 8);

        SettingsSliderCommit.Attach(
            _blurRadiusSlider,
            () => !_loading && ActiveColorExpander.IsGateOn && !IsD3DRenderingEnabled(),
            v =>
            {
                var radius = ClampBlurRadius((int)Math.Round(v));
                _blurRadiusValueText!.Text = radius.ToString();
                RefreshCompositionPreview(CompositionPreviewRefreshScope.LiveSlider);
            },
            value =>
            {
                // Live: запись/реестр на фоне; миниатюра обновляется сразу по onPreview.
                var radius = ClampBlurRadius((int)Math.Round(value));
                WriteBlurDeviationLivePreview(BlurRadiusToDeviation(radius));
            },
            SettingsSliderCommit.CommitMode.LiveImmediate,
            onGestureCommitted: CommitBlurRadiusGesture,
            liveDebounceMs: SettingsSliderCommit.LivePreviewDebounceMs);

        _blurBalanceSlider = SettingsSliderFactory.Create(stretch: true);
        _blurBalanceValueText = new TextBlock
        {
            Style = labelStyle,
            VerticalAlignment = VerticalAlignment.Center
        };
        _blurBalanceLabel = new TextBlock
        {
            Text = GlassSettingsResources.Get("Dwm_BlurLevelTitle"),
            Style = labelStyle
        };

        SettingsPickerBodyLayout.AddLabeledRow(
            _activeColorBlurFooter,
            _blurBalanceLabel,
            _blurBalanceSlider,
            topMargin: true);
        SettingsPickerBodyLayout.AddTrailingContent(
            _activeColorBlurFooter,
            row: 1,
            trailing: _blurBalanceValueText,
            topMargin: true,
            leftGap: 8);

        ActiveColorPanel.FooterContent = _activeColorBlurFooter;
        ActiveColorExpander.WireExpandedLayoutRefresh(() =>
        {
            ActiveColorPanel.RefreshLayout();
            if (_activeColorBlurFooter is not null)
                SettingsSliderFactory.UpdateLabeledSliderValueSuffixRowLayout(_activeColorBlurFooter);
        });

        SettingsSliderCommit.Attach(
            _blurBalanceSlider,
            () => !_loading && ActiveColorExpander.IsGateOn && IsWin7Tab(GlassTypePivot.SelectedIndex),
            v =>
            {
                _blurBalanceValueText!.Text = GlassSettingsResources.Format("Dwm_ReflectionPercentFormat", (uint)v);
                RefreshCompositionPreview(CompositionPreviewRefreshScope.LiveSlider);
            },
            v => TryWriteBlurBalanceLivePreview((uint)Math.Round(v)),
            SettingsSliderCommit.CommitMode.LiveImmediate,
            onGestureCommitted: CommitBlurBalanceGesture,
            liveDebounceMs: SettingsSliderCommit.LivePreviewDebounceMs);
    }

    private void BuildBaseColorHeader()
    {
        _baseColorModeCombo = new ComboBox { MinWidth = 120 };
        _baseColorModeCombo.Items.Add(GlassSettingsResources.Get("Dwm_ModeAuto"));
        _baseColorModeCombo.Items.Add(GlassSettingsResources.Get("Dwm_ModeFromTheme"));
        _baseColorModeCombo.Items.Add(GlassSettingsResources.Get("Dwm_ModeManual"));
        SettingsInteractionDeferral.WhenComboCommit(_baseColorModeCombo, () => !_loading, OnBaseColorModeChanged);

        BaseColorExpander.HeaderActions = new HubInlineLabel
        {
            Label = GlassSettingsResources.Get("Dwm_LabelMode"),
            Control = _baseColorModeCombo
        };
    }

    private void BuildBaseOpacityRows()
    {
        var opacityGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.Opacity);
        _baseOpacityActiveRow = CreateBaseOpacityRow(opacityGlyph);
        _baseOpacityInactiveRow = CreateBaseOpacityRow(opacityGlyph);
        BaseOpacityRowsPanel.Children.Add(_baseOpacityActiveRow);
        BaseOpacityRowsPanel.Children.Add(_baseOpacityInactiveRow);
    }

    private DwmOpacityGroupRow CreateBaseOpacityRow(string iconGlyph)
    {
        var row = new DwmOpacityGroupRow
        {
            IconGlyph = iconGlyph,
            UseOpenGlassOpacitySemantics = true,
            UseOpenGlassModeWithoutGate = true,
            UseSliderValueSuffix = true,
            Hive = _hive
        };
        row.BuildControls();
        return row;
    }

    private void BuildPriorityRow()
    {
        PriorityRow.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.GlassColor);
        PriorityRow.Title = GlassSettingsResources.Get("Dwm_LabelOpaqueBlendPriority");
        PriorityRow.Description = GlassSettingsResources.Get("Dwm_OpaqueBlendPriorityDesc");
        PriorityRow.UnsavedScope = DwmColorizationScopeCatalog.PriorityScope(_hive);

        _priorityCombo = new ComboBox { MinWidth = 160 };
        _priorityCombo.Items.Add(GlassSettingsResources.Get("Dwm_GlassTypeVista"));
        _priorityCombo.Items.Add(GlassSettingsResources.Get("Dwm_GlassTypeWin7"));
        _priorityCombo.Items.Add(GlassSettingsResources.Get("Dwm_ModeAuto"));
        SettingsInteractionDeferral.WhenComboCommit(_priorityCombo, () => !_loading, OnPriorityComboChanged);
        PriorityRow.HeaderActions = _priorityCombo;
    }

    // ─── Alpha mapping ────────────────────────────────────────────────────────

    private static uint AlphaToPercent(byte alpha) => (uint)((alpha * 100u + 127u) / 255u);

    private static byte PercentToAlpha(uint percent) => (byte)((percent * 255u + 50u) / 100u);

    private static uint ComposePickerArgb(uint colorDword, uint percent) =>
        DwmCaptionColorHelper.ComposePickerArgbFromDwmBgr(colorDword, percent);

    private static uint ExtractRgb(uint argb) => argb & 0x00FFFFFFu;

    private static byte ExtractAlphaByte(uint argb) => (byte)((argb >> 24) & 0xFF);

    private static uint BlurRadiusToDeviation(int radius)
    {
        var clampedRadius = ClampBlurRadius(radius);
        var value = (clampedRadius * 10 + 2) / 3;
        return (uint)Math.Clamp(value, 0, 100);
    }

    private static int BlurDeviationToRadius(uint deviation)
    {
        var value = ((int)deviation * 3 + 5) / 10;
        return ClampBlurRadius(value);
    }

    private static int ClampBlurRadius(int radius) => Math.Clamp(radius, BlurRadiusMin, BlurRadiusMax);

    private bool IsD3DRenderingEnabled() =>
        (DwmRegistryStore.ReadDwordForPage(_hive, "UseDirect3DRendering") ?? 0) != 0;

    private void ReloadCompositionTopControls()
    {
        ReloadOverrideAccentToggle();
        ReloadBlurRadiusSlider();
    }

    private void ReloadOverrideAccentToggle()
    {
        OverrideAccentToggle.IsOn = (DwmRegistryStore.ReadDwordForPage(_hive, "GlassOverrideAccent") ?? 0) != 0;
    }

    private void ReloadBlurRadiusSlider()
    {
        if (_blurRadiusSlider is null)
            return;

        var d3d = IsD3DRenderingEnabled();
        var deviation = DwmRegistryStore.ReadDwordForPage(_hive, BlurDeviationKey) ?? BlurDeviationDefault;
        var radius = d3d
            ? BlurRadiusD3DValue
            : BlurDeviationToRadius(Math.Min(deviation, 100u));

        _blurRadiusSlider.Minimum = BlurRadiusMin;
        _blurRadiusSlider.Maximum = BlurRadiusMax;
        _blurRadiusSlider.Value = ClampBlurRadius(radius);
        UpdateBlurRadiusSliderEnabled();
        if (_blurRadiusValueText is not null)
            _blurRadiusValueText.Text = ((int)_blurRadiusSlider.Value).ToString();
        SettingsSliderCommit.SyncCommittedValue(_blurRadiusSlider, _blurRadiusSlider.Value);
    }

    private void WriteBlurDeviationLivePreview(uint deviation)
    {
        if (deviation == BlurDeviationDefault)
            SettingsSession.Current.DeleteDwmValueLivePreview(_hive, BlurDeviationKey);
        else
            SettingsSession.Current.WriteDwmDwordLivePreview(_hive, BlurDeviationKey, deviation);
    }

    private void BlurRadiusSlider_OnLostFocus(object sender, RoutedEventArgs e)
    {
        if (SettingsInteractionDeferral.IsSliderGestureActive)
            return;

        CommitBlurRadiusGesture();
    }

    private void CommitBlurRadiusGesture()
    {
        if (_loading || _blurRadiusSlider is null || !ActiveColorExpander.IsGateOn || IsD3DRenderingEnabled())
            return;

        CancelExternalPreviewRefresh();

        var radius = ClampBlurRadius((int)Math.Round(_blurRadiusSlider.Value));
        WriteBlurDeviationLivePreview(BlurRadiusToDeviation(radius));
        SettingsSession.Current.MarkDirty();
        RefreshPivotTabUnsavedIndicators();
        RefreshCompositionPreview(CompositionPreviewRefreshScope.Full);
    }

    private void CommitBlurBalanceGesture()
    {
        if (_loading || _blurBalanceSlider is null)
            return;

        if (!ActiveColorExpander.IsGateOn || !IsWin7Tab(GlassTypePivot.SelectedIndex))
            return;

        TryWriteBlurBalanceLivePreview((uint)Math.Round(_blurBalanceSlider.Value));
        SettingsSession.Current.MarkDirty();
        RefreshCompositionPreview(CompositionPreviewRefreshScope.Full);
    }

    private async void OnOverrideAccentToggleChanged(bool enabled)
    {
        if (_loading)
            return;

        await WriteCompositionBoolToggleAsync(OverrideAccentKey, enabled).ConfigureAwait(true);
    }

    private async Task WriteCompositionBoolToggleAsync(string key, bool enabled)
    {
        await PageShell.Operations.RunSequenceAsync(async seq =>
        {
            CancelExternalPreviewRefresh();

            await seq.RegistryMutationAsync(async () =>
            {
                if (enabled)
                    SettingsSession.Current.WriteDwmDword(_hive, key, 1, liveDwm: true, dwmNotifyImmediate: true);
                else
                    SettingsSession.Current.DeleteDwmValue(_hive, key, liveDwm: true);

                await Task.CompletedTask.ConfigureAwait(false);
            }).ConfigureAwait(true);

            await seq.CosmeticAsync(() =>
            {
                RefreshCompositionPreview();
                RefreshPivotTabUnsavedIndicators();
            }).ConfigureAwait(true);

            await seq.NoticeSyncAsync(() => PageShell.FlushSessionNotice()).ConfigureAwait(true);
        }).ConfigureAwait(true);
    }

    public void ReloadFromExternalRegistrySync() => Reload();

    private void RefreshCompositionPreview(
        CompositionPreviewRefreshScope scope = CompositionPreviewRefreshScope.Full,
        bool animateModeChange = false)
    {
        var win7Runtime = IsRuntimeWin7GlassType();
        var vistaState = BuildVistaPreviewState();
        var win7State = BuildWin7PreviewState();
        RefreshCompositionPreviewStates(vistaState, win7State, scope);
        SetCompositionVisibleMode(win7Runtime, animateModeChange);
    }

    private async Task RefreshCompositionPreviewAsync(
        CompositionPreviewRefreshScope scope = CompositionPreviewRefreshScope.Full,
        bool animateModeChange = false)
    {
        var win7Runtime = IsRuntimeWin7GlassType();
        var vistaState = BuildVistaPreviewState();
        var win7State = BuildWin7PreviewState();
        RefreshCompositionPreviewStates(vistaState, win7State, scope);
        await SetCompositionVisibleModeAsync(win7Runtime, animateModeChange).ConfigureAwait(true);
    }

    private void RefreshCompositionPreviewStates(
        DwmCompositionCascadePreview.PreviewState vistaState,
        DwmCompositionCascadePreview.PreviewState win7State,
        CompositionPreviewRefreshScope scope)
    {
        var skipVistaBlur = scope == CompositionPreviewRefreshScope.LiveSlider && _visibleWin7Preview;
        var skipWin7Blur = scope == CompositionPreviewRefreshScope.LiveSlider && !_visibleWin7Preview;
        VistaCompositionPreview.ApplyState(vistaState, skipVistaBlur);
        Win7CompositionPreview.ApplyState(win7State, skipWin7Blur);
    }

    private void SetCompositionVisibleMode(bool win7, bool animate)
    {
        if (!animate && _compositionCrossfade.IsAnimating)
            return;

        if (_hasVisiblePreviewMode && _visibleWin7Preview == win7)
            return;

        var shouldAnimate = animate && _hasVisiblePreviewMode;
        _visibleWin7Preview = win7;
        _hasVisiblePreviewMode = true;
        _compositionCrossfade.ShowFront(layerAIsFront: !win7, shouldAnimate);
    }

    private Task SetCompositionVisibleModeAsync(bool win7, bool animate)
    {
        if (!animate && _compositionCrossfade.IsAnimating)
            return Task.CompletedTask;

        if (_hasVisiblePreviewMode && _visibleWin7Preview == win7)
            return Task.CompletedTask;

        var shouldAnimate = animate && _hasVisiblePreviewMode;
        _visibleWin7Preview = win7;
        _hasVisiblePreviewMode = true;
        return _compositionCrossfade.ShowFrontAsync(layerAIsFront: !win7, shouldAnimate);
    }

    private Task PrepareCompositionPreviewPivotAsync(int pivotIndex, bool animate)
    {
        RefreshCompositionPreviewStatesFromRegistry();
        return SetCompositionVisibleModeAsync(
            ResolveRuntimeWin7ForPendingGlassType(pivotIndex),
            animate);
    }

    private void RefreshVistaInactiveSplitPreview()
    {
        VistaCompositionPreview.ApplyInactiveSplitLayout(BuildVistaPreviewState());
    }

    private DwmCompositionCascadePreview.PreviewState BuildVistaPreviewState()
    {
        var opaque = OpaqueBlendToggle.IsOn;
        var blurRadius = ClampBlurRadius((int)Math.Round(_blurRadiusSlider?.Value ?? BlurRadiusD3DValue));
        var blurBalancePercent = ReadPreviewBlurBalancePercent();
        var tint = ActiveColorExpander.IsGateOn
            ? ActiveColorPicker.GetColorArgb()
            : ComposePickerArgb(
                VistaGlassColorPolicy.ReadRuntimeFactualActiveColorRgb(),
                VistaGlassColorPolicy.ReadRuntimeFactualGlassOpacityPercent());
        var showSplit = IsInactiveGateOn();
        var inactiveTint = showSplit
            && SecondaryColorExpander.IsGateOn
            && !IsWin7Tab(GlassTypePivot.SelectedIndex)
            ? SecondaryColorPicker.GetColorArgb()
            : ResolveVistaInactiveTintArgb(opaque);

        return new DwmCompositionCascadePreview.PreviewState(
            CurrentWallpaperResolver.TryGetDesktopWallpaperPath(),
            tint,
            Win7Mode: false,
            GlowArgb: 0,
            ShowGlow: false,
            blurBalancePercent,
            opaque,
            blurRadius,
            Win7TintBgr: 0,
            Win7ColorBalancePercent: 0,
            Win7AfterglowBgr: 0,
            Win7AfterglowBalancePercent: 0,
            showSplit,
            inactiveTint);
    }

    private DwmCompositionCascadePreview.PreviewState BuildWin7PreviewState()
    {
        var opaque = OpaqueBlendToggle.IsOn;
        var blurRadius = ClampBlurRadius((int)Math.Round(_blurRadiusSlider?.Value ?? BlurRadiusD3DValue));
        var blurBalancePercent = ReadPreviewBlurBalancePercent();
        var tint = ActiveColorExpander.IsGateOn
            ? ActiveColorPicker.GetColorArgb()
            : ComposePickerArgb(
                Win7GlassColorPolicy.ReadRuntimeFactualActiveColorRgb(),
                Win7GlassColorPolicy.ReadRuntimeFactualActiveBalancePercent());
        var (previewAfterglowBgr, previewAfterglowBalance) = ResolveWin7AfterglowPreviewParams();
        var glow = Win7GlassColorPolicy.ToPickerArgbFromBgr(previewAfterglowBgr, previewAfterglowBalance);
        ResolveWin7PreviewParams(
            win7: true,
            out var win7TintBgr,
            out var win7ColorBalancePercent,
            out var win7AfterglowBgr,
            out var win7AfterglowBalancePercent);

        return new DwmCompositionCascadePreview.PreviewState(
            CurrentWallpaperResolver.TryGetDesktopWallpaperPath(),
            tint,
            Win7Mode: true,
            glow,
            ShowGlow: true,
            blurBalancePercent,
            opaque,
            blurRadius,
            win7TintBgr,
            win7ColorBalancePercent,
            win7AfterglowBgr,
            win7AfterglowBalancePercent,
            ShowInactiveSplit: false,
            InactiveTintArgb: 0);
    }

    private void RefreshCompositionPreviewStatesFromRegistry()
    {
        var (vistaState, win7State) = CompositionPreviewRegistryStates.BuildFromRuntimeEffective();
        RefreshCompositionPreviewStates(vistaState, win7State, CompositionPreviewRefreshScope.Full);
    }

    private void RefreshCompositionPreviewFromRegistry()
    {
        RefreshCompositionPreviewStatesFromRegistry();
        if (!_compositionCrossfade.IsAnimating)
            SetCompositionVisibleMode(IsRuntimeWin7GlassType(), animate: false);
    }

    /// <summary>Win7 compositor: BGR + balance percents (как GlassRenderer pre-scale), не ARGB пикера.</summary>
    private void ResolveWin7PreviewParams(
        bool win7,
        out uint tintBgr,
        out uint colorBalancePercent,
        out uint afterglowBgr,
        out uint afterglowBalancePercent)
    {
        if (!win7)
        {
            tintBgr = 0;
            colorBalancePercent = 0;
            afterglowBgr = 0;
            afterglowBalancePercent = 0;
            return;
        }

        if (ActiveColorExpander.IsGateOn)
        {
            var argb = ActiveColorPicker.GetColorArgb();
            tintBgr = argb & 0x00FFFFFFu;
            colorBalancePercent = AlphaToPercent(ExtractAlphaByte(argb));
        }
        else
        {
            tintBgr = Win7GlassColorPolicy.ReadFactualActiveColorRgb(_hive);
            colorBalancePercent = Win7GlassColorPolicy.ReadFactualActiveBalancePercent(_hive);
        }

        (afterglowBgr, afterglowBalancePercent) = ResolveWin7AfterglowPreviewParams();
    }

    private (uint BgrRgb, uint BalancePercent) ResolveWin7AfterglowPreviewParams() =>
        Win7GlassColorPolicy.IsAfterglowGateOn(_hive)
            ? (
                Win7GlassColorPolicy.ReadFactualAfterglowColorRgb(_hive),
                Win7GlassColorPolicy.ReadUiAfterglowBalancePercent(_hive))
            : Win7GlassColorPolicy.ResolveRuntimePreviewAfterglow();

    private void UpdateCompositionHeroLayout()
    {
        var hostWidth = CompositionHeroLayoutHost.ActualWidth;
        if (hostWidth <= 0)
            return;

        EnsureQuickActionsColumnWidths();
        if (_quickActionsMinTableWidth <= 0)
            return;

        var availableRight = hostWidth
            - HeroPreviewLayoutMetrics.FootprintWidth
            - CompositionHeroColumnGap;

        var controlsColWidth = Math.Max(availableRight, CompositionControlsColumn.MinWidth);

        // A = max(pref0, pref1) — max card width
        // B = minColWidth — min card width (text clipped)
        // C = minTableWidth = 2*B + gap — min width for 2 columns
        // D = B — min width for 1 column
        var thresholdCompact = _quickActionsMinTableWidth;       // C
        var thresholdUncompact = _quickActionsMinTableWidth + 1; // C + 1
        var thresholdStack = _quickActionsMinCol0Width;          // D
        var thresholdUnstack = _quickActionsMinCol0Width + 1;    // D + 1

        // Cards available width depends on stacked or side-by-side
        var cardsWidth = _compositionHeroSideBySide ? controlsColWidth : hostWidth;

        bool stacked = _compositionHeroSideBySide ? false : true;
        bool compact = _quickActionCompactMode;

        if (!_compositionLayoutInitialized)
        {
            // First layout: pick narrowest state that fits
            if (controlsColWidth >= _quickActionsPreferredTableWidth)
            {
                stacked = false; compact = false; // side-by-side, full
            }
            else if (cardsWidth >= thresholdCompact)
            {
                stacked = false; compact = false; // side-by-side, * cols shrink
            }
            else if (cardsWidth >= thresholdStack)
            {
                stacked = false; compact = true; // compact side-by-side
            }
            else if (hostWidth >= thresholdStack)
            {
                stacked = true; compact = false; // stacked, full width
            }
            else
            {
                stacked = true; compact = true; // stacked compact
            }
        }
        else
        {
            // Shrink: detect C-1 and D-1 on cardsWidth
            if (cardsWidth < thresholdCompact)
            {
                compact = true;
            }

            if (compact && cardsWidth < thresholdStack)
            {
                stacked = true;
                compact = false; // reset compact for stacked (recalc below)
            }

            // Recalc cardsWidth for stacked
            if (stacked)
            {
                cardsWidth = hostWidth;
                if (cardsWidth < thresholdCompact)
                {
                    compact = true;
                }
            }

            // Grow: detect C+1 and D+1 on cardsWidth
            // When stacked, first uncompact, then unstack
            if (stacked && compact)
            {
                // stacked compact → stacked non-compact
                if (cardsWidth >= thresholdUncompact)
                {
                    compact = false;
                }
            }
            else if (stacked && !compact)
            {
                // stacked non-compact → side-by-side compact
                if (controlsColWidth >= thresholdUnstack)
                {
                    stacked = false;
                    cardsWidth = controlsColWidth;
                    // Check if still compact in side-by-side
                    if (cardsWidth < thresholdCompact)
                    {
                        compact = true;
                    }
                    else
                    {
                        compact = false;
                    }
                }
            }
            else if (!stacked && compact)
            {
                // side-by-side compact → side-by-side non-compact
                if (cardsWidth >= thresholdUncompact)
                {
                    compact = false;
                }
            }
            else
            {
                // side-by-side non-compact: already full
            }
        }

        ApplyCompositionHeroPlacement(stacked: stacked, compact: compact);
    }

    private void ApplyCompositionHeroPlacement(bool stacked, bool compact = false)
    {
        var sideBySide = !stacked;
        if (_compositionLayoutInitialized && _compositionHeroSideBySide == sideBySide && _quickActionCompactMode == compact)
            return;

        _compositionHeroSideBySide = sideBySide;
        _quickActionCompactMode = compact;
        _compositionLayoutInitialized = true;

        if (sideBySide)
        {
            var availableRight = CompositionHeroLayoutHost.ActualWidth
                - HeroPreviewLayoutMetrics.FootprintWidth
                - CompositionHeroColumnGap;

            ApplyQuickActionColumns(availableRight, compact);

            CompositionPreviewColumn.Width = GridLength.Auto;
            CompositionControlsColumn.Width = new GridLength(1, GridUnitType.Star);

            Grid.SetColumn(CompositionPreviewHost, 0);
            Grid.SetRow(CompositionPreviewHost, 0);
            Grid.SetColumnSpan(CompositionPreviewHost, 1);

            Grid.SetColumn(QuickActionsHost, 1);
            Grid.SetRow(QuickActionsHost, 0);
            Grid.SetColumnSpan(QuickActionsHost, 1);
            CompositionPreviewHost.MinWidth = HeroPreviewLayoutMetrics.FootprintWidth;
            CompositionPreviewHost.HorizontalAlignment = HorizontalAlignment.Left;
            QuickActionsHost.VerticalAlignment = VerticalAlignment.Stretch;
            QuickActionsHost.MinHeight = HeroPreviewLayoutMetrics.OuterHeight;
        }
        else
        {
            var fullHostWidth = CompositionHeroLayoutHost.ActualWidth;
            ApplyQuickActionColumns(fullHostWidth, compact);

            CompositionPreviewColumn.Width = GridLength.Auto;
            CompositionControlsColumn.Width = new GridLength(1, GridUnitType.Star);

            Grid.SetColumn(CompositionPreviewHost, 0);
            Grid.SetRow(CompositionPreviewHost, 0);
            Grid.SetColumnSpan(CompositionPreviewHost, 2);

            Grid.SetColumn(QuickActionsHost, 0);
            Grid.SetRow(QuickActionsHost, 1);
            Grid.SetColumnSpan(QuickActionsHost, 2);
            CompositionPreviewHost.ClearValue(FrameworkElement.MinWidthProperty);
            CompositionPreviewHost.HorizontalAlignment = HorizontalAlignment.Left;
            QuickActionsHost.ClearValue(FrameworkElement.MinHeightProperty);
            QuickActionsHost.VerticalAlignment = VerticalAlignment.Top;
        }

        if (compact)
        {
            Grid.SetColumn(OpaqueBlendCard, 0);
            Grid.SetRow(OpaqueBlendCard, 1);
        }
        else
        {
            Grid.SetColumn(OpaqueBlendCard, 1);
            Grid.SetRow(OpaqueBlendCard, 0);
        }
    }

    private void ApplyQuickActionColumns(double availableWidth, bool compact)
    {
        QuickActionsHost.ColumnSpacing = compact ? 0 : QuickActionColumnGap;

        if (compact)
        {
            var compactMaxWidth = Math.Max(_quickActionsCol0Width, _quickActionsCol1Width);
            QuickActionCol0.Width = new GridLength(1, GridUnitType.Star);
            QuickActionCol0.MinWidth = _quickActionsMinCol0Width;
            QuickActionCol0.MaxWidth = compactMaxWidth;
            QuickActionCol1.Width = new GridLength(0, GridUnitType.Pixel);
            QuickActionCol1.MinWidth = 0;
            QuickActionCol1.MaxWidth = 0;

            QuickActionsHost.ClearValue(FrameworkElement.WidthProperty);
            QuickActionsHost.ClearValue(FrameworkElement.MinWidthProperty);
            QuickActionsHost.ClearValue(FrameworkElement.MaxWidthProperty);
            QuickActionsHost.HorizontalAlignment = HorizontalAlignment.Stretch;
        }
        else
        {
            QuickActionCol0.Width = new GridLength(1, GridUnitType.Star);
            QuickActionCol0.MinWidth = _quickActionsMinCol0Width;
            QuickActionCol0.MaxWidth = _quickActionsCol0Width;
            QuickActionCol1.Width = new GridLength(1, GridUnitType.Star);
            QuickActionCol1.MinWidth = _quickActionsMinCol1Width;
            QuickActionCol1.MaxWidth = _quickActionsCol1Width;

            QuickActionsHost.ClearValue(FrameworkElement.WidthProperty);
            QuickActionsHost.ClearValue(FrameworkElement.MinWidthProperty);
            QuickActionsHost.ClearValue(FrameworkElement.MaxWidthProperty);
            QuickActionsHost.HorizontalAlignment = HorizontalAlignment.Stretch;
        }
    }

    private void EnsureQuickActionsColumnWidths()
    {
        if (!_quickActionsWidthsDirty && _quickActionsMinTableWidth > 0)
            return;

        var pref0 = OverrideAccentCard.MeasurePreferredWidth();
        var pref1 = OpaqueBlendCard.MeasurePreferredWidth();
        var min0 = OverrideAccentCard.MeasureMinWidth();
        var min1 = OpaqueBlendCard.MeasureMinWidth();
        if (pref0 <= 0 || pref1 <= 0 || min0 <= 0 || min1 <= 0)
            return;

        _quickActionsCol0Width = pref0;
        _quickActionsCol1Width = pref1;
        _quickActionsMinCol0Width = Math.Min(min0, pref0);
        _quickActionsMinCol1Width = Math.Min(min1, pref1);
        _quickActionsPreferredTableWidth = pref0 + QuickActionColumnGap + pref1;
        _quickActionsMinTableWidth = _quickActionsMinCol0Width + QuickActionColumnGap + _quickActionsMinCol1Width;
        QuickActionsHost.ColumnSpacing = QuickActionColumnGap;
        _quickActionsWidthsDirty = false;
    }

    // ─── Reload ───────────────────────────────────────────────────────────────

    private void Reload()
    {
        _loading = true;
        try
        {
            SyncGlassTypePivotFromRegistry();
            ApplyActiveColorLabels(GlassTypePivot.SelectedIndex);
            ApplySecondaryColorLabels(GlassTypePivot.SelectedIndex);
            ApplyActiveBlurSliderVisibility(GlassTypePivot.SelectedIndex);
            ApplyExpanderUnsavedScopesForTab(GlassTypePivot.SelectedIndex);
            ApplyBaseColorExpanderUnsavedScope(BaseColorPivot.SelectedIndex);
            ReloadOpaqueBlend();
            ReloadActiveColor();
            ReloadSecondaryColor();
            ReloadActiveBlurSlider();
            ReloadBaseColorRow();
            ReloadPriorityRow();
            ReloadBaseOpacityRows();
            ReloadCompositionTopControls();
        }
        finally
        {
            _loading = false;
            RefreshPivotTabUnsavedIndicators();
            RefreshParallelBranchWarnings();
            _ = RefreshHkcuConflictNoticeAsync();
            RefreshCompositionPreview();
            UpdateCompositionHeroLayout();
        }
    }

    private void SyncGlassTypePivotFromRegistry()
    {
        var win7 = IsWin7GlassType();
        var index = win7 ? 1 : 0;
        _lastGlassTypePivotIndex = index;
        if (GlassTypePivot.SelectedIndex != index)
            GlassTypePivot.SelectedIndex = index;
    }

    private void ReloadOpaqueBlend()
    {
        OpaqueBlendToggle.IsOn = (DwmRegistryStore.ReadDwordForPage(_hive, "ColorizationOpaqueBlend") ?? 0) != 0;
    }

    private void ReloadActiveColor()
    {
        var win7 = IsWin7Tab(GlassTypePivot.SelectedIndex);
        var gateOn = win7
            ? Win7GlassColorPolicy.IsActiveGateOn(_hive)
            : VistaGlassColorPolicy.IsActiveGateOn(_hive);
        ActiveColorExpander.IsGateOn = gateOn;

        var colorDword = win7
            ? Win7GlassColorPolicy.ReadFactualActiveColorRgb(_hive)
            : VistaGlassColorPolicy.ReadFactualActiveColorRgb(_hive);
        var percent = win7
            ? Win7GlassColorPolicy.ReadUiActiveBalancePercent(_hive)
            : VistaGlassColorPolicy.ReadUiGlassOpacityPercent(_hive);
        var argb = ComposePickerArgb(colorDword, percent);
        ActiveColorPicker.SetColor(argb);
        SyncActivePickerTracking(argb, percent);

        ActiveColorPanel.SetDefaultColorActionState(
            gateOn,
            disableDefaultButton: gateOn && DwmColorRegistry.IsActiveColorAtDefaults(_hive, win7));
    }

    private uint ReadActiveOpacityPercent(int tabIndex)
    {
        if (IsWin7Tab(tabIndex))
            return ReadWin7ActiveBalancePercent();

        return VistaGlassColorPolicy.ReadUiGlassOpacityPercent(_hive);
    }

    private void ReloadSecondaryColor()
    {
        if (IsWin7Tab(GlassTypePivot.SelectedIndex))
            ReloadAfterglowSecondary();
        else
            ReloadInactiveSecondary();
    }

    private void ReloadInactiveSecondary()
    {
        if (_inactiveVistaGateArmed && HasInactiveColorKeyForPage())
            _inactiveVistaGateArmed = false;

        var gateOn = IsInactiveGateOn();
        SecondaryColorExpander.IsGateOn = gateOn;

        var inactiveValue = DwmRegistryStore.ReadDwordForPage(_hive, "ColorizationColorInactive");
        var hasInactive = inactiveValue is { } v && v != AutoColorSentinel && v != ThemeColorSentinel;
        var colorDword = hasInactive
            ? inactiveValue!.Value
            : VistaGlassColorPolicy.ReadOgFactualActiveColorRgb(_hive);
        var percent = ReadInactiveOpacityPercent(hasInactive);
        var argb = ComposePickerArgb(colorDword, percent);
        SecondaryColorPicker.SetColor(argb);
        SyncSecondaryPickerTracking(argb, percent);

        SecondaryColorPanel.SetDefaultColorActionState(
            gateOn,
            disableDefaultButton: gateOn && hasInactive && DwmColorRegistry.IsInactiveSecondaryAtDefaults(_hive));
    }

    private bool HasInactiveColorKeyForPage() =>
        VistaGlassColorPolicy.HasInactiveColorKey(_hive);

    private uint ResolveVistaInactiveTintArgb(bool opaqueBlend)
    {
        var inactiveValue = DwmRegistryStore.ReadDwordForPage(_hive, "ColorizationColorInactive");
        var hasFactual = inactiveValue is { } v && v != AutoColorSentinel && v != ThemeColorSentinel;
        if (hasFactual)
            return ComposePickerArgb(inactiveValue!.Value, ReadInactiveOpacityPercent(hasFactual));

        return VistaGlassPreviewCompositor.BuildInactiveTintArgb(_hive, opaqueBlend);
    }

    private bool IsInactiveGateOn() =>
        _inactiveVistaGateArmed || VistaGlassColorPolicy.HasInactiveOverrideKey(_hive);

    private uint ReadInactiveOpacityPercent(bool hasInactive)
    {
        if (DwmRegistryStore.ReadDwordForPage(_hive, "GlassOpacityInactive") is { } inactive)
            return Math.Min(inactive, 100u);

        return VistaGlassColorPolicy.ReadUiGlassOpacityPercent(_hive);
    }

    private void ReloadAfterglowSecondary()
    {
        var gateOn = Win7GlassColorPolicy.IsAfterglowGateOn(_hive);
        SecondaryColorExpander.IsGateOn = gateOn;

        uint colorDword;
        uint percent;
        if (gateOn)
        {
            colorDword = Win7GlassColorPolicy.ReadFactualAfterglowColorRgb(_hive);
            percent = Win7GlassColorPolicy.ReadUiAfterglowBalancePercent(_hive);
        }
        else
        {
            (colorDword, percent) = Win7GlassColorPolicy.ResolveOgAfterglowPreview(_hive);
        }

        var argb = Win7GlassColorPolicy.ToPickerArgbFromBgr(colorDword, percent);
        SecondaryColorPicker.SetColor(argb);
        SyncSecondaryPickerTracking(argb, percent);

        SecondaryColorPanel.SetDefaultColorActionState(
            gateOn,
            disableDefaultButton: gateOn && DwmColorRegistry.IsAfterglowSecondaryAtDefaults(_hive));
    }

    private bool HasAfterglowColorKeyForPage() =>
        DwmRegistryStore.ValueExistsForPage(_hive, DwmMirrorRegistry.AfterglowOverride)
        || DwmRegistryStore.ValueExistsForPage(_hive, DwmMirrorRegistry.AfterglowPrimary);

    private uint ReadWin7ActiveBalancePercent() =>
        Win7GlassColorPolicy.ReadFactualActiveBalancePercent(_hive);

    private uint ReadPreviewBlurBalancePercent()
    {
        if (ActiveColorExpander.IsGateOn && IsWin7Tab(GlassTypePivot.SelectedIndex) && _blurBalanceSlider is not null)
            return (uint)Math.Round(_blurBalanceSlider.Value);

        return Win7GlassColorPolicy.ReadRuntimeFactualBlurBalancePercent();
    }

    private void ReloadActiveBlurSlider()
    {
        if (_blurBalanceSlider is null)
            return;

        var value = Win7GlassColorPolicy.ReadUiBlurBalancePercent(_hive);

        _blurBalanceSlider.Value = value;
        UpdateActiveBlurSliderEnabled();
        _lastBlurBalanceLive = value;
        SettingsSliderCommit.SyncCommittedValue(_blurBalanceSlider, value);
        if (_blurBalanceValueText is not null)
            _blurBalanceValueText.Text = GlassSettingsResources.Format("Dwm_ReflectionPercentFormat", value);
    }

    private void UpdateActiveBlurSliderEnabled()
    {
        UpdateBlurRadiusSliderEnabled();
        if (_blurBalanceSlider is not null)
            _blurBalanceSlider.IsEnabled = ActiveColorExpander.IsGateOn && IsWin7Tab(GlassTypePivot.SelectedIndex);
    }

    private void UpdateBlurRadiusSliderEnabled()
    {
        if (_blurRadiusSlider is not null)
            _blurRadiusSlider.IsEnabled = ActiveColorExpander.IsGateOn && !IsD3DRenderingEnabled();
    }

    private void ReloadBaseColorRow()
    {
        if (_baseColorModeCombo is null)
            return;

        var opt = DwmRegistryStore.ReadDwordForPage(_hive, _baseColorRegKey);
        var isAuto = IsBaseColorAuto(opt);

        if (isAuto)
        {
            _baseColorModeCombo.SelectedIndex = 0;
            BaseColorPicker.IsEnabled = false;
            BaseColorPicker.SetColor(0xFF808080);
        }
        else if (opt == ThemeColorSentinel)
        {
            _baseColorModeCombo.SelectedIndex = 1;
            BaseColorPicker.IsEnabled = false;
        }
        else
        {
            _baseColorModeCombo.SelectedIndex = 2;
            BaseColorPicker.IsEnabled = true;
            BaseColorPicker.SetColor(opt!.Value);
        }

        ApplyBaseColorExpansionState();
        BaseColorPanel.SetDefaultColorActionState(!isAuto, disableDefaultButton: isAuto);
        _lastBaseColorArgb = BaseColorPicker.GetColorArgb();
    }

    private void ReloadBaseOpacityRows()
    {
        if (_baseOpacityActiveRow is null || _baseOpacityInactiveRow is null)
            return;

        ApplyBaseOpacityTabLabels();
        _baseOpacityActiveRow.Reload();
        _baseOpacityInactiveRow.Reload();
    }

    private void ReloadPriorityRow()
    {
        if (_priorityCombo is null)
            return;

        var prio = DwmRegistryStore.ReadDwordForPage(_hive, "ColorizationOpaqueBlendPriority") ?? DwmDefaults.PriorityAuto;
        _priorityCombo.SelectedIndex = DwmDefaults.OpaqueBlendPriorityToComboSel(prio);
    }

    private void SyncActivePickerTracking(uint argb, uint percent)
    {
        _lastActiveRgb = ExtractRgb(argb);
        _lastActivePercent = percent;
    }

    private void SyncSecondaryPickerTracking(uint argb, uint percent)
    {
        _lastSecondaryRgb = ExtractRgb(argb);
        _lastSecondaryPercent = percent;
    }

    // ─── Glass type ───────────────────────────────────────────────────────────

    private bool IsWin7GlassType() => (DwmRegistryStore.ReadDwordForPage(_hive, "GlassType") ?? 0) == 1;
    private bool IsWin7GlassTypeLocal() => (DwmRegistryStore.ReadDword(_hive, "GlassType") ?? 0) == 1;
    private static bool IsRuntimeWin7GlassType() =>
        (DwmRegistryStore.ReadEffectiveDword("GlassType") ?? 0) == 1;

    private bool ResolveRuntimeWin7ForPendingGlassType(int pivotIndex)
    {
        if (_hive == DwmRegistryHive.Hklm
            && DwmRegistryStore.ReadDword(DwmRegistryHive.Hkcu, "GlassType") is { } userValue)
        {
            return userValue == 1;
        }

        return IsWin7Tab(pivotIndex);
    }

    private bool IsWin7Tab(int tabIndex) => tabIndex == 1;

    private void ApplyActiveColorLabels(int tabIndex)
    {
        if (IsWin7Tab(tabIndex))
        {
            ActiveColorExpander.Title = GlassSettingsResources.Get("Dwm_GlassColorWin7");
            ActiveColorExpander.Description = GlassSettingsResources.Get("Dwm_GlassColorWin7Subtitle");
            ActiveColorPanel.Description = GlassSettingsResources.Get("Dwm_ActiveGlassColorBodyDescWin7");
        }
        else
        {
            ActiveColorExpander.Title = GlassSettingsResources.Get("Dwm_ActiveGlassColor");
            ActiveColorExpander.Description = GlassSettingsResources.Get("Dwm_ActiveGlassColorSubtitle");
            ActiveColorPanel.Description = GlassSettingsResources.Get("Dwm_ActiveGlassColorBodyDescVista");
        }
    }

    private void ApplySecondaryColorLabels(int tabIndex)
    {
        if (IsWin7Tab(tabIndex))
        {
            SecondaryColorExpander.Title = GlassSettingsResources.Get("Dwm_AfterglowColor");
            SecondaryColorExpander.Description = GlassSettingsResources.Get("Dwm_AfterglowSubtitle");
            SecondaryColorPanel.Description = GlassSettingsResources.Get("Dwm_AfterglowColorBodyDesc");
        }
        else
        {
            SecondaryColorExpander.Title = GlassSettingsResources.Get("Dwm_CustomInactiveColor");
            SecondaryColorExpander.Description = GlassSettingsResources.Get("Dwm_CustomInactiveColorSubtitle");
            SecondaryColorPanel.Description = GlassSettingsResources.Get("Dwm_CustomInactiveColorBodyDesc");
        }
    }

    private void ApplyActiveBlurSliderVisibility(int tabIndex)
    {
        // Footer всегда видим (радиус); баланс — только Win7.
        ActiveColorPanel.SetFooterContentVisible(true);
        var balanceVisible = IsWin7Tab(tabIndex) ? Visibility.Visible : Visibility.Collapsed;
        if (_blurBalanceLabel is not null)
            _blurBalanceLabel.Visibility = balanceVisible;
        if (_blurBalanceSlider is not null)
            _blurBalanceSlider.Visibility = balanceVisible;
        if (_blurBalanceValueText is not null)
            _blurBalanceValueText.Visibility = balanceVisible;
        UpdateActiveBlurSliderEnabled();
    }

    private void ApplyBaseColorTabLabels()
    {
        var spec = BaseColorTabs[BaseColorPivot.SelectedIndex];
        _baseColorRegKey = spec.RegistryKey;
        BaseColorExpander.Description = GlassSettingsResources.Get(spec.HeaderDescKey);
        BaseColorPanel.Description = GlassSettingsResources.Get(spec.BodyDescKey);
    }

    private void ApplyBaseOpacityTabLabels()
    {
        if (_baseOpacityActiveRow is null || _baseOpacityInactiveRow is null)
            return;

        var spec = BaseOpacityTabSpecs[BaseOpacityPivot.SelectedIndex];
        _baseOpacityActiveRow.Hive = _hive;
        _baseOpacityInactiveRow.Hive = _hive;
        _baseOpacityActiveRow.RegistryValueName = spec.ActiveRegistryKey;
        _baseOpacityInactiveRow.RegistryValueName = spec.InactiveRegistryKey;
        _baseOpacityActiveRow.UnsavedScope = SettingsCommitScope.ForDwmKeys(_hive, spec.ActiveRegistryKey);
        _baseOpacityInactiveRow.UnsavedScope = SettingsCommitScope.ForDwmKeys(_hive, spec.InactiveRegistryKey);
        _baseOpacityActiveRow.Title = GlassSettingsResources.Get("Dwm_BaseOpacitySpoilerTitleActive");
        _baseOpacityActiveRow.Description = GlassSettingsResources.Get(spec.ActiveDescKey);
        _baseOpacityInactiveRow.Title = GlassSettingsResources.Get("Dwm_BaseOpacitySpoilerTitleInactive");
        _baseOpacityInactiveRow.Description = GlassSettingsResources.Get(spec.InactiveDescKey);
    }

    private static bool IsBaseColorAuto(uint? opt) =>
        opt is null || opt == AutoColorSentinel;

    private void ApplyBaseColorExpansionState()
    {
        BaseColorExpander.IsExpansionEnabled = _baseColorModeCombo?.SelectedIndex == 2;
    }

    private void RefreshAdvancedExpandersChrome()
    {
        EnsureExpanderHeaderChrome(ActiveColorExpander);
        EnsureExpanderHeaderChrome(BaseColorExpander);
        EnsureExpanderHeaderChrome(SecondaryColorExpander);
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

    private static void CollapseIfExpanded(SettingsSystemGateExpander expander)
    {
        if (expander.IsExpanded)
            expander.IsExpanded = false;
    }

    // ─── Pivot handlers ─────────────────────────────────────────────────────

    private async void GlassTypePivot_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (SettingsInteractionDeferral.IsSliderGestureActive)
            return;

        if (_loading)
            return;

        var newIndex = GlassTypePivot.SelectedIndex;
        var prev = _lastGlassTypePivotIndex;
        if (prev == newIndex)
            return;

        await PageShell.Operations.RunSequenceAsync(async seq =>
        {
            CancelExternalPreviewRefresh();

            if (!IsLoaded)
                return;

            await seq.AnimatingAsync(async () =>
            {
                CollapseIfExpanded(ActiveColorExpander);
                CollapseIfExpanded(SecondaryColorExpander);

                var previewTask = PrepareCompositionPreviewPivotAsync(newIndex, animate: true);

                await Task.WhenAll(
                    PivotTabContentTransition.AnimateTabChangeAsync(
                        GlassTypeTabHost, prev, newIndex,
                        () =>
                        {
                            ApplyActiveColorLabels(newIndex);
                            ApplySecondaryColorLabels(newIndex);
                            ApplyActiveBlurSliderVisibility(newIndex);
                            ApplyExpanderUnsavedScopesForTab(newIndex);
                        }),
                    previewTask).ConfigureAwait(true);
            }).ConfigureAwait(true);

            var committed = false;
            await seq.RegistryMutationAsync(async () =>
            {
                _loading = true;
                try
                {
                    committed = CommitGlassTypeFromPivot(newIndex);
                    ApplyActiveColorLabels(newIndex);
                    ApplySecondaryColorLabels(newIndex);
                    ApplyActiveBlurSliderVisibility(newIndex);
                    ApplyExpanderUnsavedScopesForTab(newIndex);
                    ReloadActiveColor();
                    ReloadSecondaryColor();
                    ReloadActiveBlurSlider();
                }
                finally
                {
                    _loading = false;
                }

                await Task.CompletedTask.ConfigureAwait(false);
            }).ConfigureAwait(true);

            await seq.CosmeticAsync(() =>
            {
                _lastGlassTypePivotIndex = newIndex;
                RefreshPivotTabUnsavedIndicators();
                RefreshAdvancedExpandersChrome();
                RefreshParallelBranchWarnings();
                FlushDeferredExternalPreviewRefresh();
            }).ConfigureAwait(true);

            if (committed)
            {
                await seq.NoticeSyncAsync(() => PageShell.FlushSessionNotice()).ConfigureAwait(true);
            }
        }).ConfigureAwait(true);
    }

    private bool CommitGlassTypeFromPivot(int pivotIndex)
    {
        var win7Selected = pivotIndex == 1;
        var localGlassTypeExists = DwmRegistryStore.ValueExists(_hive, "GlassType");
        var localWin7 = localGlassTypeExists && IsWin7GlassTypeLocal();

        if (localGlassTypeExists && localWin7 == win7Selected)
            return false;

        SettingsSession.Current.WriteDwmDword(
            _hive,
            "GlassType",
            win7Selected ? 1u : 0u,
            liveDwm: true,
            dwmNotifyImmediate: true);
        return true;
    }

    private async void BaseColorPivot_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading) return;

        var newIndex = BaseColorPivot.SelectedIndex;
        var prev = _lastBaseColorPivotIndex;
        if (prev == newIndex) return;

        CollapseIfExpanded(BaseColorExpander);

        await PivotTabContentTransition.AnimateTabChangeAsync(
            BaseColorTabHost, prev, newIndex,
            () =>
            {
                ApplyBaseColorTabLabels();
                ReloadBaseColorRow();
            }).ConfigureAwait(true);

        _lastBaseColorPivotIndex = newIndex;
        ApplyBaseColorExpanderUnsavedScope(newIndex);
        RefreshPivotTabUnsavedIndicators();
        RefreshAdvancedExpandersChrome();
        RefreshParallelBranchWarnings();
    }

    private async void BaseOpacityPivot_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading) return;

        var newIndex = BaseOpacityPivot.SelectedIndex;
        var prev = _lastBaseOpacityPivotIndex;
        if (prev == newIndex) return;

        await PivotTabContentTransition.AnimateTabChangeAsync(
            BaseOpacityTabHost, prev, newIndex,
            () =>
            {
                ApplyBaseOpacityTabLabels();
                ReloadBaseOpacityRows();
            }).ConfigureAwait(true);

        _lastBaseOpacityPivotIndex = newIndex;
        RefreshPivotTabUnsavedIndicators();
        RefreshAdvancedExpandersChrome();
    }

    private void RefreshParallelBranchWarnings()
    {
        var activeKeys = IsWin7Tab(GlassTypePivot.SelectedIndex)
            ? DwmColorizationScopeCatalog.ActiveColorWin7Keys
            : DwmColorizationScopeCatalog.ActiveColorVistaKeys;
        ApplySectionParallelWarning(
            (visible, tooltip, marker) => ActiveColorExpander.SetParallelBranchWarning(visible, tooltip, marker),
            activeKeys);

        var secondaryKeys = IsWin7Tab(GlassTypePivot.SelectedIndex)
            ? DwmColorizationScopeCatalog.SecondaryColorWin7Keys
            : DwmColorizationScopeCatalog.SecondaryColorVistaKeys;
        ApplySectionParallelWarning(
            (visible, tooltip, marker) => SecondaryColorExpander.SetParallelBranchWarning(visible, tooltip, marker),
            secondaryKeys);

        var baseColorKey = BaseColorTabs[Math.Clamp(BaseColorPivot.SelectedIndex, 0, BaseColorTabs.Length - 1)].RegistryKey;
        ApplySectionParallelWarning(
            (visible, tooltip, marker) => BaseColorExpander.SetParallelBranchWarning(visible, tooltip, marker),
            [baseColorKey]);
    }

    // ─── Color handlers ───────────────────────────────────────────────────────

    private void ActiveColorExpander_OnGateToggled(object sender, RoutedEventArgs e)
    {
        if (_loading || PageShell.Operations.ShouldBlockPageMutation)
            return;
        var win7 = IsWin7Tab(GlassTypePivot.SelectedIndex);
        if (ActiveColorExpander.IsGateOn)
        {
            DwmColorRegistry.ApplyActiveColorPlatformDefaults(_hive, win7);
            SettingsSession.Current.MarkDirty();
            LiveReloadScheduler.NotifyDwmColorizationNow();
        }
        else
        {
            if (win7)
                Win7GlassColorPolicy.ClearActiveOverrides(_hive);
            else
                VistaGlassColorPolicy.ClearActiveOverrides(_hive);
            SettingsSession.Current.MarkDirty();
        }

        _loading = true;
        try
        {
            ReloadActiveColor();
            ReloadActiveBlurSlider();
            ReloadBlurRadiusSlider();
        }
        finally
        {
            _loading = false;
        }

        RefreshPivotTabUnsavedIndicators();
        RefreshParallelBranchWarnings();
        RefreshCompositionPreview();
    }

    private void ActiveColorPicker_OnColorLivePreview(object? sender, uint argb)
    {
        if (_loading || !ActiveColorExpander.IsGateOn)
            return;

        var rgb = ExtractRgb(argb);
        var percent = AlphaToPercent(ExtractAlphaByte(argb));
        var win7 = IsWin7Tab(GlassTypePivot.SelectedIndex);
        var writeRgb = rgb != _lastActiveRgb;
        var writePct = percent != _lastActivePercent;
        if (!writeRgb && !writePct)
            return;

        _ = Task.Run(() =>
        {
            if (writeRgb)
                WriteActiveColorRgbLivePreview(argb);
            if (writePct)
                WriteActiveOpacityPercentLivePreview(percent, win7);
        });
        RefreshCompositionPreview(CompositionPreviewRefreshScope.LiveSlider);
    }

    private void ActiveColorPicker_OnColorChanged(object? sender, uint argb)
    {
        if (_loading || !ActiveColorExpander.IsGateOn) return;

        var rgb = ExtractRgb(argb);
        var percent = AlphaToPercent(ExtractAlphaByte(argb));
        var tabIndex = GlassTypePivot.SelectedIndex;
        var win7 = IsWin7Tab(tabIndex);

        // Без изменений — не трогаем реестр и не помечаем dirty (баг: спойлеры давали ложное уведомление).
        if (rgb == _lastActiveRgb && percent == _lastActivePercent)
            return;

        if (rgb != _lastActiveRgb)
            WriteActiveColorRgbOnly(argb);

        if (percent != _lastActivePercent)
            WriteActiveOpacityPercent(percent, win7);

        SyncActivePickerTracking(argb, percent);
        RefreshCompositionPreview();
    }

    private void ActiveColorPanel_OnDefaultColorClick(object sender, RoutedEventArgs e)
    {
        if (_loading || !ActiveColorExpander.IsGateOn)
            return;

        var win7 = IsWin7Tab(GlassTypePivot.SelectedIndex);
        DwmColorRegistry.ApplyActiveColorDefaults(_hive, win7);

        _loading = true;
        try
        {
            ReloadActiveColor();
            ReloadActiveBlurSlider();
            ReloadBlurRadiusSlider();
        }
        finally
        {
            _loading = false;
        }

        SettingsSession.Current.MarkDirty();
        RefreshPivotTabUnsavedIndicators();
        RefreshCompositionPreview();
    }

    private void SecondaryColorExpander_OnGateToggled(object sender, RoutedEventArgs e)
    {
        if (_loading || PageShell.Operations.ShouldBlockPageMutation)
            return;
        if (IsWin7Tab(GlassTypePivot.SelectedIndex))
            OnAfterglowGateToggled();
        else
            OnInactiveGateToggled();

        _loading = true;
        try
        {
            ReloadSecondaryColor();
        }
        finally
        {
            _loading = false;
        }

        RefreshParallelBranchWarnings();
        if (IsWin7Tab(GlassTypePivot.SelectedIndex))
            RefreshCompositionPreview();
        else
            RefreshVistaInactiveSplitPreview();
    }

    private void OnInactiveGateToggled()
    {
        if (SecondaryColorExpander.IsGateOn)
        {
            _inactiveVistaGateArmed = true;
        }
        else
        {
            _inactiveVistaGateArmed = false;
            VistaGlassColorPolicy.ClearInactiveOverrides(_hive);
            SettingsSession.Current.MarkDirty();
        }
    }

    private void OnAfterglowGateToggled()
    {
        if (SecondaryColorExpander.IsGateOn)
        {
            DwmColorRegistry.ApplyAfterglowSecondaryDefaults(_hive);
            SettingsSession.Current.MarkDirty();
            LiveReloadScheduler.NotifyDwmColorizationNow();
        }
        else
        {
            Win7GlassColorPolicy.ClearAfterglowOverrides(_hive);
            SettingsSession.Current.MarkDirty();
        }
    }

    private void SecondaryColorPicker_OnColorLivePreview(object? sender, uint argb)
    {
        if (_loading || !SecondaryColorExpander.IsGateOn)
            return;

        var rgb = ExtractRgb(argb);
        var percent = AlphaToPercent(ExtractAlphaByte(argb));
        var win7 = IsWin7Tab(GlassTypePivot.SelectedIndex);
        var writeRgb = rgb != _lastSecondaryRgb;
        var writePct = percent != _lastSecondaryPercent;
        if (!writeRgb && !writePct)
            return;

        _ = Task.Run(() =>
        {
            if (win7)
            {
                if (writeRgb)
                    WriteAfterglowRgbLivePreview(argb);
                if (writePct)
                    WriteAfterglowBalancePercentLivePreview(percent);
            }
            else
            {
                if (writeRgb)
                    WriteInactiveColorRgbLivePreview(argb);
                if (writePct)
                    SettingsSession.Current.WriteDwmDwordLivePreview(_hive, "GlassOpacityInactive", percent);
            }
        });
        RefreshCompositionPreview(CompositionPreviewRefreshScope.LiveSlider);
    }

    private void SecondaryColorPicker_OnColorChanged(object? sender, uint argb)
    {
        if (_loading || !SecondaryColorExpander.IsGateOn) return;

        var rgb = ExtractRgb(argb);
        var percent = AlphaToPercent(ExtractAlphaByte(argb));
        var win7 = IsWin7Tab(GlassTypePivot.SelectedIndex);

        if (rgb == _lastSecondaryRgb && percent == _lastSecondaryPercent)
            return;

        if (win7)
        {
            if (rgb != _lastSecondaryRgb)
                WriteAfterglowRgbOnly(argb);
            if (percent != _lastSecondaryPercent)
                WriteAfterglowBalancePercent(percent);
        }
        else
        {
            _inactiveVistaGateArmed = false;
            if (rgb != _lastSecondaryRgb)
                WriteInactiveColorRgbOnly(argb);
            if (percent != _lastSecondaryPercent)
                SettingsSession.Current.WriteDwmDword(_hive, "GlassOpacityInactive", percent, liveDwm: true, dwmNotifyImmediate: true);
        }

        SyncSecondaryPickerTracking(argb, percent);
        RefreshCompositionPreview();
    }

    private void SecondaryColorPanel_OnDefaultColorClick(object sender, RoutedEventArgs e)
    {
        if (_loading || !SecondaryColorExpander.IsGateOn)
            return;

        if (IsWin7Tab(GlassTypePivot.SelectedIndex))
        {
            DwmColorRegistry.ApplyAfterglowSecondaryDefaults(_hive);
        }
        else
        {
            _inactiveVistaGateArmed = false;
            DwmColorRegistry.ApplyInactiveSecondaryDefaults(_hive);
        }

        _loading = true;
        try
        {
            ReloadSecondaryColor();
        }
        finally
        {
            _loading = false;
        }

        SettingsSession.Current.MarkDirty();
        RefreshPivotTabUnsavedIndicators();
        RefreshCompositionPreview();
    }

    private async void OnOpaqueBlendToggleChanged(bool enabled)
    {
        if (_loading) return;
        await WriteCompositionBoolToggleAsync(OpaqueBlendKey, enabled).ConfigureAwait(true);
    }

    /// <summary>Master-тумблер карточки удалён; тумблеры управляют параметрами независимо.</summary>
    private void OnCompositionMasterToggled(bool on)
    {
    }

    // ─── Registry writes (RGB / balance split) ──────────────────────────────────

    private void WriteActiveColorRgbOnly(uint argbFromPicker)
    {
        DwmMirrorRegistry.WriteColorPair(
            _hive,
            DwmMirrorRegistry.GlassColorPrimary,
            DwmMirrorRegistry.GlassColorOverride,
            argbFromPicker,
            liveDwm: true);
    }

    private void WriteActiveColorRgbLivePreview(uint argbFromPicker)
    {
        DwmMirrorRegistry.WriteColorPairLivePreview(
            _hive,
            DwmMirrorRegistry.GlassColorPrimary,
            DwmMirrorRegistry.GlassColorOverride,
            argbFromPicker);
    }

    private void WriteInactiveColorRgbOnly(uint argbFromPicker)
    {
        var dword = DwmMirrorRegistry.ToDwordPreserveAlpha(_hive, "ColorizationColorInactive", argbFromPicker);
        SettingsSession.Current.WriteDwmDword(_hive, "ColorizationColorInactive", dword, liveDwm: true, dwmNotifyImmediate: true);
    }

    private void WriteInactiveColorRgbLivePreview(uint argbFromPicker)
    {
        var dword = DwmMirrorRegistry.ToDwordPreserveAlpha(_hive, "ColorizationColorInactive", argbFromPicker);
        SettingsSession.Current.WriteDwmDwordLivePreview(_hive, "ColorizationColorInactive", dword);
    }

    private void WriteAfterglowRgbOnly(uint argbFromPicker)
    {
        DwmMirrorRegistry.WriteColorPair(
            _hive,
            DwmMirrorRegistry.AfterglowPrimary,
            DwmMirrorRegistry.AfterglowOverride,
            argbFromPicker,
            liveDwm: true);
    }

    private void WriteAfterglowRgbLivePreview(uint argbFromPicker)
    {
        DwmMirrorRegistry.WriteColorPairLivePreview(
            _hive,
            DwmMirrorRegistry.AfterglowPrimary,
            DwmMirrorRegistry.AfterglowOverride,
            argbFromPicker);
    }

    private void WriteActiveOpacityPercent(uint percent, bool win7)
    {
        percent = Math.Min(percent, 100u);
        if (win7)
        {
            DwmMirrorRegistry.WriteBalancePair(
                _hive,
                DwmMirrorRegistry.ColorBalancePrimary,
                DwmMirrorRegistry.ColorBalanceOverride,
                percent,
                liveDwm: true);
        }
        else
            SettingsSession.Current.WriteDwmDword(_hive, "GlassOpacity", percent, liveDwm: true, dwmNotifyImmediate: true);
    }

    private void WriteActiveOpacityPercentLivePreview(uint percent, bool win7)
    {
        percent = Math.Min(percent, 100u);
        if (win7)
        {
            DwmMirrorRegistry.WriteBalancePairLivePreview(
                _hive,
                DwmMirrorRegistry.ColorBalancePrimary,
                DwmMirrorRegistry.ColorBalanceOverride,
                percent);
        }
        else
            SettingsSession.Current.WriteDwmDwordLivePreview(_hive, "GlassOpacity", percent);
    }

    private void WriteAfterglowBalancePercent(uint percent)
    {
        percent = Math.Min(percent, 100u);
        DwmMirrorRegistry.WriteBalancePair(
            _hive,
            DwmMirrorRegistry.AfterglowBalancePrimary,
            DwmMirrorRegistry.AfterglowBalanceOverride,
            percent,
            liveDwm: true);
    }

    private void WriteAfterglowBalancePercentLivePreview(uint percent)
    {
        percent = Math.Min(percent, 100u);
        DwmMirrorRegistry.WriteBalancePairLivePreview(
            _hive,
            DwmMirrorRegistry.AfterglowBalancePrimary,
            DwmMirrorRegistry.AfterglowBalanceOverride,
            percent);
    }

    private void WriteBlurBalanceLivePreview(uint value)
    {
        value = Math.Min(value, 100u);
        DwmMirrorRegistry.WriteBalancePairLivePreview(
            _hive,
            DwmMirrorRegistry.BlurBalancePrimary,
            DwmMirrorRegistry.BlurBalanceOverride,
            value);
    }

    private void TryWriteBlurBalanceLivePreview(uint value)
    {
        value = Math.Min(value, 100u);
        if (value == _lastBlurBalanceLive)
            return;

        _lastBlurBalanceLive = value;
        WriteBlurBalanceLivePreview(value);
    }

    private void DeleteActiveOpacityOnly(int tabIndex)
    {
        if (IsWin7TabStatic(tabIndex))
        {
            DwmMirrorRegistry.DeletePair(
                _hive,
                DwmMirrorRegistry.ColorBalancePrimary,
                DwmMirrorRegistry.ColorBalanceOverride,
                liveDwm: true);
        }
        else
            SettingsSession.Current.DeleteDwmValue(_hive, "GlassOpacity", liveDwm: true);
    }

    private void DeleteActiveColorAndOpacity(int tabIndex)
    {
        DwmColorRegistry.ResetGlassColor(_hive);
        DeleteActiveOpacityOnly(tabIndex);
    }

    private static bool IsWin7TabStatic(int tabIndex) => tabIndex == 1;

    // ─── Base color ───────────────────────────────────────────────────────────

    private void OnBaseColorModeChanged()
    {
        if (_loading || _baseColorModeCombo is null)
            return;

        switch (_baseColorModeCombo.SelectedIndex)
        {
            case 0:
                SettingsSession.Current.DeleteDwmValue(_hive, _baseColorRegKey, liveDwm: true);
                BaseColorPicker.IsEnabled = false;
                break;
            case 1:
                SettingsSession.Current.WriteDwmDword(_hive, _baseColorRegKey, ThemeColorSentinel, liveDwm: true, dwmNotifyImmediate: true);
                BaseColorPicker.IsEnabled = false;
                break;
            default:
                BaseColorPicker.IsEnabled = true;
                CommitBaseColorCustom(BaseColorPicker.GetColorArgb());
                break;
        }

        ApplyBaseColorExpansionState();
        if (_baseColorModeCombo.SelectedIndex == 2)
            BaseColorExpander.IsExpanded = true;
        BaseColorPanel.SetDefaultColorActionState(_baseColorModeCombo.SelectedIndex != 0, disableDefaultButton: false);
    }

    private void BaseColorPicker_OnColorLivePreview(object? sender, uint argb)
    {
        if (_loading || _baseColorModeCombo?.SelectedIndex != 2)
            return;

        _ = Task.Run(() => CommitBaseColorCustomLivePreview(argb));
    }

    private void BaseColorPicker_OnColorChanged(object? sender, uint argb)
    {
        if (_loading || _baseColorModeCombo?.SelectedIndex != 2)
            return;

        if (argb == _lastBaseColorArgb)
            return;

        CommitBaseColorCustom(argb);
    }

    private void CommitBaseColorCustomLivePreview(uint argb)
    {
        var a = (argb >> 24) & 0xFF;
        var r = (argb >> 16) & 0xFF;
        var g = (argb >> 8) & 0xFF;
        var b = argb & 0xFF;
        var dword = (uint)((a << 24) | (r << 16) | (g << 8) | b);
        SettingsSession.Current.WriteDwmDwordLivePreview(_hive, _baseColorRegKey, dword);
    }

    private void CommitBaseColorCustom(uint argb)
    {
        var a = (argb >> 24) & 0xFF;
        var r = (argb >> 16) & 0xFF;
        var g = (argb >> 8) & 0xFF;
        var b = argb & 0xFF;
        var dword = (uint)((a << 24) | (r << 16) | (g << 8) | b);
        SettingsSession.Current.WriteDwmDword(_hive, _baseColorRegKey, dword, liveDwm: true, dwmNotifyImmediate: true);
        _lastBaseColorArgb = argb;
    }

    private void BaseColorPanel_OnDefaultColorClick(object sender, RoutedEventArgs e)
    {
        if (_loading)
            return;

        SettingsSession.Current.DeleteDwmValue(_hive, _baseColorRegKey, liveDwm: true);

        _loading = true;
        try
        {
            ReloadBaseColorRow();
        }
        finally
        {
            _loading = false;
        }

        SettingsSession.Current.MarkDirty();
        RefreshPivotTabUnsavedIndicators();
    }

    // ─── Priority ─────────────────────────────────────────────────────────────

    private void OnPriorityComboChanged()
    {
        if (_loading || _priorityCombo is null)
            return;

        var sel = _priorityCombo.SelectedIndex;
        if (sel == 2)
            SettingsSession.Current.DeleteDwmValue(_hive, "ColorizationOpaqueBlendPriority", liveDwm: true);
        else
            SettingsSession.Current.WriteDwmDword(
                _hive,
                "ColorizationOpaqueBlendPriority",
                DwmDefaults.OpaqueBlendPriorityFromComboSel(sel),
                liveDwm: true,
                dwmNotifyImmediate: true);
    }

    // ─── Advanced expander ────────────────────────────────────────────────────

    private void AdvancedExpander_OnExpanded(object sender, RoutedEventArgs e)
    {
        if (!AdvancedExpander.IsExpanded)
            return;

        RefreshAdvancedExpandersChrome();
        _baseOpacityActiveRow?.RefreshHeaderLayout();
        _baseOpacityInactiveRow?.RefreshHeaderLayout();
    }

    private void ApplySectionParallelWarning(Action<bool, string?, string?> setWarning, IEnumerable<string> keys) =>
        DwmParallelBranchUi.ApplySectionWarning(_hive, keys, setWarning);

    private async Task RefreshHkcuConflictNoticeAsync()
    {
        var stack = PageShell.PageNoticeStack;
        var strip = stack.ActionNotice;
        var showConflict = _hive == DwmRegistryHive.Hkcu
                           && DwmRegistryStore.HasAnyPresent(
                               DwmRegistryHive.Hklm,
                               DwmSnapshotCatalog.GetKeys(DwmSnapshotPartition.Colorization));
        if (showConflict)
        {
            strip.AllowDismiss = false;
            var host = SettingsNoticeCoordinator.CreateShellHost(stack, PageShell.PageScrollViewer);
            await SettingsNoticeCoordinator.PresentAsync(host, new SettingsNoticeCoordinator.NoticePresentRequest
            {
                Strip = strip,
                Severity = SettingsNoticeSeverity.Error,
                Message = GlassSettingsResources.Get("Dwm_HkcuConflictNotice"),
                MirrorSuppressKey = "dwm.hkcu-conflict.colorization",
                ShowSave = false,
                ShowAction = false,
                Animate = !strip.IsVisuallyOpen
            }).ConfigureAwait(true);
            return;
        }

        strip.AllowDismiss = true;
        await stack.CloseActionNoticeAsync().ConfigureAwait(true);
    }

    public SettingsCommitScope GetCommitScope() =>
        SettingsCommitScope.ForDwmHiveArea(CustomizationAreaId.DwmColorization, _hive);
}
