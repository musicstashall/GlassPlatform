using GlassSettings.Controls;
using GlassSettings.Core.Customization;
using GlassSettings.Core.Dwm;
using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Snapshot;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;

namespace GlassSettings.Pages.Dwm;

public sealed partial class DwmGlassDetailPage : Page, ISettingsCommitScopeProvider, IExternalRegistrySyncReloadable
{
    private const int RoundRectProfileWin8 = 0;
    private const int RoundRectProfileWin7 = 1;
    private const int RoundRectProfileCustom = 2;
    private const int TextGlowModeComposited = 3;

    private static readonly string[] GlassKeys = DwmSnapshotCatalog.GetKeys(DwmSnapshotPartition.Glass).ToArray();

    private readonly ComboBox _blurOptimizationCombo = new() { MinWidth = 160 };
    private readonly CheckBox _useD3DCheck = new()
    {
        VerticalAlignment = VerticalAlignment.Center,
        MinWidth = 24
    };
    private readonly HubInlineLabel _blurOptimizationLabel = new();

    private readonly ComboBox _roundRectProfileCombo = new() { MinWidth = 150 };
    private readonly NumberBox _roundRectRadiusBox = CreateNumberBox(0, 50);
    private readonly ComboBox _captionButtonsCombo = new() { MinWidth = 150 };
    private readonly TextBlock _disableModernBordersLabel = new();
    private readonly ToggleSwitch _disableModernBordersToggle = new()
    {
        OnContent = "",
        OffContent = ""
    };

    private readonly ComboBox _centerCaptionCombo = new() { MinWidth = 150 };
    private readonly ComboBox _textGlowModeCombo = new() { MinWidth = 150 };
    private readonly NumberBox _textGlowSizeBox = CreateNumberBox(0, 100);

    private readonly TextBlock _roundRectProfileLabel = new();
    private readonly TextBlock _roundRectRadiusLabel = new();
    private readonly TextBlock _captionButtonsLabel = new();
    private readonly TextBlock _textCenteringLabel = new();
    private readonly TextBlock _textGlowModeLabel = new();
    private readonly TextBlock _textGlowSizeLabel = new();

    private readonly SettingsPivotTabHeader[] _captionPivotHeaders = new SettingsPivotTabHeader[2];
    private readonly CaptionColorSection _captionActiveNormal;
    private readonly CaptionColorSection _captionInactiveNormal;
    private readonly CaptionColorSection _captionActiveMaximized;
    private readonly CaptionColorSection _captionInactiveMaximized;

    private DwmRegistryHive _hive = DwmRegistryHive.Hkcu;
    private bool _loading;
    private bool _generalGateArmed;
    private bool _windowGateArmed;
    private bool _captionGateArmed;
    private int _lastCaptionPivotIndex;

    private sealed class CaptionColorSection
    {
        public required string RegistryKey { get; init; }
        public required SettingsSystemGateExpander Expander { get; init; }
        public required ComboBox ModeCombo { get; init; }
        public required GlassAlphaColorPicker Picker { get; init; }
        public required SettingsColorPickerPanel Panel { get; init; }
        public required Func<uint> ResolveEffective { get; init; }
        public required bool IsActive { get; init; }
        public bool PropagateInheritance { get; init; }
        public bool GateArmed { get; set; }
        public uint LastCustomBgr { get; set; } = 0x000000u;
    }

    public DwmGlassDetailPage()
    {
        _captionActiveNormal = CreateCaptionColorSection(
            "ColorizationColorCaption",
            isActive: true,
            DwmGlassScopeCatalog.CaptionColorActiveKeys,
            () => DwmCaptionColorHelper.ResolveCaptionActive(_hive),
            propagateInheritance: true);
        _captionInactiveNormal = CreateCaptionColorSection(
            "ColorizationColorCaptionInactive",
            isActive: false,
            DwmGlassScopeCatalog.CaptionColorInactiveKeys,
            () => DwmCaptionColorHelper.ResolveCaptionInactive(_hive));
        _captionActiveMaximized = CreateCaptionColorSection(
            "ColorizationColorCaptionMaximized",
            isActive: true,
            DwmGlassScopeCatalog.CaptionColorActiveMaximizedKeys,
            () => DwmCaptionColorHelper.ResolveCaptionMaximized(_hive));
        _captionInactiveMaximized = CreateCaptionColorSection(
            "ColorizationColorCaptionInactiveMaximized",
            isActive: false,
            DwmGlassScopeCatalog.CaptionColorInactiveMaximizedKeys,
            () => DwmCaptionColorHelper.ResolveCaptionInactiveMaximized(_hive));

        InitializeComponent();
        ConfigureGeneralRow();
        ConfigureWindowExpander();
        ConfigureCaptionExpander();
        ConfigureCaptionPivot();
        ApplyLocalizedStrings();
        ApplyUnsavedScopes();

        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        _hive = DwmNavigationContext.FromNavigationParameter(e.Parameter).Hive;
        HiveScopeBanner.Hive = _hive;
        ApplyUnsavedScopes();
        Reload();
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        SettingsSession.Current.DirtyChanged += Session_OnDirtyChanged;
        Reload();
        _lastCaptionPivotIndex = CaptionColorPivot.SelectedIndex;
    }

    private void OnUnloaded(object sender, RoutedEventArgs e) =>
        SettingsSession.Current.DirtyChanged -= Session_OnDirtyChanged;

    private void Session_OnDirtyChanged(object? sender, EventArgs e)
    {
        ApplyUnsavedScopes();
        SettingsUnsavedIndicatorRefresh.Refresh(RootPanel);
        RefreshCaptionPivotTabIndicators();
        _ = RefreshHkcuConflictNoticeAsync();
    }

