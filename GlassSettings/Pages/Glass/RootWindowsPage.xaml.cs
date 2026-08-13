using GlassSettings.Controls;
using GlassSettings.Core.Customization;
using GlassSettings.Core.Flyout;
using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Root;
using GlassSettings.Core.Snapshot;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Documents;
using Microsoft.UI.Xaml.Navigation;
using System.Diagnostics;

namespace GlassSettings.Pages.Glass;

public sealed partial class RootWindowsPage : Page, ISettingsCommitScopeProvider
{
    private const string RootRegSub = "Root";
    private const string RootCornerRegSub = "Root";
    private const int ThemeAlphaDebounceMs = 150;

    private ComboBox? _effectCombo;
    private ComboBox? _cornerCombo;
    private readonly HubInlineLabel _cornerInlineLabel = new();
    private readonly TextBlock _noBorderLabel = new();
    private readonly ToggleSwitch _noBorderSwitch = new()
    {
        OnContent = string.Empty,
        OffContent = string.Empty,
        MinWidth = 0,
        HorizontalAlignment = HorizontalAlignment.Left,
        VerticalAlignment = VerticalAlignment.Center,
    };
    private Slider? _themeAlphaSlider;
    private string _regSub = "Root\\Active";
    private bool _loading;
    private bool _effectGateArmed;
    private bool _isPerWindowStateScope;
    private int _lastPivotIndex;
    private readonly bool _cornersSupported = WindowsOsHelper.IsWindows11OrGreater();

    // «Дополнительно»: удаление фона иконок Ribbon / ListView / TreeView (Root\Control\{Kind}).
    private readonly NumberBox _controlIconThresholdBox;
    private readonly TextBlock _controlIconThresholdLabel = new() { VerticalAlignment = VerticalAlignment.Center };
    private readonly SettingsPivotTabHeader[] _controlIconTabHeaders = new SettingsPivotTabHeader[6];
    private RootControlIconKind _controlIconKind = RootControlIconKind.Ribbon;
    private bool _controlIconGateArmed;
    private int _lastControlIconPivotIndex;

    private static readonly string[] ControlIconPivotHeaderKeys =
    [
        "Glass_ControlIcon_Ribbon",
        "Glass_ControlIcon_ListView",
        "Glass_ControlIcon_TreeView",
        "Glass_ControlIcon_Toolbar",
        "Glass_ControlIcon_Button",
        "Glass_ControlIcon_Static",
    ];

    private static readonly string[] ControlIconSubtitleKeys =
    [
        "Glass_ControlIcon_Ribbon_Subtitle",
        "Glass_ControlIcon_ListView_Subtitle",
        "Glass_ControlIcon_TreeView_Subtitle",
        "Glass_ControlIcon_Toolbar_Subtitle",
        "Glass_ControlIcon_Button_Subtitle",
        "Glass_ControlIcon_Static_Subtitle",
    ];

    private static readonly string[] ControlIconDescriptionKeys =
    [
        "Glass_ControlIcon_Ribbon_Desc",
        "Glass_ControlIcon_ListView_Desc",
        "Glass_ControlIcon_TreeView_Desc",
        "Glass_ControlIcon_Toolbar_Desc",
        "Glass_ControlIcon_Button_Desc",
        "Glass_ControlIcon_Static_Desc",
    ];

    private static int ClampControlIconIndex(int index) =>
        index >= 0 && index < ControlIconDescriptionKeys.Length ? index : 0;