    private void ApplyLocalizedStrings()
    {
        GeneralRow.Description = GlassSettingsResources.Get("Dwm_AppearanceGeneralSubtitle");
        WindowExpander.Description = GlassSettingsResources.Get("Dwm_AppearanceWindowSubtitle");
        CaptionExpander.Description = GlassSettingsResources.Get("Dwm_AppearanceCaptionSubtitle");
        WindowParameterPanel.Description = GlassSettingsResources.Get("Dwm_AppearanceWindowBodyDesc");
        CaptionParameterPanel.Description = GlassSettingsResources.Get("Dwm_AppearanceCaptionBodyDesc");
        _roundRectProfileLabel.Text = GlassSettingsResources.Get("Dwm_RoundRectProfileLabel");
        _roundRectRadiusLabel.Text = GlassSettingsResources.Get("Dwm_GlassGeometryRadius");
        _captionButtonsLabel.Text = GlassSettingsResources.Get("Dwm_CaptionButtonsStyle");
        _textCenteringLabel.Text = GlassSettingsResources.Get("Dwm_TextCentering");
        _textGlowModeLabel.Text = GlassSettingsResources.Get("Dwm_TextGlowMode");
        _textGlowSizeLabel.Text = GlassSettingsResources.Get("Dwm_TextGlowSize");
        _disableModernBordersLabel.Text = GlassSettingsResources.Get("Dwm_DisableModernBorders");

        _blurOptimizationLabel.Label = GlassSettingsResources.Get("Dwm_BlurOptimization");
        _useD3DCheck.Content = GlassSettingsResources.Get("Dwm_UseDirect3DRenderer");
        ToolTipService.SetToolTip(_useD3DCheck, GlassSettingsResources.Get("Dwm_UseDirect3DRendererTooltip"));

        ApplyCaptionColorLocalizedStrings();
    }

    private void ApplyCaptionColorLocalizedStrings()
    {
        foreach (var section in new[]
                 {
                     _captionActiveNormal, _captionInactiveNormal,
                     _captionActiveMaximized, _captionInactiveMaximized
                 })
        {
            section.Expander.Title = GlassSettingsResources.Get(
                section.IsActive ? "Dwm_CaptionColorActiveTitle" : "Dwm_CaptionColorInactiveTitle");
            section.Expander.Description = GlassSettingsResources.Get(
                section.IsActive ? "Dwm_CaptionColorActiveSubtitle" : "Dwm_CaptionColorInactiveSubtitle");
            section.Panel.Description = GlassSettingsResources.Get("Dwm_CaptionColorPickerActionDesc");
            section.Expander.RefreshHeaderActionsLayout(resetStackState: true);
        }

        RefreshCaptionExpandersChrome();
    }

    private void ConfigureGeneralRow()
    {
        GeneralRow.IconGlyph = SettingsSectionGlyphs.General;
        GeneralRow.Title = GlassSettingsResources.Get("Dwm_AppearanceGeneral");

        DwmAppearanceComboNames.PopulateBlurOptimization(_blurOptimizationCombo);
        SettingsInteractionDeferral.WhenComboCommit(
            _blurOptimizationCombo,
            () => !_loading && GeneralRow.IsGateOn && IsBlurOptimizationEnabled(),
            OnBlurOptimizationChanged);

        _useD3DCheck.Checked += UseD3DCheck_OnChanged;
        _useD3DCheck.Unchecked += UseD3DCheck_OnChanged;

        _blurOptimizationLabel.Control = _blurOptimizationCombo;

        GeneralRow.HeaderActions = _useD3DCheck;
        GeneralRow.HeaderSlider = _blurOptimizationLabel;
        GeneralRow.RefreshHeaderLayout();
    }

    private void ConfigureWindowExpander()
    {
        WindowExpander.IconGlyph = SettingsSectionGlyphs.Glass;
        WindowExpander.Title = GlassSettingsResources.Get("Dwm_AppearanceWindow");
        WindowExpander.WireExpandedLayoutRefresh(() =>
        {
            EnsureExpanderHeaderChrome(WindowExpander);
            WindowParameterPanel.RefreshLayout();
        });

        DwmAppearanceComboNames.PopulateRoundRectProfile(_roundRectProfileCombo);
        DwmAppearanceComboNames.PopulateCaptionButtons(_captionButtonsCombo);

        SettingsInteractionDeferral.WhenComboCommit(
            _roundRectProfileCombo,
            () => !_loading && WindowExpander.IsGateOn,
            OnRoundRectProfileChanged);
        SettingsInteractionDeferral.WhenComboCommit(
            _captionButtonsCombo,
            () => !_loading && WindowExpander.IsGateOn,
            OnCaptionButtonsChanged);

        SettingsInteractionDeferral.WhenNumberBoxCommitOnEnter(_roundRectRadiusBox, WindowParameterPanel);
        SettingsNumberBoxCommit.Attach(
            _roundRectRadiusBox,
            () => !_loading
                && WindowExpander.IsGateOn
                && _roundRectProfileCombo.SelectedIndex == RoundRectProfileCustom,
            RoundRectRadius_OnCommit);

        _disableModernBordersToggle.Style =
            Application.Current.Resources["GlassSettingsCompactToggleSwitchStyle"] as Style;
        _disableModernBordersToggle.Toggled += DisableModernBordersToggle_OnToggled;

        var labelStyle = Application.Current.Resources["GlassSettingsSpoilerSecondaryTextStyle"] as Style;
        _roundRectProfileLabel.Style = labelStyle;
        _roundRectRadiusLabel.Style = labelStyle;
        _captionButtonsLabel.Style = labelStyle;
        _disableModernBordersLabel.Style = labelStyle;

        var windowTable = SettingsParameterBodyLayout.CreateBodyTable();
        SettingsParameterBodyLayout.AddDualRow(
            windowTable,
            _roundRectProfileLabel,
            _roundRectProfileCombo,
            _roundRectRadiusLabel,
            _roundRectRadiusBox);
        SettingsParameterBodyLayout.AddSingleRow(windowTable, _captionButtonsLabel, _captionButtonsCombo, topMargin: true);
        if (WindowsOsHelper.IsWindows11OrGreater())
        {
            SettingsParameterBodyLayout.AddSingleRow(
                windowTable,
                _disableModernBordersLabel,
                _disableModernBordersToggle,
                topMargin: true);
        }

        WindowParameterPanel.Description = GlassSettingsResources.Get("Dwm_AppearanceWindowBodyDesc");
        WindowParameterPanel.BodyContent = windowTable;
        WindowParameterPanel.DefaultClick += (_, _) => OnWindowDefault();
    }

    private void ConfigureCaptionExpander()
    {
        CaptionExpander.IconGlyph = SettingsSectionGlyphs.Caption;
        CaptionExpander.Title = GlassSettingsResources.Get("Dwm_AppearanceCaption");
        CaptionExpander.WireExpandedLayoutRefresh(() =>
        {
            EnsureExpanderHeaderChrome(CaptionExpander);
            CaptionParameterPanel.RefreshLayout();
        });

        DwmAppearanceComboNames.PopulateCenterCaption(_centerCaptionCombo);
        DwmAppearanceComboNames.PopulateTextGlowMode(_textGlowModeCombo);

        SettingsInteractionDeferral.WhenComboCommit(
            _centerCaptionCombo,
            () => !_loading && CaptionExpander.IsGateOn,
            OnCenterCaptionChanged);
        SettingsInteractionDeferral.WhenComboCommit(
            _textGlowModeCombo,
            () => !_loading && CaptionExpander.IsGateOn,
            OnTextGlowModeChanged);

        SettingsInteractionDeferral.WhenNumberBoxCommitOnEnter(_textGlowSizeBox, CaptionParameterPanel);
        SettingsNumberBoxCommit.Attach(
            _textGlowSizeBox,
            () => !_loading
                && CaptionExpander.IsGateOn
                && _textGlowModeCombo.SelectedIndex == TextGlowModeComposited,
            TextGlowSize_OnCommit);

        var labelStyle = Application.Current.Resources["GlassSettingsSpoilerSecondaryTextStyle"] as Style;
        _textCenteringLabel.Style = labelStyle;
        _textGlowModeLabel.Style = labelStyle;
        _textGlowSizeLabel.Style = labelStyle;

        var captionTable = SettingsParameterBodyLayout.CreateBodyTable();
        SettingsParameterBodyLayout.AddSingleRow(captionTable, _textCenteringLabel, _centerCaptionCombo);
        SettingsParameterBodyLayout.AddDualRow(
            captionTable,
            _textGlowModeLabel,
            _textGlowModeCombo,
            _textGlowSizeLabel,
            _textGlowSizeBox,
            topMargin: true);

        CaptionParameterPanel.Description = GlassSettingsResources.Get("Dwm_AppearanceCaptionBodyDesc");
        CaptionParameterPanel.BodyContent = captionTable;
        CaptionParameterPanel.DefaultClick += (_, _) => OnCaptionDefault();
    }

    private void ConfigureCaptionPivot()
    {
        _captionPivotHeaders[0] = SettingsPivotTabHeader.Create(GlassSettingsResources.Get("Dwm_PivotNormalWindow"));
        _captionPivotHeaders[1] = SettingsPivotTabHeader.Create(GlassSettingsResources.Get("Dwm_PivotMaximizedWindow"));
        CaptionColorPivot.Items[0] = new PivotItem { Header = _captionPivotHeaders[0].Root };
        CaptionColorPivot.Items[1] = new PivotItem { Header = _captionPivotHeaders[1].Root };

        CaptionNormalPanel.Children.Add(_captionActiveNormal.Expander);
        CaptionNormalPanel.Children.Add(_captionInactiveNormal.Expander);
        CaptionMaximizedPanel.Children.Add(_captionActiveMaximized.Expander);
        CaptionMaximizedPanel.Children.Add(_captionInactiveMaximized.Expander);
    }

    private CaptionColorSection CreateCaptionColorSection(
        string registryKey,
        bool isActive,
        string[] scopeKeys,
        Func<uint> resolveEffective,
        bool propagateInheritance = false)
    {
        var combo = new ComboBox { MinWidth = 120 };
        DwmAppearanceComboNames.PopulateCaptionColorMode(combo);

        var picker = new GlassAlphaColorPicker { IsAlphaVisible = false };
        var panel = new SettingsColorPickerPanel
        {
            Description = GlassSettingsResources.Get("Dwm_CaptionColorPickerActionDesc"),
            PickerContent = picker
        };

        var expander = new SettingsSystemGateExpander
        {
            HorizontalAlignment = HorizontalAlignment.Stretch,
            Title = GlassSettingsResources.Get(isActive ? "Dwm_CaptionColorActiveTitle" : "Dwm_CaptionColorInactiveTitle"),
            Description = GlassSettingsResources.Get(isActive ? "Dwm_CaptionColorActiveSubtitle" : "Dwm_CaptionColorInactiveSubtitle"),
            IconGlyph = SettingsSectionGlyphs.CaptionColor,
            HeaderActions = new HubInlineLabel
            {
                Label = GlassSettingsResources.Get("Dwm_LabelMode"),
                Control = combo
            },
            BodyContent = panel
        };

        var section = new CaptionColorSection
        {
            RegistryKey = registryKey,
            Expander = expander,
            ModeCombo = combo,
            Picker = picker,
            Panel = panel,
            ResolveEffective = resolveEffective,
            IsActive = isActive,
            PropagateInheritance = propagateInheritance
        };

        expander.UnsavedScope = DwmGlassScopeCatalog.ForKeys(scopeKeys, _hive);
        expander.GateToggled += CaptionColorGate_OnToggled;
        expander.Loaded += (_, _) => expander.RefreshHeaderActionsLayout(resetStackState: true);
        expander.WireExpandedLayoutRefresh(() =>
        {
            EnsureExpanderHeaderChrome(expander);
            SettingsUnsavedIndicatorRefresh.Refresh(expander);
        });
        SettingsInteractionDeferral.WhenComboCommit(
            combo,
            () => !_loading && IsCaptionColorGateOn(section),
            () => OnCaptionColorModeChanged(section));
        picker.ColorChanged += (_, argb) =>
        {
            if (_loading || !IsCaptionColorGateOn(section) || combo.SelectedIndex != DwmCaptionColorHelper.ModeCustom)
                return;

            var bgr = DwmCaptionColorHelper.PickerArgbToDwordBgr(argb);
            CommitCaptionColor(section, DwmCaptionColorHelper.ModeCustom, bgr);
        };
        panel.DefaultColorClick += (_, _) => OnCaptionColorDefault(section);

        return section;
    }