    public RootWindowsPage()
    {
        _controlIconThresholdBox = new NumberBox
        {
            Style = (Style)Application.Current.Resources["GlassSettingsNumberBoxStyle"],
            SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Inline,
            SmallChange = 1,
            LargeChange = 1,
            Minimum = 0,
            Maximum = 510,
        };

        InitializeComponent();
        BuildNoBorderFooter();
        ConfigureControlIconSection();
        ApplyLocalizedStrings();
        EffectRow.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.EffectType);
        GradientExpander.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.Gradient);
        BorderExpander.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.Border);
        CornerRow.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.Corners);
        DrawThemeRow.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.DrawTheme);

        if (!_cornersSupported)
            CornerRow.Visibility = Visibility.Collapsed;

        _effectCombo = new ComboBox { MinWidth = 200 };
        FlyoutEffectNames.PopulateCombo(_effectCombo);
        SettingsInteractionDeferral.WhenComboCommit(
            _effectCombo,
            () => !_loading && EffectRow.IsGateOn,
            OnEffectChanged);
        EffectRow.HeaderActions = _effectCombo;

        if (_cornersSupported)
        {
            _cornerCombo = new ComboBox { MinWidth = 200 };
            FlyoutCornerNames.PopulateCombo(_cornerCombo);
            _cornerCombo.SelectionChanged += (_, _) =>
            {
                if (_loading || !CornerRow.IsGateOn)
                    return;

                OnCornerTypeCommitted();
            };
            _cornerInlineLabel.Control = _cornerCombo;
            CornerRow.HeaderActions = _cornerInlineLabel;
        }

        _themeAlphaSlider = SettingsSliderFactory.Create(0, 255);
        SettingsSliderCommit.Attach(
            _themeAlphaSlider,
            () => !_loading && DrawThemeRow.IsGateOn,
            _ => { },
            value => RootWindowsLiveReload.ScheduleDrawThemeAlphaLive(
                (uint)value, _regSub, "GlassDrawThemeConstantAlpha"),
            SettingsSliderCommit.CommitMode.Deferred,
            () => SettingsSession.Current.MarkDirty(),
            ThemeAlphaDebounceMs);
        SettingsSliderFactory.Apply(_themeAlphaSlider, stretch: true);
        DrawThemeRow.HeaderControlsTable.SetCell(
            0,
            GlassSettingsResources.Get("Flyout_LabelAlpha"),
            _themeAlphaSlider!);
        DrawThemeRow.RefreshHeaderLayout();

        GradientPicker.UseLivePreview = true;
        BorderPicker.UseLivePreview = true;
        GradientPicker.ColorLivePreview += (_, argb) =>
        {
            if (_loading || !GradientExpander.IsGateOn)
                return;

            UpdatePickerDefaultStates();
            LiveReloadScheduler.ScheduleRegistryWrite(
                _regSub, ThemeRegistry.GradientColorName(), argb);
        };
        BorderPicker.ColorLivePreview += (_, argb) =>
        {
            if (_loading || !BorderExpander.IsGateOn)
                return;

            UpdatePickerDefaultStates();
            LiveReloadScheduler.ScheduleRegistryWrite(
                _regSub, ThemeRegistry.BorderColorName(), argb);
        };
        GradientPicker.ColorChanged += (_, argb) => OnRootGradientColorCommitted(argb);
        BorderPicker.ColorChanged += (_, argb) => OnRootBorderColorCommitted(argb);
        GradientPanel.DefaultColorClick += (_, _) => OnGradientDefaultColor();
        BorderPanel.DefaultColorClick += (_, _) => OnBorderDefaultColor();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    private void BuildNoBorderFooter()
    {
        var labelStyle = Application.Current.Resources["GlassSettingsSpoilerSecondaryTextStyle"] as Style;
        _noBorderLabel.Style = labelStyle;
        _noBorderLabel.VerticalAlignment = VerticalAlignment.Center;
        _noBorderSwitch.Style =
            Application.Current.Resources["GlassSettingsCompactToggleSwitchStyle"] as Style;
        _noBorderSwitch.Toggled += NoBorderSwitch_OnToggled;

        var table = SettingsPickerBodyLayout.CreateBodyTable();
        SettingsPickerBodyLayout.AddLabeledRow(table, _noBorderLabel, _noBorderSwitch);
        BorderPanel.FooterContent = table;
        BorderExpander.WireExpandedLayoutRefresh(() => BorderPanel.RefreshLayout());
    }

    private void ApplyLocalizedStrings()
    {
        FullGlassRow.ApplyLocalizedStrings(
            GlassSettingsResources.Get("Root_AllowBackdropTitle"),
            GlassSettingsResources.Get("Root_AllowBackdropDesc"));

        if (RootPivot.Items.Count >= 2)
        {
            ((PivotItem)RootPivot.Items[0]).Header = GlassSettingsResources.Get("Dwm_PivotActive");
            ((PivotItem)RootPivot.Items[1]).Header = GlassSettingsResources.Get("Dwm_PivotInactive");
        }

        EffectRow.Title = GlassSettingsResources.Get("Flyout_EffectType");
        ApplyEffectRowDescription();

        var gradientDesc = GlassSettingsResources.Get("Flyout_GradientColorDesc");
        GradientExpander.Title = GlassSettingsResources.Get("Flyout_GradientColor");
        GradientExpander.Description = GlassSettingsResources.Get("Flyout_GradientColorSubtitle");
        GradientPanel.Description = gradientDesc;

        var borderDesc = GlassSettingsResources.Get("Flyout_BorderColorDesc");
        BorderExpander.Title = GlassSettingsResources.Get("Flyout_BorderColor");
        BorderExpander.Description = GlassSettingsResources.Get("Flyout_BorderColorSubtitle");
        BorderPanel.Description = borderDesc;
        _noBorderLabel.Text = GlassSettingsResources.Get("Flyout_NoBorder");

        if (_cornersSupported)
        {
            CornerRow.Title = GlassSettingsResources.Get("Flyout_CornerType");
            CornerRow.Description = GlassSettingsResources.Get("Flyout_CornerTypeDesc");
            _cornerInlineLabel.Label = GlassSettingsResources.Get("ParamLabel_CornerType");

            var cornerIndex = _cornerCombo?.SelectedIndex ?? -1;
            if (_cornerCombo is not null)
            {
                FlyoutCornerNames.PopulateCombo(_cornerCombo);
                if (cornerIndex >= 0)
                    _cornerCombo.SelectedIndex = cornerIndex;
            }
        }

        DrawThemeRow.Title = GlassSettingsResources.Get("Flyout_DrawThemeAlpha");
        DrawThemeRow.Description = GlassSettingsResources.Get("Flyout_DrawThemeAlphaSubtitle");

        OpenGlassBorderNotice.Title = GlassSettingsResources.Get("Notice_OpenGlassBorderTitle");
        OpenGlassBorderNotice.Message = OpenGlassBorderPolicyCoordinator.FormatBorderLimitNoticeMessage(includeTaskbarSuffix: false);

        DwmSystemBackdropNotice.Title = GlassSettingsResources.Get("Notice_SwcaDwmBackdropTitle");

        RootAdvancedExpander.Title = GlassSettingsResources.Get("Dwm_AdvancedColorizationTitle");
        BuildControlIconPivotHeaders();
        ControlIconExpander.Title = GlassSettingsResources.Get("Glass_ControlIcon_Title");
        ControlIconExpander.Description = GlassSettingsResources.Get(
            ControlIconSubtitleKeys[ClampControlIconIndex(ControlIconPivot.SelectedIndex)]);
        ControlIconPanel.Description = GlassSettingsResources.Get(ControlIconDescriptionKeys[ClampControlIconIndex(ControlIconPivot.SelectedIndex)]);
        _controlIconThresholdLabel.Text = GlassSettingsResources.Get("Glass_ControlIcon_Threshold_Label");

        BuildOpenGlassRootBackdropNotice();
    }

    private void BuildOpenGlassRootBackdropNotice()
    {
        OpenGlassRootBackdropNotice.Title = GlassSettingsResources.Get("Notice_OgRootBackdropTitle");
        var text = OpenGlassRootBackdropNotice.MessageTextBlock;
        text.Inlines.Clear();
        AddInlineText(text, GlassSettingsResources.Get("Notice_OgRootBackdropBody"));
        AddInlineText(text, " ");

        if (OpenGlassUiPolicy.IsOrchestratedExternally())
            AddRootBackdropExternalHint(text);
        else
            AddRootBackdropGlassHint(text);
    }

    private static void AddInlineText(TextBlock target, string value) =>
        target.Inlines.Add(new Run { Text = value });

    private static void AddRootBackdropGlassHint(TextBlock target)
    {
        AddInlineText(target, GlassSettingsResources.Get("Notice_OgRootBackdrop_ActionGlassBefore"));
        AddRootBackdropHyperlink(
            target,
            GlassSettingsResources.Get("Notice_OgRootBackdrop_ActionGlassLink"),
            () => App.ShellWindow?.NavigateToDwmSettings());
        AddInlineText(target, GlassSettingsResources.Get("Notice_OgRootBackdrop_ActionGlassAfter"));
    }

    private static void AddRootBackdropHyperlink(TextBlock target, string linkText, Action navigate)
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

    private static void AddRootBackdropExternalHint(TextBlock target)
    {
        var exe = OpenGlassVersionService.TryResolveOpenGlassGuiExePath();
        if (string.IsNullOrWhiteSpace(exe))
        {
            AddInlineText(target, GlassSettingsResources.Get("Notice_OgRootBackdrop_ActionExternal"));
            return;
        }

        var link = new Hyperlink();
        link.Inlines.Add(new Run
        {
            Text = GlassSettingsResources.Get("Notice_OgRootBackdrop_ActionExternal"),
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold
        });
        link.Click += (_, _) =>
        {
            try
            {
                Process.Start(new ProcessStartInfo(exe) { UseShellExecute = true });
            }
            catch
            {
                // ignore
            }
        };
        target.Inlines.Add(link);
    }

    private void ApplyEffectRowDescription()
    {
        EffectRow.Description = _isPerWindowStateScope
            ? FlyoutEffectScopeDescriptions.ForRootPivotIndex(RootPivot.SelectedIndex)
            : GlassSettingsResources.Get("Root_ScopeStateIndependentDesc");
    }

    private void InitializeScopeFromRegistry()
    {
        _loading = true;
        _isPerWindowStateScope = CustomizationCatalog.IsRootPerWindowStateScope();
        ScopeBar.IsSeparateScope = _isPerWindowStateScope;
        UpdateRegSubFromScope();
        ApplyScopeUi();
        _loading = false;
    }

    private async void ScopeBar_OnScopeChanged(object sender, RoutedEventArgs e)
    {
        if (_loading)
            return;

        var newPerWindow = ScopeBar.IsSeparateScope;
        if (newPerWindow == _isPerWindowStateScope)
            return;

        await PivotTabContentTransition.AnimateVerticalChangeAsync(
            ScopeContentHost,
            toSeparate: newPerWindow,
            () =>
            {
                _isPerWindowStateScope = newPerWindow;
                UpdateRegSubFromScope();
                ApplyScopeUi();
                ApplyExpanderUnsavedScopes();
                Reload();
            }).ConfigureAwait(true);
    }

    private void ApplyScopeUi()
    {
        RootPivot.Visibility = _isPerWindowStateScope ? Visibility.Visible : Visibility.Collapsed;
        ApplyEffectRowDescription();
    }

    private void UpdateRegSubFromScope()
    {
        if (!_isPerWindowStateScope)
        {
            _regSub = RootRegSub;
            return;
        }

        _regSub = RootPivot.SelectedIndex == 1 ? "Root\\Inactive" : "Root\\Active";
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        OpenGlassBorderPolicyCoordinator.Changed += OnPageTopNoticesChanged;
        SwcaDwmBackdropNoticeCoordinator.Changed += OnPageTopNoticesChanged;
        OpenGlassRootBackdropNoticeCoordinator.Changed += OnPageTopNoticesChanged;
        OpenGlassUiPolicy.Changed += OnOpenGlassUiPolicyChanged;
        InitializeScopeFromRegistry();
        ApplyExpanderUnsavedScopes();
        FullGlassRow.SyncFromRegistry();
        Reload();
        _lastPivotIndex = RootPivot.SelectedIndex;
        _lastControlIconPivotIndex = ControlIconPivot.SelectedIndex;
        RootAdvancedExpander.RefreshUnsavedIndicator();
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        OpenGlassBorderPolicyCoordinator.Changed -= OnPageTopNoticesChanged;
        SwcaDwmBackdropNoticeCoordinator.Changed -= OnPageTopNoticesChanged;
        OpenGlassRootBackdropNoticeCoordinator.Changed -= OnPageTopNoticesChanged;
        OpenGlassUiPolicy.Changed -= OnOpenGlassUiPolicyChanged;
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        ApplyPageTopNoticesUi();
    }

    private void OnPageTopNoticesChanged(object? sender, EventArgs e) =>
        DispatcherQueue.TryEnqueue(ApplyPageTopNoticesUi);

    private void OnOpenGlassUiPolicyChanged(object? sender, EventArgs e) =>
        DispatcherQueue.TryEnqueue(() =>
        {
            BuildOpenGlassRootBackdropNotice();
            ApplyPageTopNoticesUi();
        });

    private void ApplyExpanderUnsavedScopes()
    {
        EffectRow.UnsavedScope = SettingsCommitScope.ForFlyoutParameterGroup(_regSub, FlyoutParameterGroup.Effect);
        GradientExpander.UnsavedScope = SettingsCommitScope.ForFlyoutParameterGroup(_regSub, FlyoutParameterGroup.Gradient);
        BorderExpander.UnsavedScope = SettingsCommitScope.ForFlyoutParameterGroup(_regSub, FlyoutParameterGroup.Border);
        if (_cornersSupported)
            CornerRow.UnsavedScope = SettingsCommitScope.ForFlyoutParameterGroup(RootCornerRegSub, FlyoutParameterGroup.Corners);
        ControlIconExpander.UnsavedScope = SettingsCommitScope.ForRootControlIcon(_controlIconKind);
    }

    public SettingsCommitScope GetCommitScope() =>
        SettingsCommitScope.ForArea(CustomizationAreaId.Root);

    private async void RootPivot_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || !_isPerWindowStateScope)
            return;

        var newIndex = RootPivot.SelectedIndex;
        var previousIndex = _lastPivotIndex;
        if (previousIndex == newIndex) return;

        await PivotTabContentTransition.AnimateTabChangeAsync(
            TabContentHost,
            previousIndex,
            newIndex,
            () =>
            {
                _regSub = newIndex == 1 ? "Root\\Inactive" : "Root\\Active";
                ApplyEffectRowDescription();
                Reload();
            }).ConfigureAwait(true);

        _lastPivotIndex = newIndex;
    }

    private void Reload()
    {
        _loading = true;
        ApplyExpanderUnsavedScopes();
        ApplyEffectGateUiFromRegistry();
        GradientExpander.IsGateOn = FlyoutThemeRegistry.IsGradientPresent(_regSub);
        GradientPicker.SetColor(FlyoutThemeRegistry.ReadGradientArgbForUi(_regSub));
        BorderExpander.IsGateOn = FlyoutThemeRegistry.IsBorderPresent(_regSub);
        var border = FlyoutThemeRegistry.ReadBorderArgbForUi(_regSub, ThemeRegistry.BorderColorName());
        _noBorderSwitch.IsOn = FlyoutPartRegistry.ReadNoBorderColorForUi(_regSub);
        BorderPicker.SetColor(border);
        BorderPanel.IsEnabled = BorderExpander.IsGateOn;
        if (_cornersSupported)
            ReloadCornerRow();
        var alphaOpt = GlassRegistry.ReadDword(_regSub, "GlassDrawThemeConstantAlpha");
        DrawThemeRow.IsGateOn = alphaOpt is not null;
        _themeAlphaSlider!.Value = alphaOpt ?? 255;
        _themeAlphaSlider.IsEnabled = DrawThemeRow.IsGateOn;
        SettingsSliderCommit.SyncCommittedValue(_themeAlphaSlider, _themeAlphaSlider.Value);
        ReloadControlIconSection();
        _loading = false;
        ApplyPageTopNoticesUi();
        UpdatePickerDefaultStates();
    }

    private uint ReadEffectTypeComboIndexForUi()
    {
        if (FlyoutEffectRegistry.IsPresent(_regSub))
            return FlyoutEffectRegistry.ReadUiIndex(_regSub);

        if (_isPerWindowStateScope
            && !string.Equals(_regSub, RootRegSub, StringComparison.Ordinal)
            && FlyoutEffectRegistry.IsPresent(RootRegSub))
        {
            return FlyoutEffectRegistry.ReadUiIndex(RootRegSub);
        }

        return FlyoutEffectRegistry.DefaultRootEffectType;
    }

    private void ApplyEffectGateUiFromRegistry()
    {
        var hasLocal = FlyoutEffectRegistry.IsPresent(_regSub);
        EffectRow.IsGateOn = hasLocal || _effectGateArmed;
        _effectCombo!.SelectedIndex = (int)ReadEffectTypeComboIndexForUi();
        _effectCombo.IsEnabled = EffectRow.IsGateOn;
        if (hasLocal)
            _effectGateArmed = false;
    }

    private void ReloadCornerRow()
    {
        CornerRow.IsGateOn = IsRootCornerTypePresentForUi();
        _cornerCombo!.SelectedIndex = (int)Math.Clamp(ReadRootCornerTypeForUi(), 0u, 3u);
        _cornerCombo.IsEnabled = CornerRow.IsGateOn;
    }

    private static bool IsRootCornerTypePresentForUi() =>
        FlyoutPartRegistry.IsCornerTypePresent(RootCornerRegSub)
        || FlyoutPartRegistry.IsCornerTypePresent("Root\\Active")
        || FlyoutPartRegistry.IsCornerTypePresent("Root\\Inactive");

    private static uint ReadRootCornerTypeForUi()
    {
        if (FlyoutPartRegistry.TryReadLocalDword(RootCornerRegSub, "CornerType", out var value))
            return value;
        if (FlyoutPartRegistry.TryReadLocalDword("Root\\Active", "CornerType", out value))
            return value;
        if (FlyoutPartRegistry.TryReadLocalDword("Root\\Inactive", "CornerType", out value))
            return value;

        return FlyoutPartRegistry.ReadCornerTypeForUi(RootCornerRegSub);
    }

    private void UpdatePickerDefaultStates()
    {
        if (_loading)
            return;

        var borderName = ThemeRegistry.BorderColorName();
        GradientPanel.SetDefaultColorActionState(
            GradientExpander.IsGateOn,
            FlyoutThemeRegistry.ShouldDisableGradientDefaultButton(_regSub));
        BorderPanel.SetDefaultColorActionState(
            BorderExpander.IsGateOn,
            FlyoutThemeRegistry.ShouldDisableBorderDefaultButton(_regSub, borderName));
    }

    private void OnGradientDefaultColor()
    {
        if (_loading || !GradientExpander.IsGateOn)
            return;

        var def = FlyoutThemeRegistry.ResetGradientToDefault(_regSub);
        GradientPicker.SetColor(def);
        SettingsSession.Current.MarkDirty();
        RootWindowsLiveReload.NotifyGradientColorChanged(_regSub);
        UpdatePickerDefaultStates();
    }

    private void OnBorderDefaultColor()
    {
        if (_loading || !BorderExpander.IsGateOn)
            return;

        var def = FlyoutThemeRegistry.ResetBorderToDefault(_regSub, ThemeRegistry.BorderColorName());
        BorderPicker.SetColor(def);
        SettingsSession.Current.MarkDirty();
        RootWindowsLiveReload.NotifyBorderColorChanged(_regSub);
        UpdatePickerDefaultStates();
    }

    private void ApplyPageTopNoticesUi()
    {
        OpenGlassRootBackdropNotice.Visibility =
            OpenGlassRootBackdropNoticeCoordinator.ShouldShowRootBackdropLimitNotice()
                ? Visibility.Visible
                : Visibility.Collapsed;
        OpenGlassBorderNotice.Visibility = OpenGlassBorderPolicyCoordinator.ShouldShowBorderColorLimitNotice()
            ? Visibility.Visible
            : Visibility.Collapsed;
        DwmSystemBackdropNotice.Visibility = SwcaDwmBackdropNoticeCoordinator.ShouldShowRootGradientTintLimitNotice()
            ? Visibility.Visible
            : Visibility.Collapsed;
        if (DwmSystemBackdropNotice.Visibility == Visibility.Visible)
            DwmSystemBackdropNotice.Message = SwcaDwmBackdropNoticeCoordinator.FormatRootGradientTintLimitNoticeMessage();
        UpdatePickerDefaultStates();
    }

    private void ApplyOpenGlassBorderPolicyUi() => ApplyPageTopNoticesUi();

    private void NoBorderSwitch_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        WriteDword("NoBorderColor", _noBorderSwitch.IsOn ? 1u : 0u);
    }

    private void OnEffectChanged()
    {
        if (_loading || !EffectRow.IsGateOn)
            return;

        var index = (uint)_effectCombo!.SelectedIndex;
        if (FlyoutEffectRegistry.IsPresent(_regSub))
        {
            if (GlassRegistry.ReadDword(_regSub, FlyoutEffectRegistry.ValueName) == index)
                return;
        }
        else if (FlyoutEffectRegistry.ReadUiIndex(_regSub) == index)
        {
            return;
        }

        WriteDword("EffectType", index);
        _effectGateArmed = false;
        SwcaDwmBackdropNoticeCoordinator.NotifyEffectOrScopeChanged();
        ApplyPageTopNoticesUi();
    }

    private void OnCornerTypeCommitted()
    {
        SettingsSession.Current.WriteGlassDword(
            RootCornerRegSub, "CornerType", (uint)_cornerCombo!.SelectedIndex);
        RootWindowsLiveReload.NotifyCornerTypeChanged();
    }

    private void EffectGate_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading)
            return;

        if (EffectRow.IsGateOn)
        {
            _effectGateArmed = true;
            _effectCombo!.IsEnabled = true;
            _effectCombo.SelectedIndex = (int)ReadEffectTypeComboIndexForUi();
            return;
        }

        _effectGateArmed = false;
        FlyoutEffectRegistry.Delete(_regSub);
        SettingsSession.Current.MarkDirty();
        Reload();
        RootWindowsLiveReload.NotifyEffectTypeChanged(_regSub);
    }

    private void GradientGate_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        if (GradientExpander.IsGateOn)
        {
            UpdatePickerDefaultStates();
            return;
        }

        FlyoutThemeRegistry.DeleteGradient(_regSub);
        SettingsSession.Current.MarkDirty();
        Reload();
        RootWindowsLiveReload.NotifyGradientColorChanged(_regSub);
    }

    private void BorderGate_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        if (BorderExpander.IsGateOn)
        {
            ApplyOpenGlassBorderPolicyUi();
            return;
        }

        FlyoutThemeRegistry.DeleteBorder(_regSub);
        SettingsSession.Current.MarkDirty();
        Reload();
        RootWindowsLiveReload.NotifyBorderColorChanged(_regSub);
    }

    private void CornerGate_OnToggled(object sender, RoutedEventArgs e)
    {
        if (!_cornersSupported || _loading) return;
        if (CornerRow.IsGateOn)
        {
            _cornerCombo!.IsEnabled = true;
            return;
        }

        GlassRegistry.DeleteValue(RootCornerRegSub, "CornerType");
        SettingsSession.Current.MarkDirty();
        Reload();
        RootWindowsLiveReload.NotifyCornerTypeChanged();
    }

    private void ThemeAlphaGate_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _themeAlphaSlider!.IsEnabled = DrawThemeRow.IsGateOn;
        if (DrawThemeRow.IsGateOn)
            return;

        SettingsSession.Current.DeleteGlassValue(_regSub, "GlassDrawThemeConstantAlpha");
        SettingsSession.Current.MarkDirty();
        Reload();
        NativeHostInterop.InvalidateThemedChromeOnTrackedRootsIfEngineLoaded();
    }

    private void WriteDword(string name, uint v)
    {
        SettingsSession.Current.WriteGlassDword(_regSub, name, v);
        if (name == "EffectType")
            RootWindowsLiveReload.NotifyEffectTypeChanged(_regSub);
        else if (name == "NoBorderColor")
            RootWindowsLiveReload.NotifyBorderColorChanged(_regSub);
    }

    private void OnRootGradientColorCommitted(uint argb)
    {
        if (_loading)
            return;

        LiveReloadScheduler.CancelRootPending();
        SettingsSession.Current.WriteGlassDword(_regSub, ThemeRegistry.GradientColorName(), argb);
        RootWindowsLiveReload.NotifyGradientColorChanged(_regSub);
        UpdatePickerDefaultStates();
    }

    private void OnRootBorderColorCommitted(uint argb)
    {
        if (_loading)
            return;

        LiveReloadScheduler.CancelRootPending();
        SettingsSession.Current.WriteGlassDword(_regSub, ThemeRegistry.BorderColorName(), argb);
        RootWindowsLiveReload.NotifyBorderColorChanged(_regSub);
        UpdatePickerDefaultStates();
    }

    // ─── «Дополнительно»: фон иконок Ribbon / ListView / TreeView ───────────

    private void ConfigureControlIconSection()
    {
        RootAdvancedExpander.HasUnsavedOverride = HasAnyControlIconUnsaved;

        ControlIconExpander.IconGlyph = SettingsSectionGlyphs.Image;
        ControlIconExpander.WireExpandedLayoutRefresh(() => ControlIconPanel.RefreshLayout());

        var labelStyle = Application.Current.Resources["GlassSettingsSpoilerSecondaryTextStyle"] as Style;
        _controlIconThresholdLabel.Style = labelStyle;
        var table = SettingsPickerBodyLayout.CreateBodyTable();
        SettingsPickerBodyLayout.AddLabeledRow(table, _controlIconThresholdLabel, _controlIconThresholdBox);
        ControlIconPanel.FooterContent = table;

        ControlIconPicker.ColorChanged += (_, argb) =>
        {
            if (_loading || !ControlIconExpander.IsGateOn)
                return;

            SettingsNumberBoxCommit.Flush(_controlIconThresholdBox);
            RootControlIconRegistry.WriteIconBackground(
                _controlIconKind,
                argb,
                (uint)_controlIconThresholdBox.Value);
            SettingsNumberBoxCommit.SyncCommittedValue(
                _controlIconThresholdBox,
                _controlIconThresholdBox.Value);
            AfterControlIconMutation();
        };
        SettingsInteractionDeferral.WhenNumberBoxCommitOnEnter(_controlIconThresholdBox, ControlIconPanel);
        SettingsNumberBoxCommit.Attach(
            _controlIconThresholdBox,
            () => !_loading && RootControlIconRegistry.IsIconBackgroundPresent(_controlIconKind),
            ControlIconThresholdBox_OnCommit);
    }

    private void BuildControlIconPivotHeaders()
    {
        if (ControlIconPivot.Items.Count < ControlIconPivotHeaderKeys.Length)
            return;

        for (var i = 0; i < ControlIconPivotHeaderKeys.Length; i++)
        {
            var header = SettingsPivotTabHeader.Create(GlassSettingsResources.Get(ControlIconPivotHeaderKeys[i]));
            _controlIconTabHeaders[i] = header;
            ((PivotItem)ControlIconPivot.Items[i]).Header = header.Root;
        }
    }

    private static bool HasAnyControlIconUnsaved() =>
        SettingsSession.Current.HasUnsavedInScope(SettingsCommitScope.ForRootControlIcons());

    private void RootAdvancedExpander_OnExpanded(object sender, RoutedEventArgs e)
    {
        if (!RootAdvancedExpander.IsExpanded)
            return;

        RootAdvancedExpander.RefreshDividerChrome();
        ReloadControlIconSection();
        DispatcherQueue.TryEnqueue(() =>
        {
            ControlIconPanel.RefreshLayout();
            ControlIconExpander.RefreshHeaderActionsLayout();
        });
    }

    private async void ControlIconPivot_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading)
            return;

        var newIndex = ControlIconPivot.SelectedIndex;
        var previousIndex = _lastControlIconPivotIndex;
        if (previousIndex == newIndex)
            return;

        // Развёрнутый спойлер и включённый пользователем gate сохраняются при перелистывании таба.
        var wasExpanded = ControlIconExpander.IsExpanded;

        await PivotTabContentTransition.AnimateTabChangeAsync(
            ControlIconTabHost,
            previousIndex,
            newIndex,
            () =>
            {
                if (wasExpanded)
                    _controlIconGateArmed = true;
                ReloadControlIconSection();
                if (wasExpanded)
                    ControlIconExpander.IsExpanded = true;
            }).ConfigureAwait(true);

        _lastControlIconPivotIndex = newIndex;
    }

    private void ReloadControlIconSection()
    {
        var prevLoading = _loading;
        _loading = true;
        try
        {
            _controlIconKind = RootControlIconRegistry.KindFromPivotIndex(ControlIconPivot.SelectedIndex);
            ControlIconExpander.UnsavedScope = SettingsCommitScope.ForRootControlIcon(_controlIconKind);
            ControlIconExpander.Description = GlassSettingsResources.Get(
                ControlIconSubtitleKeys[ClampControlIconIndex(ControlIconPivot.SelectedIndex)]);
            ControlIconPanel.Description = GlassSettingsResources.Get(
                ControlIconDescriptionKeys[ClampControlIconIndex(ControlIconPivot.SelectedIndex)]);

            // Gate «залипает» после включения пользователем (не сбрасывается при перелистывании таба).
            var present = RootControlIconRegistry.IsIconBackgroundPresent(_controlIconKind);
            ControlIconExpander.IsGateOn = present || _controlIconGateArmed;

            ControlIconPicker.SetColor(RootControlIconRegistry.ReadEffectiveIconColor(_controlIconKind));
            _controlIconThresholdBox.Value = RootControlIconRegistry.ReadEffectiveIconThreshold(_controlIconKind);
            SettingsNumberBoxCommit.SyncCommittedValue(
                _controlIconThresholdBox,
                _controlIconThresholdBox.Value);

            ControlIconPanel.IsEnabled = ControlIconExpander.IsGateOn;
            UpdateControlIconDefaultButtonState();
            ControlIconExpander.RefreshUnsavedIndicator();
            RefreshControlIconPivotUnsavedIndicators();
            RootAdvancedExpander.RefreshUnsavedIndicator();
        }
        finally
        {
            _loading = prevLoading;
        }
    }

    private void UpdateControlIconDefaultButtonState() =>
        ControlIconPanel.SetDefaultColorActionState(
            ControlIconExpander.IsGateOn,
            !RootControlIconRegistry.IsIconBackgroundPresent(_controlIconKind));

    private void RefreshControlIconPivotUnsavedIndicators()
    {
        for (var i = 0; i < _controlIconTabHeaders.Length; i++)
        {
            if (_controlIconTabHeaders[i] is null)
                continue;

            var kind = RootControlIconRegistry.KindFromPivotIndex(i);
            _controlIconTabHeaders[i].SetUnsavedVisible(
                SettingsSession.Current.HasUnsavedInScope(SettingsCommitScope.ForRootControlIcon(kind)));
        }
    }

    private void AfterControlIconMutation()
    {
        SettingsSession.Current.MarkDirty();
        UpdateControlIconDefaultButtonState();
        RefreshControlIconPivotUnsavedIndicators();
        ControlIconExpander.RefreshUnsavedIndicator();
        RootAdvancedExpander.RefreshUnsavedIndicator();
        RootWindowsLiveReload.NotifyControlIconChanged();
    }

    private void ControlIconExpander_OnGateToggled(object sender, RoutedEventArgs e)
    {
        if (_loading)
            return;

        if (ControlIconExpander.IsGateOn)
        {
            // Ключ появится при первом выборе цвета — до этого в реестр не пишем.
            _controlIconGateArmed = true;
            ControlIconPanel.IsEnabled = true;
            UpdateControlIconDefaultButtonState();
            return;
        }

        _controlIconGateArmed = false;
        RootControlIconRegistry.DeleteIconBackground(_controlIconKind);
        AfterControlIconMutation();
        ReloadControlIconSection();
    }

    private void ControlIconThresholdBox_OnCommit(double value)
    {
        RootControlIconRegistry.WriteIconBackground(
            _controlIconKind,
            RootControlIconRegistry.ReadEffectiveIconColor(_controlIconKind),
            (uint)value);
        AfterControlIconMutation();
    }

    private void ControlIconPanel_OnDefaultClick(object sender, RoutedEventArgs e)
    {
        if (_loading || !ControlIconExpander.IsGateOn)
            return;

        RootControlIconRegistry.DeleteIconBackground(_controlIconKind);
        // Гейт остаётся включённым: пользователь может сразу выбрать новый цвет.
        _controlIconGateArmed = true;
        AfterControlIconMutation();
        ReloadControlIconSection();
    }
}