    private IEnumerable<CaptionColorSection> EnumerateCaptionColorSections()
    {
        yield return _captionActiveNormal;
        yield return _captionInactiveNormal;
        yield return _captionActiveMaximized;
        yield return _captionInactiveMaximized;
    }

    private CaptionColorSection? FindCaptionColorSection(SettingsSystemGateExpander expander)
    {
        foreach (var section in EnumerateCaptionColorSections())
        {
            if (ReferenceEquals(section.Expander, expander))
                return section;
        }

        return null;
    }

    private static NumberBox CreateNumberBox(double min, double max) =>
        new()
        {
            Style = (Style)Application.Current.Resources["GlassSettingsNumberBoxStyle"],
            SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Inline,
            Minimum = min,
            Maximum = max,
            SmallChange = 1
        };

    private static void EnsureExpanderHeaderChrome(SettingsSystemGateExpander expander)
    {
        void Apply()
        {
            expander.RefreshThemeChrome();
            expander.RefreshHeaderActionsLayout();
        }

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

    private void RefreshCaptionExpandersChrome()
    {
        EnsureExpanderHeaderChrome(_captionActiveNormal.Expander);
        EnsureExpanderHeaderChrome(_captionInactiveNormal.Expander);
        EnsureExpanderHeaderChrome(_captionActiveMaximized.Expander);
        EnsureExpanderHeaderChrome(_captionInactiveMaximized.Expander);
    }

    private void ApplyCaptionColorExpansionState(CaptionColorSection? section = null)
    {
        void Apply(CaptionColorSection s)
        {
            var gateOn = IsCaptionColorGateOn(s);
            var mode = s.ModeCombo.SelectedIndex;
            s.Expander.IsExpansionEnabled = gateOn && mode == DwmCaptionColorHelper.ModeCustom;
        }

        if (section is not null)
        {
            Apply(section);
            return;
        }

        Apply(_captionActiveNormal);
        Apply(_captionInactiveNormal);
        Apply(_captionActiveMaximized);
        Apply(_captionInactiveMaximized);
    }

    private void UpdateParameterPanelDefaultStates()
    {
        if (_loading)
            return;

        WindowParameterPanel.SetDefaultActionState(
            WindowExpander.IsGateOn,
            !DwmRegistryStore.HasAnyPresentForPage(_hive, DwmGlassScopeCatalog.WindowKeys));
        CaptionParameterPanel.SetDefaultActionState(
            CaptionExpander.IsGateOn,
            !DwmRegistryStore.HasAnyPresentForPage(_hive, DwmGlassScopeCatalog.CaptionStyleKeys));
    }

    private void OnWindowDefault()
    {
        if (_loading || !WindowExpander.IsGateOn)
            return;

        foreach (var key in DwmGlassScopeCatalog.WindowKeys)
            SettingsSession.Current.DeleteDwmValue(_hive, key, liveDwm: true);

        _loading = true;
        try
        {
            ReloadWindowSection();
        }
        finally
        {
            _loading = false;
        }

        MarkDirtyAndInvalidate();
    }

    private void OnCaptionDefault()
    {
        if (_loading || !CaptionExpander.IsGateOn)
            return;

        foreach (var key in DwmGlassScopeCatalog.CaptionStyleKeys)
            SettingsSession.Current.DeleteDwmValue(_hive, key, liveDwm: true);

        _loading = true;
        try
        {
            ReloadCaptionSection();
        }
        finally
        {
            _loading = false;
        }

        MarkDirtyAndInvalidate();
    }

    private void OnCaptionColorDefault(CaptionColorSection section)
    {
        if (_loading || !IsCaptionColorGateOn(section))
            return;

        SettingsSession.Current.DeleteDwmValue(_hive, section.RegistryKey, liveDwm: true);

        if (section.PropagateInheritance)
            PropagateCaptionInheritance();

        _loading = true;
        try
        {
            ReloadCaptionColorSection(section);
        }
        finally
        {
            _loading = false;
        }

        MarkDirtyAndInvalidate();
    }

    private void Reload()
    {
        _loading = true;
        try
        {
            ClearArmedIfKeysPresent();
            ReloadGeneralSection();
            ReloadWindowSection();
            ReloadCaptionSection();
            ReloadAllCaptionColors();
            UpdateD3DDependentControls();
        }
        finally
        {
            _loading = false;
        }

        UpdateParameterPanelDefaultStates();
        RefreshCaptionPivotTabIndicators();
        RefreshParallelBranchWarnings();
        _ = RefreshHkcuConflictNoticeAsync();
    }

    private void RefreshParallelBranchWarnings()
    {
        ApplySectionParallelWarning(
            (visible, tooltip, marker) => GeneralRow.SetParallelBranchWarning(visible, tooltip, marker),
            DwmGlassScopeCatalog.GeneralKeys);
        ApplySectionParallelWarning(
            (visible, tooltip, marker) => WindowExpander.SetParallelBranchWarning(visible, tooltip, marker),
            DwmGlassScopeCatalog.WindowKeys);
        ApplySectionParallelWarning(
            (visible, tooltip, marker) => CaptionExpander.SetParallelBranchWarning(visible, tooltip, marker),
            DwmGlassScopeCatalog.CaptionStyleKeys);
        ApplySectionParallelWarning(
            (visible, tooltip, marker) => _captionActiveNormal.Expander.SetParallelBranchWarning(visible, tooltip, marker),
            DwmGlassScopeCatalog.CaptionColorActiveKeys);
        ApplySectionParallelWarning(
            (visible, tooltip, marker) => _captionInactiveNormal.Expander.SetParallelBranchWarning(visible, tooltip, marker),
            DwmGlassScopeCatalog.CaptionColorInactiveKeys);
        ApplySectionParallelWarning(
            (visible, tooltip, marker) => _captionActiveMaximized.Expander.SetParallelBranchWarning(visible, tooltip, marker),
            DwmGlassScopeCatalog.CaptionColorActiveMaximizedKeys);
        ApplySectionParallelWarning(
            (visible, tooltip, marker) => _captionInactiveMaximized.Expander.SetParallelBranchWarning(visible, tooltip, marker),
            DwmGlassScopeCatalog.CaptionColorInactiveMaximizedKeys);
    }

    private void ApplySectionParallelWarning(Action<bool, string?, string?> setWarning, IEnumerable<string> keys) =>
        DwmParallelBranchUi.ApplySectionWarning(_hive, keys, setWarning);

    private void ClearArmedIfKeysPresent()
    {
        if (_generalGateArmed && IsGeneralGateOn(fromRegistryOnly: true))
            _generalGateArmed = false;
        if (_windowGateArmed && IsWindowGateOn(fromRegistryOnly: true))
            _windowGateArmed = false;
        if (_captionGateArmed && IsCaptionGateOn(fromRegistryOnly: true))
            _captionGateArmed = false;

        foreach (var section in EnumerateCaptionColorSections())
        {
            if (section.GateArmed && IsCaptionColorGateOn(section, fromRegistryOnly: true))
                section.GateArmed = false;
        }
    }

    private void ReloadGeneralSection()
    {
        GeneralRow.IsGateOn = IsGeneralGateOn();
        var blurOpt = (int)(DwmRegistryStore.ReadDwordForPage(_hive, "BlurOptimization") ?? 0);
        _blurOptimizationCombo.SelectedIndex = Math.Clamp(blurOpt, 0, _blurOptimizationCombo.Items.Count - 1);
        _useD3DCheck.IsChecked = (DwmRegistryStore.ReadDwordForPage(_hive, "UseDirect3DRendering") ?? 0) != 0;
    }

    private void ReloadWindowSection()
    {
        WindowExpander.IsGateOn = IsWindowGateOn();
        var radius = DwmRegistryStore.ReadDwordForPage(_hive, "RoundRectRadius") ?? 0;
        if (!DwmRegistryStore.ValueExistsForPage(_hive, "RoundRectRadius") || radius == 0)
        {
            _roundRectProfileCombo.SelectedIndex = RoundRectProfileWin8;
            _roundRectRadiusBox.Value = 0;
            _roundRectRadiusBox.IsEnabled = false;
        }
        else if (radius == 6)
        {
            _roundRectProfileCombo.SelectedIndex = RoundRectProfileWin7;
            _roundRectRadiusBox.Value = 6;
            _roundRectRadiusBox.IsEnabled = false;
        }
        else
        {
            _roundRectProfileCombo.SelectedIndex = RoundRectProfileCustom;
            _roundRectRadiusBox.Value = radius;
            _roundRectRadiusBox.IsEnabled = WindowExpander.IsGateOn;
        }
        SettingsNumberBoxCommit.SyncCommittedValue(
            _roundRectRadiusBox,
            _roundRectRadiusBox.Value);

        var captionButtons = (int)(DwmRegistryStore.ReadDwordForPage(_hive, "CaptionButtons") ?? 0);
        _captionButtonsCombo.SelectedIndex = Math.Clamp(captionButtons, 0, _captionButtonsCombo.Items.Count - 1);
        _disableModernBordersToggle.IsOn =
            (DwmRegistryStore.ReadDwordForPage(_hive, "DisableModernBorders") ?? 0) != 0;
    }

    private void ReloadCaptionSection()
    {
        CaptionExpander.IsGateOn = IsCaptionGateOn();
        var center = (int)(DwmRegistryStore.ReadDwordForPage(_hive, "CenterCaption") ?? 0);
        _centerCaptionCombo.SelectedIndex = Math.Clamp(center, 0, _centerCaptionCombo.Items.Count - 1);

        var glowPacked = DwmRegistryStore.ReadDwordForPage(_hive, "TextGlowMode") ?? 1;
        var glowMode = (int)(glowPacked & 0xFFFF);
        var glowSize = (int)(glowPacked >> 16);
        _textGlowModeCombo.SelectedIndex = Math.Clamp(glowMode, 0, _textGlowModeCombo.Items.Count - 1);
        _textGlowSizeBox.Value = Math.Clamp(glowSize, 0, 100);
        SettingsNumberBoxCommit.SyncCommittedValue(_textGlowSizeBox, _textGlowSizeBox.Value);
        _textGlowSizeBox.IsEnabled = CaptionExpander.IsGateOn && glowMode == TextGlowModeComposited;
    }

    private void ReloadAllCaptionColors()
    {
        ReloadCaptionColorSection(_captionActiveNormal);
        ReloadCaptionColorSection(_captionInactiveNormal);
        ReloadCaptionColorSection(_captionActiveMaximized);
        ReloadCaptionColorSection(_captionInactiveMaximized);
    }

    private void ReloadCaptionColorSection(CaptionColorSection section)
    {
        var gateOn = IsCaptionColorGateOn(section);
        section.Expander.IsGateOn = gateOn;
        section.ModeCombo.IsEnabled = gateOn;

        var effective = section.ResolveEffective();
        var mode = DwmCaptionColorHelper.ModeToComboIndex(effective);
        section.ModeCombo.SelectedIndex = mode;

        if (DwmCaptionColorHelper.IsCustomBgr(effective))
        {
            section.LastCustomBgr = effective;
            section.Picker.SetColor(DwmCaptionColorHelper.DwordBgrToPickerArgb(effective));
        }
        else
        {
            section.Picker.SetColor(DwmCaptionColorHelper.DwordBgrToPickerArgb(section.LastCustomBgr));
        }

        section.Picker.IsEnabled = gateOn && mode == DwmCaptionColorHelper.ModeCustom;
        ApplyCaptionColorExpansionState(section);

        var keyExists = DwmRegistryStore.ValueExistsForPage(_hive, section.RegistryKey);
        var isAuto = mode == DwmCaptionColorHelper.ModeAuto;
        section.Panel.SetDefaultColorActionState(!isAuto || keyExists, disableDefaultButton: !keyExists);
    }

    private void PropagateCaptionInheritance()
    {
        if (!DwmRegistryStore.ValueExistsForPage(_hive, "ColorizationColorCaptionInactive"))
            ReloadCaptionColorSection(_captionInactiveNormal);
        if (!DwmRegistryStore.ValueExistsForPage(_hive, "ColorizationColorCaptionMaximized"))
            ReloadCaptionColorSection(_captionActiveMaximized);
        if (!DwmRegistryStore.ValueExistsForPage(_hive, "ColorizationColorCaptionInactiveMaximized"))
            ReloadCaptionColorSection(_captionInactiveMaximized);
    }

    private bool IsGeneralGateOn(bool fromRegistryOnly = false) =>
        DwmRegistryStore.HasAnyPresentForPage(_hive, DwmGlassScopeCatalog.GeneralKeys)
        || (!fromRegistryOnly && _generalGateArmed);

    private bool IsWindowGateOn(bool fromRegistryOnly = false) =>
        DwmRegistryStore.HasAnyPresentForPage(_hive, DwmGlassScopeCatalog.WindowKeys)
        || (!fromRegistryOnly && _windowGateArmed);

    private bool IsCaptionGateOn(bool fromRegistryOnly = false) =>
        DwmRegistryStore.HasAnyPresentForPage(_hive, DwmGlassScopeCatalog.CaptionStyleKeys)
        || (!fromRegistryOnly && _captionGateArmed);

    private bool IsCaptionColorGateOn(CaptionColorSection section, bool fromRegistryOnly = false) =>
        DwmRegistryStore.ValueExistsForPage(_hive, section.RegistryKey)
        || (!fromRegistryOnly && section.GateArmed);

    private bool IsBlurOptimizationEnabled() =>
        (DwmRegistryStore.ReadDwordForPage(_hive, "UseDirect3DRendering") ?? 0) == 0;

    private void UpdateD3DDependentControls()
    {
        var useD3D = (DwmRegistryStore.ReadDwordForPage(_hive, "UseDirect3DRendering") ?? 0) != 0;
        _useD3DCheck.IsEnabled = GeneralRow.IsGateOn;
        _blurOptimizationCombo.IsEnabled = GeneralRow.IsGateOn && !useD3D;
        if (useD3D)
            _blurOptimizationCombo.SelectedIndex = 0;
    }

    private async void GeneralRow_OnGateToggled(object sender, RoutedEventArgs e)
    {
        if (_loading || PageShell.Operations.ShouldBlockPageMutation)
            return;

        await PageShell.Operations.RunSequenceAsync(async seq =>
        {
            await seq.RegistryMutationAsync(() =>
            {
                if (GeneralRow.IsGateOn)
                {
                    _generalGateArmed = true;
                }
                else
                {
                    _generalGateArmed = false;
                    foreach (var key in DwmGlassScopeCatalog.GeneralKeys)
                        SettingsSession.Current.DeleteDwmValue(_hive, key, liveDwm: true);
                }

                _loading = true;
                try { ReloadGeneralSection(); UpdateD3DDependentControls(); }
                finally { _loading = false; }
                return Task.CompletedTask;
            });

            await seq.CosmeticAsync(() => SettingsUnsavedIndicatorRefresh.Refresh(RootPanel));
            await seq.NoticeSyncAsync(() => PageShell.FlushSessionNotice());
        }).ConfigureAwait(true);

        RefreshParallelBranchWarnings();
        InvalidateNoticeDismiss();
    }

    private async void WindowExpander_OnGateToggled(object sender, RoutedEventArgs e)
    {
        if (_loading || PageShell.Operations.ShouldBlockPageMutation)
            return;

        await PageShell.Operations.RunSequenceAsync(async seq =>
        {
            await seq.RegistryMutationAsync(() =>
            {
                if (WindowExpander.IsGateOn)
                {
                    _windowGateArmed = true;
                }
                else
                {
                    _windowGateArmed = false;
                    foreach (var key in DwmGlassScopeCatalog.WindowKeys)
                        SettingsSession.Current.DeleteDwmValue(_hive, key, liveDwm: true);
                }

                _loading = true;
                try { ReloadWindowSection(); }
                finally { _loading = false; }
                return Task.CompletedTask;
            });

            await seq.CosmeticAsync(() =>
            {
                SettingsUnsavedIndicatorRefresh.Refresh(RootPanel);
                EnsureExpanderHeaderChrome(WindowExpander);
                UpdateParameterPanelDefaultStates();
                RefreshParallelBranchWarnings();
            });
            await seq.NoticeSyncAsync(() => PageShell.FlushSessionNotice());
        }).ConfigureAwait(true);

        InvalidateNoticeDismiss();
    }

    private async void CaptionExpander_OnGateToggled(object sender, RoutedEventArgs e)
    {
        if (_loading || PageShell.Operations.ShouldBlockPageMutation)
            return;

        await PageShell.Operations.RunSequenceAsync(async seq =>
        {
            await seq.RegistryMutationAsync(() =>
            {
                if (CaptionExpander.IsGateOn)
                {
                    _captionGateArmed = true;
                }
                else
                {
                    _captionGateArmed = false;
                    foreach (var key in DwmGlassScopeCatalog.CaptionStyleKeys)
                        SettingsSession.Current.DeleteDwmValue(_hive, key, liveDwm: true);
                }

                _loading = true;
                try { ReloadCaptionSection(); }
                finally { _loading = false; }
                return Task.CompletedTask;
            });

            await seq.CosmeticAsync(() =>
            {
                SettingsUnsavedIndicatorRefresh.Refresh(RootPanel);
                EnsureExpanderHeaderChrome(CaptionExpander);
                UpdateParameterPanelDefaultStates();
                RefreshParallelBranchWarnings();
            });
            await seq.NoticeSyncAsync(() => PageShell.FlushSessionNotice());
        }).ConfigureAwait(true);

        InvalidateNoticeDismiss();
    }

    private async void CaptionColorGate_OnToggled(object? sender, RoutedEventArgs e)
    {
        if (_loading || PageShell.Operations.ShouldBlockPageMutation)
            return;

        if (sender is not SettingsSystemGateExpander expander)
            return;

        var section = FindCaptionColorSection(expander);
        if (section is null)
            return;

        await PageShell.Operations.RunSequenceAsync(async seq =>
        {
            await seq.RegistryMutationAsync(() =>
            {
                if (section.Expander.IsGateOn)
                {
                    section.GateArmed = true;
                }
                else
                {
                    section.GateArmed = false;
                    SettingsSession.Current.DeleteDwmValue(_hive, section.RegistryKey, liveDwm: true);
                    section.Expander.IsExpanded = false;
                }

                _loading = true;
                try
                {
                    ReloadCaptionColorSection(section);
                    if (section.PropagateInheritance)
                        PropagateCaptionInheritance();
                }
                finally
                {
                    _loading = false;
                }

                return Task.CompletedTask;
            });

            await seq.CosmeticAsync(() =>
            {
                SettingsUnsavedIndicatorRefresh.Refresh(RootPanel);
                EnsureExpanderHeaderChrome(section.Expander);
                section.Expander.RefreshHeaderActionsLayout();
                RefreshParallelBranchWarnings();
            });
            await seq.NoticeSyncAsync(() => PageShell.FlushSessionNotice());
        }).ConfigureAwait(true);

        InvalidateNoticeDismiss();
    }

    private void OnBlurOptimizationChanged()
    {
        if (_loading || !GeneralRow.IsGateOn || !IsBlurOptimizationEnabled())
            return;

        var sel = _blurOptimizationCombo.SelectedIndex;
        SettingsSession.Current.WriteDwmDwordOrDelete(
            _hive,
            "BlurOptimization",
            (uint)sel,
            v => v == 0,
            liveDwm: true);
        MarkDirtyAndInvalidate();
    }

    private void UseD3DCheck_OnChanged(object sender, RoutedEventArgs e)
    {
        if (_loading || !GeneralRow.IsGateOn)
            return;

        if (_useD3DCheck.IsChecked == true)
            SettingsSession.Current.WriteDwmDword(_hive, "UseDirect3DRendering", 1, liveDwm: true, dwmNotifyImmediate: true);
        else
            SettingsSession.Current.DeleteDwmValue(_hive, "UseDirect3DRendering", liveDwm: true);

        UpdateD3DDependentControls();
        MarkDirtyAndInvalidate();
    }

    private void OnRoundRectProfileChanged()
    {
        if (_loading || !WindowExpander.IsGateOn)
            return;

        var sel = _roundRectProfileCombo.SelectedIndex;
        if (sel == RoundRectProfileWin8)
        {
            _roundRectRadiusBox.Value = 0;
            _roundRectRadiusBox.IsEnabled = false;
            SettingsSession.Current.DeleteDwmValue(_hive, "RoundRectRadius", liveDwm: true);
        }
        else if (sel == RoundRectProfileWin7)
        {
            _roundRectRadiusBox.Value = 6;
            _roundRectRadiusBox.IsEnabled = false;
            SettingsSession.Current.WriteDwmDword(_hive, "RoundRectRadius", 6, liveDwm: true, dwmNotifyImmediate: true);
        }
        else
        {
            _roundRectRadiusBox.IsEnabled = true;
            WriteRoundRectCustom((uint)_roundRectRadiusBox.Value);
        }

        MarkDirtyAndInvalidate();
    }

    private void RoundRectRadius_OnCommit(double value)
    {
        WriteRoundRectCustom((uint)value);
        MarkDirtyAndInvalidate();
    }

    private void WriteRoundRectCustom(uint value)
    {
        SettingsSession.Current.WriteDwmDwordOrDelete(
            _hive,
            "RoundRectRadius",
            value,
            v => v == 0,
            liveDwm: true);
    }

    private void OnCaptionButtonsChanged()
    {
        if (_loading || !WindowExpander.IsGateOn)
            return;

        SettingsSession.Current.WriteDwmDwordOrDelete(
            _hive,
            "CaptionButtons",
            (uint)_captionButtonsCombo.SelectedIndex,
            v => v == 0,
            liveDwm: true);
        MarkDirtyAndInvalidate();
    }

    private void DisableModernBordersToggle_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading || !WindowExpander.IsGateOn)
            return;

        if (_disableModernBordersToggle.IsOn)
            SettingsSession.Current.WriteDwmDword(_hive, "DisableModernBorders", 1, liveDwm: true, dwmNotifyImmediate: true);
        else
            SettingsSession.Current.DeleteDwmValue(_hive, "DisableModernBorders", liveDwm: true);

        MarkDirtyAndInvalidate();
    }

    private void OnCenterCaptionChanged()
    {
        if (_loading || !CaptionExpander.IsGateOn)
            return;

        SettingsSession.Current.WriteDwmDwordOrDelete(
            _hive,
            "CenterCaption",
            (uint)_centerCaptionCombo.SelectedIndex,
            v => v == 0,
            liveDwm: true);
        MarkDirtyAndInvalidate();
    }

    private void OnTextGlowModeChanged()
    {
        if (_loading || !CaptionExpander.IsGateOn)
            return;

        _textGlowSizeBox.IsEnabled = _textGlowModeCombo.SelectedIndex == TextGlowModeComposited;
        WriteTextGlowMode();
        MarkDirtyAndInvalidate();
    }

    private void TextGlowSize_OnCommit(double value)
    {
        WriteTextGlowMode((uint)value);
        MarkDirtyAndInvalidate();
    }

    private void WriteTextGlowMode(uint? sizeOverride = null)
    {
        var mode = _textGlowModeCombo.SelectedIndex;
        var size = sizeOverride ?? (uint)Math.Clamp(_textGlowSizeBox.Value, 0, 100);
        var packed = (uint)mode | (size << 16);
        SettingsSession.Current.WriteDwmDwordOrDelete(
            _hive,
            "TextGlowMode",
            packed,
            v => v == 1,
            liveDwm: true);
    }

    private void OnCaptionColorModeChanged(CaptionColorSection section)
    {
        if (_loading || !IsCaptionColorGateOn(section))
            return;

        var mode = section.ModeCombo.SelectedIndex;
        section.Picker.IsEnabled = mode == DwmCaptionColorHelper.ModeCustom;

        if (mode == DwmCaptionColorHelper.ModeCustom)
        {
            section.Expander.IsExpanded = true;
            CommitCaptionColor(section, mode, section.LastCustomBgr);
            return;
        }

        DwmCaptionColorHelper.CommitMode(_hive, section.RegistryKey, mode, 0);
        if (mode == DwmCaptionColorHelper.ModeAuto)
            SettingsSession.Current.MarkDirty();

        if (section.PropagateInheritance)
            PropagateCaptionInheritance();

        _loading = true;
        try
        {
            ReloadCaptionColorSection(section);
        }
        finally
        {
            _loading = false;
        }

        MarkDirtyAndInvalidate();
    }

    private void CommitCaptionColor(CaptionColorSection section, int mode, uint customBgr)
    {
        DwmCaptionColorHelper.CommitMode(_hive, section.RegistryKey, mode, customBgr);
        if (mode == DwmCaptionColorHelper.ModeCustom)
            section.LastCustomBgr = customBgr;

        if (section.PropagateInheritance)
            PropagateCaptionInheritance();

        _loading = true;
        try
        {
            ReloadCaptionColorSection(section);
        }
        finally
        {
            _loading = false;
        }

        MarkDirtyAndInvalidate();
    }

    private async void CaptionColorPivot_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading)
            return;

        var newIndex = CaptionColorPivot.SelectedIndex;
        var previousIndex = _lastCaptionPivotIndex;
        if (previousIndex == newIndex)
            return;

        await PivotTabContentTransition.AnimateTabChangeAsync(
            CaptionTabContentHost,
            previousIndex,
            newIndex,
            () =>
            {
                CaptionNormalPanel.Visibility = newIndex == 0 ? Visibility.Visible : Visibility.Collapsed;
                CaptionMaximizedPanel.Visibility = newIndex == 1 ? Visibility.Visible : Visibility.Collapsed;
            }).ConfigureAwait(true);

        _lastCaptionPivotIndex = newIndex;
        RefreshCaptionPivotTabIndicators();
        RefreshCaptionExpandersChrome();
        foreach (var section in EnumerateCaptionColorSections())
            section.Expander.RefreshHeaderActionsLayout(resetStackState: true);
        SettingsUnsavedIndicatorRefresh.Refresh(RootPanel);
    }

    private void ApplyUnsavedScopes()
    {
        GeneralRow.UnsavedScope = DwmGlassScopeCatalog.GeneralScope(_hive);
        WindowExpander.UnsavedScope = DwmGlassScopeCatalog.WindowScope(_hive);
        CaptionExpander.UnsavedScope = DwmGlassScopeCatalog.CaptionStyleScope(_hive);

        _captionActiveNormal.Expander.UnsavedScope = DwmGlassScopeCatalog.ForKeys(
            DwmGlassScopeCatalog.CaptionColorActiveKeys, _hive);
        _captionInactiveNormal.Expander.UnsavedScope = DwmGlassScopeCatalog.ForKeys(
            DwmGlassScopeCatalog.CaptionColorInactiveKeys, _hive);
        _captionActiveMaximized.Expander.UnsavedScope = DwmGlassScopeCatalog.ForKeys(
            DwmGlassScopeCatalog.CaptionColorActiveMaximizedKeys, _hive);
        _captionInactiveMaximized.Expander.UnsavedScope = DwmGlassScopeCatalog.ForKeys(
            DwmGlassScopeCatalog.CaptionColorInactiveMaximizedKeys, _hive);
    }

    private void RefreshCaptionPivotTabIndicators()
    {
        _captionPivotHeaders[0].SetUnsavedVisible(
            SettingsSession.Current.HasUnsavedInScope(DwmGlassScopeCatalog.CaptionColorNormalScope(_hive)));
        _captionPivotHeaders[1].SetUnsavedVisible(
            SettingsSession.Current.HasUnsavedInScope(DwmGlassScopeCatalog.CaptionColorMaximizedScope(_hive)));
    }

    private void MarkDirtyAndInvalidate()
    {
        SettingsSession.Current.MarkDirty();
        PageSessionNoticeState.InvalidateDismissOnMutation();
        HomeSessionNoticeState.InvalidateDismissOnMutation();
        if (!_loading)
            UpdateParameterPanelDefaultStates();
    }

    private static void InvalidateNoticeDismiss()
    {
        PageSessionNoticeState.InvalidateDismissOnMutation();
        HomeSessionNoticeState.InvalidateDismissOnMutation();
    }

    private async Task RefreshHkcuConflictNoticeAsync()
    {
        var stack = PageShell.PageNoticeStack;
        var strip = stack.ActionNotice;
        var showConflict = _hive == DwmRegistryHive.Hkcu
                           && DwmRegistryStore.HasAnyPresent(DwmRegistryHive.Hklm, GlassKeys);
        if (showConflict)
        {
            strip.AllowDismiss = false;
            var host = SettingsNoticeCoordinator.CreateShellHost(stack, PageShell.PageScrollViewer);
            await SettingsNoticeCoordinator.PresentAsync(host, new SettingsNoticeCoordinator.NoticePresentRequest
            {
                Strip = strip,
                Severity = SettingsNoticeSeverity.Error,
                Message = GlassSettingsResources.Get("Dwm_HkcuConflictNotice"),
                MirrorSuppressKey = "dwm.hkcu-conflict.glass",
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
        SettingsCommitScope.ForDwmHiveArea(CustomizationAreaId.DwmGlass, _hive);

    public void ReloadFromExternalRegistrySync()
    {
        if (SettingsPageOperationGate.Current?.ShouldDeferRegistryWatch == true)
            return;

        Reload();
    }
}
