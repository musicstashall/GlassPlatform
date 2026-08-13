using GlassSettings.Controls;
using GlassSettings.Core.Customization;
using GlassSettings.Core.Flyout;
using GlassSettings.Core.Menu;
using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Snapshot;
using GlassSettings.Core.Tooltip;
using GlassSettings.Navigation;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Navigation;

namespace GlassSettings.Pages.Glass;

public sealed partial class PopupHubPage : Page
{
    private readonly ComboBox _popInStyleCombo = new() { MinWidth = 140 };
    private readonly ComboBox _cornerCombo = new() { MinWidth = 160 };
    private readonly ComboBox _effectCombo = new() { MinWidth = 200 };
    private readonly NumberBox _fadeOutBox;
    private readonly NumberBox _popInBox;
    private readonly NumberBox _fadeInBox;
    private readonly NumberBox _startRatioBox;
    private readonly ToggleSwitch _shadowSwitch = new()
    {
        OnContent = string.Empty,
        OffContent = string.Empty,
        MinWidth = 0,
        HorizontalAlignment = HorizontalAlignment.Left,
        VerticalAlignment = VerticalAlignment.Center,
    };
    private readonly ToggleSwitch _immediateSwitch = new()
    {
        OnContent = string.Empty,
        OffContent = string.Empty,
        MinWidth = 0,
        HorizontalAlignment = HorizontalAlignment.Left,
        VerticalAlignment = VerticalAlignment.Center,
    };
    private readonly TextBlock _noBorderLabel = new();
    private readonly ToggleSwitch _noBorderSwitch = new()
    {
        OnContent = string.Empty,
        OffContent = string.Empty,
        MinWidth = 0,
        HorizontalAlignment = HorizontalAlignment.Left,
        VerticalAlignment = VerticalAlignment.Center,
    };
    private readonly HubInlineLabel _shadowInlineLabel = new();
    private readonly HubInlineLabel _cornerInlineLabel = new();
    private readonly TextBlock _fadeOutLabel = new() { VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock _popInLabel = new() { VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock _fadeInLabel = new() { VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock _startRatioLabel = new() { VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock _immediateLabel = new() { VerticalAlignment = VerticalAlignment.Center };

    private readonly SettingsPivotTabHeader[] _menuItemTabHeaders = new SettingsPivotTabHeader[4];
    private readonly ComboBox _menuItemEnabledCombo = new() { MinWidth = 140 };
    private readonly NumberBox _menuItemCornerBox;
    private readonly NumberBox _menuItemWidthBox;
    private readonly TextBlock _menuItemCornerLabel = new() { VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock _menuItemWidthLabel = new() { VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock _menuItemThemeColorLabel = new() { VerticalAlignment = VerticalAlignment.Center };
    private readonly ToggleSwitch _menuItemThemeColorToggle = new()
    {
        OnContent = string.Empty,
        OffContent = string.Empty,
        MinWidth = 0,
        HorizontalAlignment = HorizontalAlignment.Left,
        VerticalAlignment = VerticalAlignment.Center,
    };
    private readonly ToggleSwitch _menuNoSystemDropShadowToggle = new()
    {
        OnContent = string.Empty,
        OffContent = string.Empty,
        MinWidth = 0,
        HorizontalAlignment = HorizontalAlignment.Left,
        VerticalAlignment = VerticalAlignment.Center,
    };
    private readonly ToggleSwitch _menuImmersiveStyleToggle = new()
    {
        OnContent = string.Empty,
        OffContent = string.Empty,
        MinWidth = 0,
        HorizontalAlignment = HorizontalAlignment.Left,
        VerticalAlignment = VerticalAlignment.Center,
    };
    private readonly TextBlock _menuImmersiveStyleLabel = new() { VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock _menuNoSystemDropShadowLabel = new() { VerticalAlignment = VerticalAlignment.Center };
    private readonly ToggleSwitch _menuNoModernAppBackgroundToggle = new()
    {
        OnContent = string.Empty,
        OffContent = string.Empty,
        MinWidth = 0,
        HorizontalAlignment = HorizontalAlignment.Left,
        VerticalAlignment = VerticalAlignment.Center,
    };
    private readonly TextBlock _menuNoModernAppBackgroundLabel = new() { VerticalAlignment = VerticalAlignment.Center };
    private readonly NumberBox _menuIconThresholdBox;
    private readonly TextBlock _menuIconThresholdLabel = new() { VerticalAlignment = VerticalAlignment.Center };
    private readonly ComboBox _menuItemThemeColorTypeCombo = new() { MinWidth = 220 };
    private readonly TextBlock _menuItemThemeColorTypeLabel = new() { VerticalAlignment = VerticalAlignment.Center };

    private readonly ComboBox _tooltipMarginsTypeCombo = new() { MinWidth = 220 };
    private readonly NumberBox _tooltipMarginLeftBox;
    private readonly NumberBox _tooltipMarginRightBox;
    private readonly NumberBox _tooltipMarginTopBox;
    private readonly NumberBox _tooltipMarginBottomBox;
    private readonly TextBlock _tooltipMarginsTypeLabel = new() { VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock _tooltipMarginLeftLabel = new() { VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock _tooltipMarginRightLabel = new() { VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock _tooltipMarginTopLabel = new() { VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock _tooltipMarginBottomLabel = new() { VerticalAlignment = VerticalAlignment.Center };

    private static readonly string[] MenuItemPivotHeaderKeys =
    [
        "Glass_MenuItem_Hot",
        "Glass_MenuItem_DisabledHot",
        "Glass_MenuItem_Focusing",
        "Glass_MenuItem_Separator",
    ];

    private static readonly string[] MenuItemActionKeys =
    [
        "Glass_MenuItem_HotAction",
        "Glass_MenuItem_DisabledHotAction",
        "Glass_MenuItem_FocusingAction",
        "Glass_MenuItem_SeparatorAction",
    ];

    private static readonly string[] MenuItemDescriptionKeys =
    [
        "Glass_MenuItem_HotDesc",
        "Glass_MenuItem_DisabledHotDesc",
        "Glass_MenuItem_FocusingDesc",
        "Glass_MenuItem_SeparatorDesc",
    ];

    private MenuItemKind _menuItemKind = MenuItemKind.Hot;
    private bool _menuItemGateEngaged;
    private bool _tooltipRenderingGateEngaged;
    private int _lastMenuItemPivotIndex;
    private bool _lastIsDarkMode;
    private EventHandler<DesktopEnvironmentChangedEventArgs>? _environmentChangedHandler;

    private FlyoutKind _kind = FlyoutKind.Menu;
    private string _regSub = string.Empty;
    private bool _isSeparateScope;
    private bool _loading;
    private bool _effectGateArmed;
    private int _lastPivotIndex;
    private readonly bool _cornersSupported = WindowsOsHelper.IsWindows11OrGreater();

    public PopupHubPage()
    {
        _fadeOutBox = CreateAnimNumberBox(5000);
        _popInBox = CreateAnimNumberBox(5000);
        _fadeInBox = CreateAnimNumberBox(5000);
        _startRatioBox = CreateAnimNumberBox(100);
        _menuItemCornerBox = CreateMenuItemNumberBox(0, 10, 1);
        _menuItemWidthBox = CreateMenuItemNumberBox(0, 10000, 1000);
        _menuIconThresholdBox = CreateMenuItemNumberBox(0, 510, 1);
        _tooltipMarginLeftBox = CreateMenuItemNumberBox(0, 1000, 1);
        _tooltipMarginRightBox = CreateMenuItemNumberBox(0, 1000, 1);
        _tooltipMarginTopBox = CreateMenuItemNumberBox(0, 1000, 1);
        _tooltipMarginBottomBox = CreateMenuItemNumberBox(0, 1000, 1);

        InitializeComponent();
        ConfigureEffectRow();
        BuildNoBorderFooter();
        ConfigureDetailControls();
        ConfigureMenuAdvancedExtras();
        ConfigureMenuItemSection();
        ConfigureTooltipRenderingSection();
        ApplyLocalizedStrings();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        InitializeScopeFromRegistry();
        ApplyScopeUi();
        ApplyExpanderUnsavedScopes();
        Reload();
        MenuAdvancedExpander.RefreshUnsavedIndicator();
        TooltipAdvancedExpander.RefreshUnsavedIndicator();
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        OpenGlassBorderPolicyCoordinator.Changed += OnOpenGlassBorderPolicyChanged;
        SwcaDwmBackdropNoticeCoordinator.Changed += OnPageTopNoticesChanged;
        SettingsSession.Current.DirtyChanged += Session_OnDirtyChanged;
        InitializeScopeFromRegistry();
        ApplyScopeUi();
        ApplyExpanderUnsavedScopes();
        Reload();
        MenuAdvancedExpander.RefreshUnsavedIndicator();
        TooltipAdvancedExpander.RefreshUnsavedIndicator();
        _lastPivotIndex = PopupPivot.SelectedIndex;
        _lastMenuItemPivotIndex = MenuItemPivot.SelectedIndex;
        _lastIsDarkMode = ThemeRegistry.IsDarkMode();
        HookEnvironmentWatcher();
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        OpenGlassBorderPolicyCoordinator.Changed -= OnOpenGlassBorderPolicyChanged;
        SwcaDwmBackdropNoticeCoordinator.Changed -= OnPageTopNoticesChanged;
        SettingsSession.Current.DirtyChanged -= Session_OnDirtyChanged;
        UnhookEnvironmentWatcher();
    }

    private void HookEnvironmentWatcher()
    {
        if (_environmentChangedHandler is not null)
            return;

        if (DesktopEnvironmentWatcher.Current is not { } watcher)
        {
            DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, HookEnvironmentWatcher);
            return;
        }

        _environmentChangedHandler = (_, e) =>
        {
            if (!IsLoaded || !e.ThemeChanged)
                return;

            DispatcherQueue.TryEnqueue(ReloadForThemeChange);
        };
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

    private void ReloadForThemeChange()
    {
        var isDark = ThemeRegistry.IsDarkMode();
        if (isDark == _lastIsDarkMode)
            return;

        _lastIsDarkMode = isDark;
        if (_loading)
            return;

        var prevLoading = _loading;
        _loading = true;
        try
        {
            GradientPicker.SetColor(_isSeparateScope
                ? FlyoutSeparateScopeUi.ReadGradientArgbForUi(_regSub)
                : FlyoutThemeRegistry.ReadGradientArgbForUi(_regSub));
            BorderPicker.SetColor(_isSeparateScope
                ? FlyoutSeparateScopeUi.ReadBorderArgbForUi(_regSub)
                : FlyoutThemeRegistry.ReadBorderArgbForUi(_regSub, ThemeRegistry.BorderColorName()));
            if (_isSeparateScope && _kind == FlyoutKind.Menu)
            {
                MenuItemPicker.SetColor(MenuItemRegistry.ReadEffectiveColor(_menuItemKind, isDark));
                var themeColorType = MenuItemRegistry.ReadEffectiveThemeColorizationType(_menuItemKind, isDark);
                var typeIndex = IndexOfThemeColorTypeCombo(themeColorType);
                if (typeIndex >= 0)
                    _menuItemThemeColorTypeCombo.SelectedIndex = typeIndex;
            }

            if (_isSeparateScope && _kind == FlyoutKind.ToolTip)
            {
                TooltipRenderingPicker.SetColor(TooltipRenderingRegistry.ReadEffectiveColor(isDark));
            }
        }
        finally
        {
            _loading = prevLoading;
        }

        UpdatePickerDefaultStates();
    }

    private void Session_OnDirtyChanged(object? sender, EventArgs e)
    {
        RefreshMenuItemPivotUnsavedIndicators();
        MenuAdvancedExpander.RefreshUnsavedIndicator();
        TooltipAdvancedExpander.RefreshUnsavedIndicator();
    }

    private void ConfigureEffectRow()
    {
        EffectRow.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.EffectType);
        FlyoutEffectNames.PopulateCombo(_effectCombo);
        SettingsInteractionDeferral.WhenComboCommit(
            _effectCombo,
            () => !_loading && EffectRow.IsGateOn,
            OnEffectChanged);
        EffectRow.HeaderActions = _effectCombo;
    }

    private void ConfigureDetailControls()
    {
        GradientExpander.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.Gradient);
        BorderExpander.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.Border);
        CornerRow.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.Corners);
        AnimationExpander.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.Animation);

        _shadowSwitch.Style =
            Application.Current.Resources["GlassSettingsCompactToggleSwitchStyle"] as Style;
        _shadowSwitch.Toggled += ShadowSwitch_OnToggled;

        RebuildCornerRowHeader();

        PopulateCornerCombo();
        ConfigureAnimationRows();
        if (_cornersSupported)
        {
            SettingsInteractionDeferral.WhenComboCommit(
                _cornerCombo,
                () => !_loading && CornerRow.IsGateOn,
                () => WriteDword("CornerType", (uint)_cornerCombo.SelectedIndex));
        }

        GradientPicker.ColorChanged += (_, a) =>
        {
            if (_loading || !GradientExpander.IsGateOn)
                return;

            WriteColor(ThemeRegistry.GradientColorName(), a);
            UpdatePickerDefaultStates();
        };
        BorderPicker.ColorChanged += (_, a) =>
        {
            if (_loading || !BorderExpander.IsGateOn)
                return;

            WriteColor(ThemeRegistry.BorderColorName(), a);
            UpdatePickerDefaultStates();
        };
        GradientPanel.DefaultColorClick += (_, _) => OnGradientDefaultColor();
        BorderPanel.DefaultColorClick += (_, _) => OnBorderDefaultColor();
        AnimationPanel.DefaultClick += (_, _) => OnAnimationDefault();
        BorderExpander.WireExpandedLayoutRefresh(() => BorderPanel.RefreshLayout());
    }

    private NumberBox CreateMenuItemNumberBox(double minimum, double maximum, double smallChange)
    {
        var box = new NumberBox
        {
            Style = (Style)Application.Current.Resources["GlassSettingsNumberBoxStyle"],
            SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Inline,
            Minimum = minimum,
            Maximum = maximum,
            SmallChange = smallChange,
            LargeChange = smallChange,
        };
        return box;
    }

    private bool IsMenuItemGateActive() =>
        MenuCustomRenderingCoordinator.IsGlobalGateOnFromRegistry() || _menuItemGateEngaged;

    private void RebuildCornerRowHeader()
    {
        _shadowInlineLabel.Label = GlassSettingsResources.Get("Flyout_Shadow");
        _shadowInlineLabel.Control = _shadowSwitch;
        CornerRow.HeaderActions = _shadowInlineLabel;

        if (_cornersSupported)
        {
            _cornerInlineLabel.Label = GlassSettingsResources.Get("ParamLabel_CornerType");
            _cornerInlineLabel.Control = _cornerCombo;
            CornerRow.HeaderSlider = _cornerInlineLabel;
        }
        else
        {
            CornerRow.HeaderSlider = null;
        }

        CornerRow.RefreshHeaderLayout();
    }

    private void ConfigureMenuAdvancedExtras()
    {
        MenuAdvancedExpander.HasUnsavedOverride = HasAnyMenuAdvancedSectionUnsaved;

        MenuGeneralSettingsExpander.IconGlyph = SettingsSectionGlyphs.Menu;
        MenuGeneralSettingsExpander.WireExpandedLayoutRefresh(() => MenuGeneralPanel.RefreshLayout());

        var toggleStyle = Application.Current.Resources["GlassSettingsCompactToggleSwitchStyle"] as Style;
        _menuImmersiveStyleToggle.Style = toggleStyle;
        _menuImmersiveStyleToggle.Toggled += MenuImmersiveStyleToggle_OnToggled;
        _menuNoSystemDropShadowToggle.Style = toggleStyle;
        _menuNoSystemDropShadowToggle.Toggled += MenuNoSystemDropShadowToggle_OnToggled;
        _menuNoModernAppBackgroundToggle.Style = toggleStyle;
        _menuNoModernAppBackgroundToggle.Toggled += MenuNoModernAppBackgroundToggle_OnToggled;

        BuildMenuGeneralPanelFooter();

        MenuIconPicker.ColorChanged += (_, argb) =>
        {
            if (_loading)
                return;

            SettingsNumberBoxCommit.Flush(_menuIconThresholdBox);
            MenuRootRegistry.WriteIconBackground(argb, (uint)_menuIconThresholdBox.Value, liveFlyout: false);
            SettingsNumberBoxCommit.SyncCommittedValue(
                _menuIconThresholdBox,
                _menuIconThresholdBox.Value);
            AfterMenuRootMutation();
        };
        SettingsInteractionDeferral.WhenNumberBoxCommitOnEnter(_menuIconThresholdBox, MenuGeneralPanel);
        SettingsNumberBoxCommit.Attach(
            _menuIconThresholdBox,
            () => !_loading && MenuRootRegistry.IsIconBackgroundPresent(),
            MenuIconThresholdBox_OnCommit);
    }

    private void BuildMenuGeneralPanelFooter()
    {
        var labelStyle = Application.Current.Resources["GlassSettingsSpoilerSecondaryTextStyle"] as Style;
        _menuIconThresholdLabel.Style = labelStyle;
        _menuNoSystemDropShadowLabel.Style = labelStyle;
        _menuImmersiveStyleLabel.Style = labelStyle;
        _menuNoModernAppBackgroundLabel.Style = labelStyle;

        var footer = SettingsPickerBodyLayout.CreateBodyTable();
        SettingsPickerBodyLayout.AddLabeledRow(footer, _menuIconThresholdLabel, _menuIconThresholdBox);
        SettingsPickerBodyLayout.AddLabeledRow(
            footer,
            _menuNoModernAppBackgroundLabel,
            _menuNoModernAppBackgroundToggle,
            topMargin: true);
        SettingsPickerBodyLayout.AddLabeledRow(
            footer,
            _menuNoSystemDropShadowLabel,
            _menuNoSystemDropShadowToggle,
            topMargin: true);
        SettingsPickerBodyLayout.AddLabeledRow(
            footer,
            _menuImmersiveStyleLabel,
            _menuImmersiveStyleToggle,
            topMargin: true);
        MenuGeneralPanel.FooterContent = footer;
    }

    private void ConfigureMenuItemSection()
    {
        MenuItemExpander.IconGlyph = SettingsSectionGlyphs.Gradient;
        MenuItemExpander.HeaderActions = _menuItemEnabledCombo;
        MenuItemExpander.WireExpandedLayoutRefresh(() => MenuItemPanel.RefreshLayout());

        foreach (var item in GlassSettingsResources.Get("Combo_MenuItemEnabledItems").Split(';'))
            _menuItemEnabledCombo.Items.Add(item.Trim());

        SettingsInteractionDeferral.WhenComboCommit(
            _menuItemEnabledCombo,
            () => !_loading && IsMenuItemGateActive(),
            OnMenuItemEnabledComboChanged);

        _menuItemThemeColorToggle.Style =
            Application.Current.Resources["GlassSettingsCompactToggleSwitchStyle"] as Style;
        _menuItemThemeColorToggle.Toggled += MenuItemThemeColorToggle_OnToggled;

        foreach (var item in GlassSettingsResources.Get("Combo_MenuThemeColorTypeItems").Split(';'))
            _menuItemThemeColorTypeCombo.Items.Add(item.Trim());

        SettingsInteractionDeferral.WhenComboCommit(
            _menuItemThemeColorTypeCombo,
            () => !_loading && IsMenuItemGateActive() && _menuItemThemeColorToggle.IsOn,
            OnMenuItemThemeColorTypeChanged);

        BuildMenuItemFooter();

        MenuItemPicker.UseLivePreview = false;
        MenuItemPicker.ColorChanged += (_, argb) =>
        {
            if (_loading || !IsMenuItemGateActive())
                return;

            var name = ThemeRegistry.IsDarkMode()
                ? MenuItemRegistry.DarkModeColorValueName
                : MenuItemRegistry.LightModeColorValueName;
            WriteMenuItemValue(name, argb);
        };

        SettingsInteractionDeferral.WhenNumberBoxCommitOnEnter(_menuItemCornerBox, MenuItemPanel);
        SettingsInteractionDeferral.WhenNumberBoxCommitOnEnter(_menuItemWidthBox, MenuItemPanel);
        SettingsNumberBoxCommit.Attach(
            _menuItemCornerBox,
            () => !_loading && IsMenuItemGateActive(),
            value => WriteMenuItemValue(MenuItemRegistry.CornerRadiusValueName, (uint)value));
        SettingsNumberBoxCommit.Attach(
            _menuItemWidthBox,
            () => !_loading && IsMenuItemGateActive() && MenuItemRegistry.HasWidth(_menuItemKind),
            value => WriteMenuItemValue(MenuItemRegistry.WidthValueName, (uint)value));
    }

    private void ConfigureTooltipRenderingSection()
    {
        TooltipRenderingExpander.IconGlyph = SettingsSectionGlyphs.Gradient;
        TooltipRenderingExpander.WireExpandedLayoutRefresh(() => TooltipRenderingPanel.RefreshLayout());
        TooltipAdvancedExpander.HasUnsavedOverride = HasTooltipRenderingUnsaved;

        foreach (var item in GlassSettingsResources.Get("Combo_TooltipMarginsTypeItems").Split(';'))
            _tooltipMarginsTypeCombo.Items.Add(item.Trim());

        SettingsInteractionDeferral.WhenComboCommit(
            _tooltipMarginsTypeCombo,
            () => !_loading && TooltipRenderingExpander.IsGateOn,
            OnTooltipMarginsTypeChanged);

        BuildTooltipRenderingFooter();

        TooltipRenderingPicker.UseLivePreview = false;
        TooltipRenderingPicker.ColorChanged += (_, argb) =>
        {
            if (_loading || !TooltipRenderingExpander.IsGateOn)
                return;

            var name = ThemeRegistry.IsDarkMode()
                ? TooltipRenderingRegistry.DarkModeColorValueName
                : TooltipRenderingRegistry.LightModeColorValueName;
            WriteTooltipRenderingValue(name, argb);
        };

        SettingsInteractionDeferral.WhenNumberBoxCommitOnEnter(_tooltipMarginLeftBox, TooltipRenderingPanel);
        SettingsInteractionDeferral.WhenNumberBoxCommitOnEnter(_tooltipMarginRightBox, TooltipRenderingPanel);
        SettingsInteractionDeferral.WhenNumberBoxCommitOnEnter(_tooltipMarginTopBox, TooltipRenderingPanel);
        SettingsInteractionDeferral.WhenNumberBoxCommitOnEnter(_tooltipMarginBottomBox, TooltipRenderingPanel);
        AttachTooltipMargin(_tooltipMarginLeftBox, TooltipRenderingRegistry.MarginsLeftValueName);
        AttachTooltipMargin(_tooltipMarginRightBox, TooltipRenderingRegistry.MarginsRightValueName);
        AttachTooltipMargin(_tooltipMarginTopBox, TooltipRenderingRegistry.MarginsTopValueName);
        AttachTooltipMargin(_tooltipMarginBottomBox, TooltipRenderingRegistry.MarginsBottomValueName);
    }

    private void AttachTooltipMargin(NumberBox box, string valueName) =>
        SettingsNumberBoxCommit.Attach(
            box,
            () => !_loading && TooltipRenderingExpander.IsGateOn,
            value => WriteTooltipRenderingValue(valueName, (uint)value));

    private void BuildTooltipRenderingFooter()
    {
        var labelStyle = Application.Current.Resources["GlassSettingsSpoilerSecondaryTextStyle"] as Style;
        _tooltipMarginsTypeLabel.Style = labelStyle;
        _tooltipMarginLeftLabel.Style = labelStyle;
        _tooltipMarginRightLabel.Style = labelStyle;
        _tooltipMarginTopLabel.Style = labelStyle;
        _tooltipMarginBottomLabel.Style = labelStyle;

        var table = SettingsPickerBodyLayout.CreateBodyTable();
        SettingsPickerBodyLayout.AddLabeledRow(table, _tooltipMarginsTypeLabel, _tooltipMarginsTypeCombo);
        SettingsPickerBodyLayout.AddLabeledRow(
            table,
            _tooltipMarginLeftLabel,
            _tooltipMarginLeftBox,
            topMargin: true);
        SettingsPickerBodyLayout.AddLabeledRow(
            table,
            _tooltipMarginRightLabel,
            _tooltipMarginRightBox,
            topMargin: true);
        SettingsPickerBodyLayout.AddLabeledRow(
            table,
            _tooltipMarginTopLabel,
            _tooltipMarginTopBox,
            topMargin: true);
        SettingsPickerBodyLayout.AddLabeledRow(
            table,
            _tooltipMarginBottomLabel,
            _tooltipMarginBottomBox,
            topMargin: true);
        TooltipRenderingPanel.FooterContent = table;
    }

    private void BuildMenuItemFooter()
    {
        var labelStyle = Application.Current.Resources["GlassSettingsSpoilerSecondaryTextStyle"] as Style;
        _menuItemCornerLabel.Style = labelStyle;
        _menuItemWidthLabel.Style = labelStyle;
        _menuItemThemeColorLabel.Style = labelStyle;

        var table = SettingsPickerBodyLayout.CreateBodyTable();
        SettingsPickerBodyLayout.AddLabeledRow(table, _menuItemCornerLabel, _menuItemCornerBox);
        SettingsPickerBodyLayout.AddLabeledRow(
            table,
            _menuItemThemeColorLabel,
            _menuItemThemeColorToggle,
            topMargin: true);
        _menuItemThemeColorTypeLabel.Style = labelStyle;
        SettingsPickerBodyLayout.AddLabeledRow(
            table,
            _menuItemThemeColorTypeLabel,
            _menuItemThemeColorTypeCombo,
            topMargin: true);
        SettingsPickerBodyLayout.AddLabeledRow(
            table,
            _menuItemWidthLabel,
            _menuItemWidthBox,
            topMargin: true);
        MenuItemPanel.FooterContent = table;
    }

    private void BuildMenuItemPivotHeaders()
    {
        if (MenuItemPivot.Items.Count < MenuItemPivotHeaderKeys.Length)
            return;

        for (var i = 0; i < MenuItemPivotHeaderKeys.Length; i++)
        {
            var header = SettingsPivotTabHeader.Create(GlassSettingsResources.Get(MenuItemPivotHeaderKeys[i]));
            _menuItemTabHeaders[i] = header;
            ((PivotItem)MenuItemPivot.Items[i]).Header = header.Root;
        }
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
    }

    private NumberBox CreateAnimNumberBox(double maximum)
    {
        return new NumberBox
        {
            Style = (Style)Application.Current.Resources["GlassSettingsNumberBoxStyle"],
            SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Inline,
            Minimum = 0,
            Maximum = maximum,
            SmallChange = 1
        };
    }

    private void ConfigureAnimationRows()
    {
        PopulatePopInStyleCombo();
        SettingsInteractionDeferral.WhenComboCommit(
            _popInStyleCombo,
            () => !_loading && AnimationExpander.IsGateOn,
            OnPopInStyleChanged);
        AnimationExpander.HeaderActions = _popInStyleCombo;
        AnimationExpander.WireExpandedLayoutRefresh(() => AnimationPanel.RefreshLayout());

        _immediateSwitch.Style =
            Application.Current.Resources["GlassSettingsCompactToggleSwitchStyle"] as Style;
        _immediateSwitch.Toggled += ImmediateSwitch_OnToggled;

        var labelStyle = Application.Current.Resources["GlassSettingsSpoilerSecondaryTextStyle"] as Style;
        var rows = new (TextBlock Label, FrameworkElement Control)[]
        {
            (_fadeOutLabel, _fadeOutBox),
            (_popInLabel, _popInBox),
            (_fadeInLabel, _fadeInBox),
            (_startRatioLabel, _startRatioBox),
            (_immediateLabel, _immediateSwitch)
        };

        AnimationPanel.BodyContent = SettingsParameterBodyLayout.BuildLabeledRowsGrid(rows, labelStyle);

        SettingsInteractionDeferral.WhenNumberBoxCommitOnEnter(_fadeOutBox, AnimationPanel);
        SettingsInteractionDeferral.WhenNumberBoxCommitOnEnter(_popInBox, AnimationPanel);
        SettingsInteractionDeferral.WhenNumberBoxCommitOnEnter(_fadeInBox, AnimationPanel);
        SettingsInteractionDeferral.WhenNumberBoxCommitOnEnter(_startRatioBox, AnimationPanel);
        AttachAnimationNumber(_fadeOutBox, "FadeOutTime");
        AttachAnimationNumber(_popInBox, "PopInTime");
        AttachAnimationNumber(_fadeInBox, "FadeInTime");
        AttachAnimationNumber(_startRatioBox, "StartRatio");
    }

    private void AttachAnimationNumber(NumberBox box, string valueName) =>
        SettingsNumberBoxCommit.Attach(
            box,
            () => !_loading && AnimationExpander.IsGateOn,
            value =>
            {
                WriteAnim(valueName, (uint)value);
                UpdateAnimationDefaultState();
            });

    private void ApplyLocalizedStrings()
    {
        ScopeBar.ApplyLocalizedStrings();

        if (PopupPivot.Items.Count >= 3)
        {
            ((PivotItem)PopupPivot.Items[0]).Header = GlassSettingsResources.Get("Glass_PopupMenu");
            ((PivotItem)PopupPivot.Items[1]).Header = GlassSettingsResources.Get("Glass_PopupDropDown");
            ((PivotItem)PopupPivot.Items[2]).Header = GlassSettingsResources.Get("Glass_PopupTooltip");
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

        OpenGlassBorderNotice.Title = GlassSettingsResources.Get("Notice_OpenGlassBorderTitle");
        OpenGlassBorderNotice.Message = OpenGlassBorderPolicyCoordinator.FormatBorderLimitNoticeMessage(includeTaskbarSuffix: false);

        DwmSystemBackdropNotice.Title = GlassSettingsResources.Get("Notice_SwcaDwmBackdropTitle");

        CornerRow.Title = GlassSettingsResources.Get("Flyout_CornersShadows");
        CornerRow.Description = GlassSettingsResources.Get("Flyout_CornersShadowsSubtitle");

        AnimationExpander.Title = GlassSettingsResources.Get("Flyout_Animation");
        AnimationExpander.Description = GlassSettingsResources.Get("Flyout_AnimationSubtitle");
        AnimationPanel.Description = GlassSettingsResources.Get("Flyout_AnimationDesc");
        _noBorderLabel.Text = GlassSettingsResources.Get("Flyout_NoBorder");
        RebuildCornerRowHeader();
        _fadeOutLabel.Text = GlassSettingsResources.Get("ParamLabel_FadeOutTime");
        _popInLabel.Text = GlassSettingsResources.Get("ParamLabel_PopInTime");
        _fadeInLabel.Text = GlassSettingsResources.Get("ParamLabel_FadeInTime");
        _startRatioLabel.Text = GlassSettingsResources.Get("ParamLabel_StartRatio");
        _immediateLabel.Text = GlassSettingsResources.Get("ParamLabel_ImmediateInterrupting");

        var popInStyleIndex = _popInStyleCombo.SelectedIndex;
        PopulatePopInStyleCombo();
        if (popInStyleIndex >= 0)
            _popInStyleCombo.SelectedIndex = popInStyleIndex;

        var cornerIndex = _cornerCombo.SelectedIndex;
        FlyoutCornerNames.PopulateCombo(_cornerCombo);
        if (cornerIndex >= 0)
            _cornerCombo.SelectedIndex = cornerIndex;

        MenuAdvancedExpander.Title = GlassSettingsResources.Get("Dwm_AdvancedColorizationTitle");
        TooltipAdvancedExpander.Title = GlassSettingsResources.Get("Dwm_AdvancedColorizationTitle");
        BuildMenuItemPivotHeaders();
        _menuItemCornerLabel.Text = GlassSettingsResources.Get("Glass_LabelCornerRadius");
        _menuItemWidthLabel.Text = GlassSettingsResources.Get("Glass_LabelMenuItemWidth");
        _menuItemThemeColorLabel.Text = GlassSettingsResources.Get("Glass_LabelEnableThemeColorization");
        _menuItemThemeColorTypeLabel.Text = GlassSettingsResources.Get("Glass_LabelThemeColorizationType");

        MenuGeneralSettingsExpander.Title = GlassSettingsResources.Get("Glass_MenuGeneralSettings_Title");
        MenuGeneralSettingsExpander.Description = GlassSettingsResources.Get("Glass_MenuGeneralSettings_Subtitle");
        MenuGeneralPanel.Description = GlassSettingsResources.Get("Glass_MenuGeneralSettings_Desc");
        _menuImmersiveStyleLabel.Text = GlassSettingsResources.Get("Glass_MenuGeneral_EnableImmersiveStyle_Title");
        _menuNoSystemDropShadowLabel.Text = GlassSettingsResources.Get("Glass_MenuGeneral_NoSystemDropShadow_Title");
        _menuNoModernAppBackgroundLabel.Text = GlassSettingsResources.Get("Glass_MenuGeneral_NoModernAppBackground_Title");
        _menuIconThresholdLabel.Text = GlassSettingsResources.Get("Glass_MenuIcon_Threshold_Label");

        var themeTypeIndex = _menuItemThemeColorTypeCombo.SelectedIndex;
        _menuItemThemeColorTypeCombo.Items.Clear();
        foreach (var item in GlassSettingsResources.Get("Combo_MenuThemeColorTypeItems").Split(';'))
            _menuItemThemeColorTypeCombo.Items.Add(item.Trim());
        if (themeTypeIndex >= 0 && themeTypeIndex < _menuItemThemeColorTypeCombo.Items.Count)
            _menuItemThemeColorTypeCombo.SelectedIndex = themeTypeIndex;

        TooltipRenderingExpander.Title = GlassSettingsResources.Get("Glass_TooltipRendering_Title");
        TooltipRenderingExpander.Description = GlassSettingsResources.Get("Glass_TooltipRendering_Desc");
        TooltipRenderingPanel.Description = GlassSettingsResources.Get("Glass_TooltipRendering_Action");
        _tooltipMarginsTypeLabel.Text = GlassSettingsResources.Get("Glass_TooltipRendering_MarginsType");
        _tooltipMarginLeftLabel.Text = GlassSettingsResources.Get("Glass_TooltipRendering_MarginLeft");
        _tooltipMarginRightLabel.Text = GlassSettingsResources.Get("Glass_TooltipRendering_MarginRight");
        _tooltipMarginTopLabel.Text = GlassSettingsResources.Get("Glass_TooltipRendering_MarginTop");
        _tooltipMarginBottomLabel.Text = GlassSettingsResources.Get("Glass_TooltipRendering_MarginBottom");

        var tooltipMarginsIndex = _tooltipMarginsTypeCombo.SelectedIndex;
        _tooltipMarginsTypeCombo.Items.Clear();
        foreach (var item in GlassSettingsResources.Get("Combo_TooltipMarginsTypeItems").Split(';'))
            _tooltipMarginsTypeCombo.Items.Add(item.Trim());
        if (tooltipMarginsIndex >= 0)
            _tooltipMarginsTypeCombo.SelectedIndex = tooltipMarginsIndex;

        ApplyMenuItemTabLabels();
    }

    private void ApplyMenuItemTabLabels()
    {
        for (var i = 0; i < _menuItemTabHeaders.Length; i++)
        {
            if (_menuItemTabHeaders[i] is null)
                continue;
            _menuItemTabHeaders[i].SetLabel(GlassSettingsResources.Get(MenuItemPivotHeaderKeys[i]));
        }
    }

    private static string MenuItemDescriptionKey(MenuItemKind kind) => kind switch
    {
        MenuItemKind.DisabledHot => MenuItemDescriptionKeys[1],
        MenuItemKind.Focusing => MenuItemDescriptionKeys[2],
        MenuItemKind.Separator => MenuItemDescriptionKeys[3],
        _ => MenuItemDescriptionKeys[0],
    };

    private static string MenuItemActionKey(MenuItemKind kind) => kind switch
    {
        MenuItemKind.DisabledHot => MenuItemActionKeys[1],
        MenuItemKind.Focusing => MenuItemActionKeys[2],
        MenuItemKind.Separator => MenuItemActionKeys[3],
        _ => MenuItemActionKeys[0],
    };

    private static string MenuItemSectionTitleKey(MenuItemKind kind) => kind switch
    {
        MenuItemKind.DisabledHot => MenuItemPivotHeaderKeys[1],
        MenuItemKind.Focusing => MenuItemPivotHeaderKeys[2],
        MenuItemKind.Separator => MenuItemPivotHeaderKeys[3],
        _ => MenuItemPivotHeaderKeys[0],
    };

    private void ApplyEffectRowDescription()
    {
        EffectRow.Description = _isSeparateScope
            ? FlyoutEffectScopeDescriptions.ForKind(_kind)
            : GlassSettingsResources.Get("Flyout_EffectParams");
    }

    private void PopulateCornerCombo() => FlyoutCornerNames.PopulateCombo(_cornerCombo);

    private void PopulatePopInStyleCombo() =>
        FlyoutAnimNames.PopulatePopInStyleCombo(_popInStyleCombo);

    private static bool DetectSeparateScope() =>
        CustomizationCatalog.IsFlyoutSectionActive("Menu") ||
        CustomizationCatalog.IsFlyoutSectionActive("DropDown") ||
        CustomizationCatalog.IsFlyoutSectionActive("ToolTip");

    private void InitializeScopeFromRegistry()
    {
        _loading = true;
        _isSeparateScope = DetectSeparateScope();
        ScopeBar.IsSeparateScope = _isSeparateScope;
        _kind = KindFromPivotIndex(PopupPivot.SelectedIndex);
        UpdateRegSubFromScope();
        _loading = false;
    }

    private async void ScopeBar_OnScopeChanged(object sender, RoutedEventArgs e)
    {
        if (_loading)
            return;

        var newSeparate = ScopeBar.IsSeparateScope;
        if (newSeparate == _isSeparateScope)
            return;

        await PivotTabContentTransition.AnimateVerticalChangeAsync(
            ScopeContentHost,
            toSeparate: newSeparate,
            () =>
            {
                _isSeparateScope = newSeparate;
                UpdateRegSubFromScope();
                ApplyScopeUi();
                ApplyExpanderUnsavedScopes();
                Reload();
            }).ConfigureAwait(true);
    }

    private void ApplyScopeUi()
    {
        PopupPivot.Visibility = _isSeparateScope ? Visibility.Visible : Visibility.Collapsed;
        ApplyEffectRowDescription();
        ApplyAnimationVisibility();
        ApplyAdvancedSectionsVisibility();
        RefreshGeneralFallbackWarnings();
    }

    private void RefreshGeneralFallbackWarnings()
    {
        if (!_isSeparateScope)
        {
            ClearGeneralFallbackWarnings();
            return;
        }

        var marker = GlassSettingsResources.Get("Popup_GeneralFallbackMarker");
        var tooltip = GlassSettingsResources.Get("Popup_GeneralFallbackTooltip");

        ApplyGeneralFallbackWarning(
            EffectRow,
            FlyoutSeparateScopeUi.EffectUsesGeneral(_regSub),
            marker,
            tooltip);
        ApplyGeneralFallbackWarning(
            GradientExpander,
            FlyoutSeparateScopeUi.GradientUsesGeneral(_regSub),
            marker,
            tooltip);
        ApplyGeneralFallbackWarning(
            BorderExpander,
            FlyoutSeparateScopeUi.BorderUsesGeneral(_regSub),
            marker,
            tooltip);
        ApplyGeneralFallbackWarning(
            CornerRow,
            FlyoutSeparateScopeUi.CornersUseGeneral(_regSub),
            marker,
            tooltip);
        ApplyGeneralFallbackWarning(
            AnimationExpander,
            FlyoutSeparateScopeUi.AnimationUsesGeneral(_regSub),
            marker,
            tooltip);
    }

    private static void ApplyGeneralFallbackWarning(
        SettingsFlatGateRow row,
        bool visible,
        string marker,
        string tooltip) =>
        row.SetParallelBranchWarning(visible, tooltip, marker, highlightBackground: visible);

    private static void ApplyGeneralFallbackWarning(
        SettingsSystemGateExpander expander,
        bool visible,
        string marker,
        string tooltip) =>
        expander.SetParallelBranchWarning(visible, tooltip, marker, highlightBackground: visible);

    private void ClearGeneralFallbackWarnings()
    {
        EffectRow.SetParallelBranchWarning(false, null);
        GradientExpander.SetParallelBranchWarning(false, null);
        BorderExpander.SetParallelBranchWarning(false, null);
        CornerRow.SetParallelBranchWarning(false, null);
        AnimationExpander.SetParallelBranchWarning(false, null);
    }

    private void ApplyAdvancedSectionsVisibility()
    {
        var menuOnly = _isSeparateScope && _kind == FlyoutKind.Menu;
        MenuAdvancedExpander.Visibility = menuOnly ? Visibility.Visible : Visibility.Collapsed;
        TooltipAdvancedExpander.Visibility = _isSeparateScope && _kind == FlyoutKind.ToolTip
            ? Visibility.Visible
            : Visibility.Collapsed;
        RebuildCornerRowHeader();
    }

    private void ApplyMenuAdvancedVisibility() => ApplyAdvancedSectionsVisibility();

    private void UpdateRegSubFromScope()
    {
        if (_isSeparateScope)
        {
            _kind = KindFromPivotIndex(PopupPivot.SelectedIndex);
            _regSub = _kind.RegSub();
            return;
        }

        _regSub = string.Empty;
    }

    private static FlyoutKind KindFromPivotIndex(int index) => index switch
    {
        1 => FlyoutKind.DropDown,
        2 => FlyoutKind.ToolTip,
        _ => FlyoutKind.Menu
    };

    private async void PopupPivot_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || !_isSeparateScope)
            return;

        var newIndex = PopupPivot.SelectedIndex;
        var previousIndex = _lastPivotIndex;
        if (previousIndex == newIndex)
            return;

        await PivotTabContentTransition.AnimateTabChangeAsync(
            TabContentHost,
            previousIndex,
            newIndex,
            () =>
            {
                _kind = KindFromPivotIndex(newIndex);
                _regSub = _kind.RegSub();
                ApplyEffectRowDescription();
                ApplyAnimationVisibility();
                ApplyAdvancedSectionsVisibility();
                ApplyExpanderUnsavedScopes();
                Reload();
            }).ConfigureAwait(true);

        _lastPivotIndex = newIndex;
    }

    private async void MenuItemPivot_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || !_isSeparateScope || _kind != FlyoutKind.Menu)
            return;

        var newIndex = MenuItemPivot.SelectedIndex;
        var previousIndex = _lastMenuItemPivotIndex;
        if (previousIndex == newIndex)
            return;

        var preserveExpanded = MenuItemExpander.IsExpanded && MenuItemExpander.IsGateOn;

        await PivotTabContentTransition.AnimateTabChangeAsync(
            MenuItemTabHost,
            previousIndex,
            newIndex,
            () =>
            {
                _menuItemKind = MenuItemRegistry.KindFromPivotIndex(newIndex);
                ReloadMenuItemSection();
                if (preserveExpanded && MenuItemExpander.IsGateOn)
                    MenuItemExpander.IsExpanded = true;
            }).ConfigureAwait(true);

        _lastMenuItemPivotIndex = newIndex;
    }

    private void TooltipAdvancedExpander_OnExpanded(object sender, RoutedEventArgs e)
    {
        if (!TooltipAdvancedExpander.IsExpanded)
            return;

        TooltipAdvancedExpander.RefreshDividerChrome();
        DispatcherQueue.TryEnqueue(() =>
        {
            TooltipRenderingPanel.RefreshLayout();
            TooltipRenderingExpander.RefreshHeaderActionsLayout();
        });
    }

    private void MenuAdvancedExpander_OnExpanded(object sender, RoutedEventArgs e)
    {
        if (!MenuAdvancedExpander.IsExpanded)
            return;

        MenuAdvancedExpander.RefreshDividerChrome();
        DispatcherQueue.TryEnqueue(() =>
        {
            MenuGeneralPanel.RefreshLayout();
            MenuItemPanel.RefreshLayout();
            MenuItemExpander.RefreshHeaderActionsLayout();
        });
    }

    private void Reload()
    {
        _loading = true;
        ApplyExpanderUnsavedScopes();
        ApplyEffectGateUiFromRegistry();

        if (_isSeparateScope)
            ReloadSeparateScopeMainSections();
        else
            ReloadSharedScopeMainSections();

        ReloadMenuGeneralSection();
        ReloadMenuItemSection();
        ReloadTooltipRenderingSection();

        _loading = false;
        RefreshDwmBackdropNoticeUi();
        UpdatePickerDefaultStates();
        UpdateAnimationDefaultState();
        RefreshGeneralFallbackWarnings();
    }

    private void ReloadSharedScopeMainSections()
    {
        _cornerCombo.SelectedIndex = _cornersSupported
            ? (int)Math.Clamp(FlyoutPartRegistry.ReadCornerTypeForUi(_regSub), 0u, 3u)
            : 0;
        _shadowSwitch.IsOn = FlyoutPartRegistry.ReadEffectiveDword(_regSub, "EnableDropShadow", 0) != 0;
        CornerRow.IsGateOn = FlyoutCornerRegistry.IsAnyPresent(_regSub);
        if (_cornersSupported)
            _cornerCombo.IsEnabled = CornerRow.IsGateOn;
        _shadowSwitch.IsEnabled = CornerRow.IsGateOn;

        GradientExpander.IsGateOn = FlyoutThemeRegistry.IsGradientPresent(_regSub);
        GradientPicker.SetColor(FlyoutThemeRegistry.ReadGradientArgbForUi(_regSub));
        BorderExpander.IsGateOn = FlyoutThemeRegistry.IsBorderPresent(_regSub);
        var border = FlyoutThemeRegistry.ReadBorderArgbForUi(_regSub, ThemeRegistry.BorderColorName());
        _noBorderSwitch.IsOn = FlyoutPartRegistry.ReadNoBorderColorForUi(_regSub);
        BorderPicker.SetColor(border);

        AnimationExpander.IsGateOn = FlyoutAnimRegistry.IsAnyPresent(_regSub);
        _popInStyleCombo.SelectedIndex = (int)Math.Clamp(
            FlyoutAnimRegistry.ReadAnimForUi(_regSub, "PopInStyle"), 0u, 3u);
        _fadeOutBox.Value = FlyoutAnimRegistry.ReadAnimForUi(_regSub, "FadeOutTime");
        _popInBox.Value = FlyoutAnimRegistry.ReadAnimForUi(_regSub, "PopInTime");
        _fadeInBox.Value = FlyoutAnimRegistry.ReadAnimForUi(_regSub, "FadeInTime");
        _startRatioBox.Value = FlyoutAnimRegistry.ReadAnimForUi(_regSub, "StartRatio");
        SettingsNumberBoxCommit.SyncCommittedValue(_fadeOutBox, _fadeOutBox.Value);
        SettingsNumberBoxCommit.SyncCommittedValue(_popInBox, _popInBox.Value);
        SettingsNumberBoxCommit.SyncCommittedValue(_fadeInBox, _fadeInBox.Value);
        SettingsNumberBoxCommit.SyncCommittedValue(_startRatioBox, _startRatioBox.Value);
        _immediateSwitch.IsOn = FlyoutAnimRegistry.ReadAnimForUi(_regSub, "ImmediateInterrupting") != 0;
        SetAnimationControlsEnabled(AnimationExpander.IsGateOn);
    }

    private void ReloadSeparateScopeMainSections()
    {
        _cornerCombo.SelectedIndex = _cornersSupported
            ? (int)Math.Clamp(FlyoutSeparateScopeUi.ReadCornerTypeForUi(_regSub), 0u, 3u)
            : 0;
        _shadowSwitch.IsOn = FlyoutSeparateScopeUi.ReadDropShadowForUi(_regSub);
        CornerRow.IsGateOn = FlyoutCornerRegistry.IsAnyPresent(_regSub);
        if (_cornersSupported)
            _cornerCombo.IsEnabled = CornerRow.IsGateOn;
        _shadowSwitch.IsEnabled = CornerRow.IsGateOn;

        GradientExpander.IsGateOn = FlyoutThemeRegistry.IsGradientPresent(_regSub);
        GradientPicker.SetColor(FlyoutSeparateScopeUi.ReadGradientArgbForUi(_regSub));
        BorderExpander.IsGateOn = FlyoutThemeRegistry.IsBorderPresent(_regSub);
        var border = FlyoutSeparateScopeUi.ReadBorderArgbForUi(_regSub);
        _noBorderSwitch.IsOn = FlyoutSeparateScopeUi.ReadNoBorderColorForUi(_regSub);
        BorderPicker.SetColor(border);

        AnimationExpander.IsGateOn = FlyoutAnimRegistry.IsAnyPresent(_regSub);
        _popInStyleCombo.SelectedIndex = (int)Math.Clamp(
            FlyoutSeparateScopeUi.ReadAnimForUi(_regSub, "PopInStyle"), 0u, 3u);
        _fadeOutBox.Value = FlyoutSeparateScopeUi.ReadAnimForUi(_regSub, "FadeOutTime");
        _popInBox.Value = FlyoutSeparateScopeUi.ReadAnimForUi(_regSub, "PopInTime");
        _fadeInBox.Value = FlyoutSeparateScopeUi.ReadAnimForUi(_regSub, "FadeInTime");
        _startRatioBox.Value = FlyoutSeparateScopeUi.ReadAnimForUi(_regSub, "StartRatio");
        SettingsNumberBoxCommit.SyncCommittedValue(_fadeOutBox, _fadeOutBox.Value);
        SettingsNumberBoxCommit.SyncCommittedValue(_popInBox, _popInBox.Value);
        SettingsNumberBoxCommit.SyncCommittedValue(_fadeInBox, _fadeInBox.Value);
        SettingsNumberBoxCommit.SyncCommittedValue(_startRatioBox, _startRatioBox.Value);
        _immediateSwitch.IsOn = FlyoutSeparateScopeUi.ReadAnimForUi(_regSub, "ImmediateInterrupting") != 0;
        SetAnimationControlsEnabled(AnimationExpander.IsGateOn);
    }

    private uint ReadEffectTypeComboIndexForUi()
    {
        if (!_isSeparateScope)
            return FlyoutEffectRegistry.ReadUiIndex(_regSub);

        return FlyoutSeparateScopeUi.ReadEffectComboIndexForUi(_regSub);
    }

    private void ApplyEffectGateUiFromRegistry()
    {
        var hasLocal = _isSeparateScope
            ? FlyoutSeparateScopeUi.IsStrictLocalEffectPresent(_regSub)
            : FlyoutEffectRegistry.IsPresent(_regSub);
        EffectRow.IsGateOn = hasLocal || _effectGateArmed;
        _effectCombo.SelectedIndex = (int)ReadEffectTypeComboIndexForUi();
        _effectCombo.IsEnabled = EffectRow.IsGateOn;
        if (hasLocal)
            _effectGateArmed = false;
    }

    private void RefreshDwmBackdropNoticeUi()
    {
        SwcaDwmBackdropNoticeCoordinator.NotifyEffectOrScopeChanged();
        ApplyPageTopNoticesUi();
    }

    private void ApplyAnimationVisibility()
    {
        AnimationExpander.Visibility = _isSeparateScope && _kind == FlyoutKind.ToolTip
            ? Visibility.Collapsed
            : Visibility.Visible;
    }

    private static bool HasMenuRootUnsaved() =>
        SettingsSession.Current.HasUnsavedInScope(SettingsCommitScope.ForMenuRoot());

    private static bool HasAnyMenuAdvancedSectionUnsaved() =>
        HasMenuRootUnsaved() || HasAnyMenuAdvancedUnsaved();

    private void RefreshMenuRootUnsavedIndicators()
    {
        MenuGeneralSettingsExpander.RefreshUnsavedIndicator();
    }

    private static bool HasAnyMenuAdvancedUnsaved()
    {
        if (SettingsSession.Current.HasUnsavedInScope(SettingsCommitScope.ForMenuCustomRenderingMaster()))
            return true;

        foreach (MenuItemKind kind in Enum.GetValues<MenuItemKind>())
        {
            if (SettingsSession.Current.HasUnsavedInScope(SettingsCommitScope.ForMenuItemKind(kind)))
                return true;
        }

        return false;
    }

    private static bool HasTooltipRenderingUnsaved() =>
        SettingsSession.Current.HasUnsavedInScope(SettingsCommitScope.ForTooltipRendering());

    private static bool HasAnyMenuItemKindUnsaved() => HasAnyMenuAdvancedUnsaved();

    private void ApplyExpanderUnsavedScopes()
    {
        EffectRow.UnsavedScope = SettingsCommitScope.ForFlyoutParameterGroup(_regSub, FlyoutParameterGroup.Effect);
        GradientExpander.UnsavedScope = SettingsCommitScope.ForFlyoutParameterGroup(_regSub, FlyoutParameterGroup.Gradient);
        BorderExpander.UnsavedScope = SettingsCommitScope.ForFlyoutParameterGroup(_regSub, FlyoutParameterGroup.Border);
        if (_cornersSupported)
            CornerRow.UnsavedScope = SettingsCommitScope.ForFlyoutParameterGroup(_regSub, FlyoutParameterGroup.Corners);
        AnimationExpander.UnsavedScope = SettingsCommitScope.ForFlyoutParameterGroup(_regSub, FlyoutParameterGroup.Animation);
        MenuGeneralSettingsExpander.UnsavedScope = SettingsCommitScope.ForMenuRoot();
        RefreshMenuRootUnsavedIndicators();
        MenuItemExpander.UnsavedScope = SettingsCommitScope.ForMenuItemKind(_menuItemKind);
        MenuItemExpander.RefreshUnsavedIndicator();
        MenuAdvancedExpander.RefreshUnsavedIndicator();
        TooltipRenderingExpander.UnsavedScope = SettingsCommitScope.ForTooltipRendering();
        TooltipRenderingExpander.RefreshUnsavedIndicator();
        TooltipAdvancedExpander.HasUnsavedOverride = HasTooltipRenderingUnsaved;
        TooltipAdvancedExpander.RefreshUnsavedIndicator();
    }

    private void ReloadMenuGeneralSection()
    {
        if (!_isSeparateScope || _kind != FlyoutKind.Menu)
            return;

        var prevLoading = _loading;
        _loading = true;
        try
        {
            MenuGeneralSettingsExpander.UnsavedScope = SettingsCommitScope.ForMenuRoot();

            MenuGeneralSettingsExpander.IsGateOn = MenuRootRegistry.IsEnableImmersiveStylePresent();
            _menuImmersiveStyleToggle.IsOn =
                MenuRootRegistry.ReadEffectiveDword(MenuRootRegistry.EnableImmersiveStyleValueName) != 0;
            SetMenuImmersiveStyleControlsEnabled(MenuGeneralSettingsExpander.IsGateOn);

            _menuNoSystemDropShadowToggle.IsOn =
                MenuRootRegistry.ReadEffectiveDword(MenuRootRegistry.NoSystemDropShadowValueName) != 0;

            _menuNoModernAppBackgroundToggle.IsOn =
                MenuRootRegistry.ReadEffectiveDword(MenuRootRegistry.NoModernAppBackgroundColorValueName) != 0;

            MenuIconPicker.SetColor(MenuRootRegistry.ReadEffectiveIconColor());
            _menuIconThresholdBox.Value = MenuRootRegistry.ReadEffectiveIconThreshold();
            SettingsNumberBoxCommit.SyncCommittedValue(
                _menuIconThresholdBox,
                _menuIconThresholdBox.Value);

            UpdateMenuIconDefaultButtonState();
            RefreshMenuRootUnsavedIndicators();
        }
        finally
        {
            _loading = prevLoading;
        }
    }

    private void ReloadMenuItemSection()
    {
        if (!_isSeparateScope || _kind != FlyoutKind.Menu)
            return;

        var prevLoading = _loading;
        _loading = true;
        try
        {
            _menuItemKind = MenuItemRegistry.KindFromPivotIndex(MenuItemPivot.SelectedIndex);
            MenuItemExpander.UnsavedScope = SettingsCommitScope.ForMenuItemKind(_menuItemKind);

            MenuItemExpander.Title = GlassSettingsResources.Get(MenuItemSectionTitleKey(_menuItemKind));
            MenuItemExpander.Description = GlassSettingsResources.Get(MenuItemDescriptionKey(_menuItemKind));
            MenuItemPanel.Description = GlassSettingsResources.Get(MenuItemActionKey(_menuItemKind));

            ApplyMenuItemGateUiFromRegistry();

            var disabled = MenuItemRegistry.ReadEffectiveDword(_menuItemKind, MenuItemRegistry.DisabledValueName);
            _menuItemEnabledCombo.SelectedIndex = disabled != 0 ? 1 : 0;

            MenuItemPicker.SetColor(MenuItemRegistry.ReadEffectiveColor(_menuItemKind, ThemeRegistry.IsDarkMode()));
            _menuItemCornerBox.Value = MenuItemRegistry.ReadEffectiveDword(
                _menuItemKind,
                MenuItemRegistry.CornerRadiusValueName);
            _menuItemWidthBox.Value = MenuItemRegistry.ReadEffectiveDword(
                _menuItemKind,
                MenuItemRegistry.WidthValueName);
            SettingsNumberBoxCommit.SyncCommittedValue(_menuItemCornerBox, _menuItemCornerBox.Value);
            SettingsNumberBoxCommit.SyncCommittedValue(_menuItemWidthBox, _menuItemWidthBox.Value);
            _menuItemThemeColorToggle.IsOn = MenuItemRegistry.ReadEffectiveDword(
                _menuItemKind,
                MenuItemRegistry.EnableThemeColorizationValueName) != 0;

            var themeColorType = MenuItemRegistry.ReadEffectiveThemeColorizationType(
                _menuItemKind,
                ThemeRegistry.IsDarkMode());
            var typeIndex = IndexOfThemeColorTypeCombo(themeColorType);
            if (typeIndex >= 0)
                _menuItemThemeColorTypeCombo.SelectedIndex = typeIndex;

            var showThemeType = _menuItemThemeColorToggle.IsOn;
            _menuItemThemeColorTypeLabel.Visibility = showThemeType ? Visibility.Visible : Visibility.Collapsed;
            _menuItemThemeColorTypeCombo.Visibility = showThemeType ? Visibility.Visible : Visibility.Collapsed;

            var showWidth = MenuItemRegistry.HasWidth(_menuItemKind);
            _menuItemWidthLabel.Visibility = showWidth ? Visibility.Visible : Visibility.Collapsed;
            _menuItemWidthBox.Visibility = showWidth ? Visibility.Visible : Visibility.Collapsed;

            var showCorner = _menuItemKind != MenuItemKind.Separator;
            _menuItemCornerLabel.Visibility = showCorner ? Visibility.Visible : Visibility.Collapsed;
            _menuItemCornerBox.Visibility = showCorner ? Visibility.Visible : Visibility.Collapsed;

            SetMenuItemControlsEnabled(IsMenuItemGateActive());
            UpdateMenuItemDefaultButtonState();
            MenuItemExpander.RefreshUnsavedIndicator();
            RefreshMenuItemPivotUnsavedIndicators();
        }
        finally
        {
            _loading = prevLoading;
        }
    }

    private void ApplyMenuItemGateUiFromRegistry()
    {
        var globalOn = MenuCustomRenderingCoordinator.IsGlobalGateOnFromRegistry();
        if (globalOn)
            _menuItemGateEngaged = false;

        MenuItemExpander.IsGateOn = globalOn || _menuItemGateEngaged;
        _menuItemEnabledCombo.IsEnabled = MenuItemExpander.IsGateOn;
    }

    private void ReloadTooltipRenderingSection()
    {
        if (!_isSeparateScope || _kind != FlyoutKind.ToolTip)
            return;

        var prevLoading = _loading;
        _loading = true;
        try
        {
            ApplyTooltipRenderingGateUiFromRegistry();

            TooltipRenderingPicker.SetColor(
                TooltipRenderingRegistry.ReadEffectiveColor(ThemeRegistry.IsDarkMode()));
            _tooltipMarginsTypeCombo.SelectedIndex = (int)Math.Clamp(
                TooltipRenderingRegistry.ReadEffectiveDword(TooltipRenderingRegistry.MarginsTypeValueName),
                0u,
                1u);
            _tooltipMarginLeftBox.Value = TooltipRenderingRegistry.ReadEffectiveDword(
                TooltipRenderingRegistry.MarginsLeftValueName);
            _tooltipMarginRightBox.Value = TooltipRenderingRegistry.ReadEffectiveDword(
                TooltipRenderingRegistry.MarginsRightValueName);
            _tooltipMarginTopBox.Value = TooltipRenderingRegistry.ReadEffectiveDword(
                TooltipRenderingRegistry.MarginsTopValueName);
            _tooltipMarginBottomBox.Value = TooltipRenderingRegistry.ReadEffectiveDword(
                TooltipRenderingRegistry.MarginsBottomValueName);
            SettingsNumberBoxCommit.SyncCommittedValue(
                _tooltipMarginLeftBox,
                _tooltipMarginLeftBox.Value);
            SettingsNumberBoxCommit.SyncCommittedValue(
                _tooltipMarginRightBox,
                _tooltipMarginRightBox.Value);
            SettingsNumberBoxCommit.SyncCommittedValue(
                _tooltipMarginTopBox,
                _tooltipMarginTopBox.Value);
            SettingsNumberBoxCommit.SyncCommittedValue(
                _tooltipMarginBottomBox,
                _tooltipMarginBottomBox.Value);

            SetTooltipRenderingControlsEnabled(TooltipRenderingExpander.IsGateOn);
            UpdateTooltipRenderingDefaultButtonState();
            TooltipRenderingExpander.RefreshUnsavedIndicator();
            TooltipAdvancedExpander.RefreshUnsavedIndicator();
        }
        finally
        {
            _loading = prevLoading;
        }
    }

    private void ApplyTooltipRenderingGateUiFromRegistry()
    {
        var hasLocal = TooltipRenderingRegistry.IsAnyPresent();
        TooltipRenderingExpander.IsGateOn = hasLocal || _tooltipRenderingGateEngaged;
        if (hasLocal)
            _tooltipRenderingGateEngaged = false;
    }

    private void SetTooltipRenderingControlsEnabled(bool enabled)
    {
        _tooltipMarginsTypeCombo.IsEnabled = enabled;
        _tooltipMarginLeftBox.IsEnabled = enabled;
        _tooltipMarginRightBox.IsEnabled = enabled;
        _tooltipMarginTopBox.IsEnabled = enabled;
        _tooltipMarginBottomBox.IsEnabled = enabled;
        TooltipRenderingPicker.IsEnabled = enabled;
    }

    private void UpdateTooltipRenderingDefaultButtonState()
    {
        if (_loading)
            return;

        TooltipRenderingPanel.SetDefaultColorActionState(
            TooltipRenderingExpander.IsGateOn,
            TooltipRenderingRegistry.ShouldDisableDefaultButton());
    }

    private void AfterTooltipRenderingMutation()
    {
        SettingsSession.Current.MarkDirty();
        UpdateTooltipRenderingDefaultButtonState();
        TooltipRenderingExpander.RefreshUnsavedIndicator();
        TooltipAdvancedExpander.RefreshUnsavedIndicator();
    }

    private void WriteTooltipRenderingValue(string name, uint value)
    {
        if (TooltipRenderingRegistry.DefaultDword(name) == value)
            SettingsSession.Current.DeleteGlassValue(TooltipRenderingRegistry.RegSub, name, liveFlyout: false);
        else
            SettingsSession.Current.WriteGlassDword(TooltipRenderingRegistry.RegSub, name, value, liveFlyout: false);
        AfterTooltipRenderingMutation();
    }

    private void SetMenuItemControlsEnabled(bool enabled)
    {
        _menuItemEnabledCombo.IsEnabled = enabled;
        _menuItemCornerBox.IsEnabled = enabled;
        _menuItemWidthBox.IsEnabled = enabled && MenuItemRegistry.HasWidth(_menuItemKind);
        _menuItemThemeColorToggle.IsEnabled = enabled;
        _menuItemThemeColorTypeCombo.IsEnabled = enabled && _menuItemThemeColorToggle.IsOn;
        MenuItemPicker.IsEnabled = enabled;
    }

    private static int IndexOfThemeColorTypeCombo(string type)
    {
        var items = GlassSettingsResources.Get("Combo_MenuThemeColorTypeItems").Split(';');
        for (var i = 0; i < items.Length; i++)
        {
            if (string.Equals(items[i].Trim(), type, StringComparison.OrdinalIgnoreCase))
                return i;
        }

        return items.Length > 0 ? 0 : -1;
    }

    private static string ThemeColorTypeFromComboIndex(int index)
    {
        var items = GlassSettingsResources.Get("Combo_MenuThemeColorTypeItems").Split(';');
        if (index < 0 || index >= items.Length)
            return MenuItemRegistry.DefaultThemeColorizationType;
        return items[index].Trim();
    }

    private void UpdateMenuItemDefaultButtonState()
    {
        if (_loading)
            return;

        MenuItemPanel.SetDefaultColorActionState(
            IsMenuItemGateActive(),
            MenuItemRegistry.ShouldDisableDefaultButton(_menuItemKind));
    }

    private void RefreshMenuItemPivotUnsavedIndicators()
    {
        var masterUnsaved = SettingsSession.Current.HasUnsavedInScope(
            SettingsCommitScope.ForMenuCustomRenderingMaster());

        for (var i = 0; i < _menuItemTabHeaders.Length; i++)
        {
            if (_menuItemTabHeaders[i] is null)
                continue;

            var kind = MenuItemRegistry.KindFromPivotIndex(i);
            var kindUnsaved = SettingsSession.Current.HasUnsavedInScope(
                SettingsCommitScope.ForMenuItemKind(kind));
            _menuItemTabHeaders[i].SetUnsavedVisible(masterUnsaved || kindUnsaved);
        }
    }

    private void AfterMenuItemMutation()
    {
        SettingsSession.Current.MarkDirty();
        UpdateMenuItemDefaultButtonState();
        RefreshMenuItemPivotUnsavedIndicators();
        MenuItemExpander.RefreshUnsavedIndicator();
    }

    private void WriteMenuItemValue(string name, uint value)
    {
        if (!MenuCustomRenderingCoordinator.IsGlobalGateOnFromRegistry())
            MenuCustomRenderingCoordinator.EnableGlobalGate(liveFlyout: false);

        var sub = MenuItemRegistry.RegSub(_menuItemKind);
        if (MenuItemRegistry.ShouldDeleteInsteadOfWrite(name, value))
            SettingsSession.Current.DeleteGlassValue(sub, name, liveFlyout: false);
        else
            SettingsSession.Current.WriteGlassDword(sub, name, value, liveFlyout: false);
        AfterMenuItemMutation();
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

    private void UpdateAnimationDefaultState()
    {
        if (_loading)
            return;

        AnimationPanel.SetDefaultActionState(
            AnimationExpander.IsGateOn,
            FlyoutAnimRegistry.ShouldDisableDefaultButton(_regSub));
    }

    private void SetAnimationControlsEnabled(bool enabled)
    {
        _popInStyleCombo.IsEnabled = enabled;
        _fadeOutBox.IsEnabled = enabled;
        _popInBox.IsEnabled = enabled;
        _fadeInBox.IsEnabled = enabled;
        _startRatioBox.IsEnabled = enabled;
        _immediateSwitch.IsEnabled = enabled;
    }

    private void OnGradientDefaultColor()
    {
        if (_loading || !GradientExpander.IsGateOn)
            return;

        var def = FlyoutThemeRegistry.ResetGradientToDefault(_regSub);
        GradientPicker.SetColor(def);
        SettingsSession.Current.MarkDirty();
        UpdatePickerDefaultStates();
    }

    private void OnBorderDefaultColor()
    {
        if (_loading || !BorderExpander.IsGateOn)
            return;

        var def = FlyoutThemeRegistry.ResetBorderToDefault(_regSub, ThemeRegistry.BorderColorName());
        BorderPicker.SetColor(def);
        SettingsSession.Current.MarkDirty();
        UpdatePickerDefaultStates();
    }

    private void OnAnimationDefault()
    {
        if (_loading || !AnimationExpander.IsGateOn)
            return;

        FlyoutAnimRegistry.ResetVisibleParametersToDefault(_regSub);
        SettingsSession.Current.MarkDirty();
        Reload();
    }

    private void OnOpenGlassBorderPolicyChanged(object? sender, EventArgs e) =>
        DispatcherQueue.TryEnqueue(ApplyPageTopNoticesUi);

    private void OnPageTopNoticesChanged(object? sender, EventArgs e) =>
        DispatcherQueue.TryEnqueue(ApplyPageTopNoticesUi);

    private void ApplyPageTopNoticesUi()
    {
        OpenGlassBorderNotice.Visibility = OpenGlassBorderPolicyCoordinator.ShouldShowBorderColorLimitNotice()
            ? Visibility.Visible
            : Visibility.Collapsed;
        DwmSystemBackdropNotice.Visibility = SwcaDwmBackdropNoticeCoordinator.ShouldShowPopupGradientTintLimitNotice()
            ? Visibility.Visible
            : Visibility.Collapsed;
        if (DwmSystemBackdropNotice.Visibility == Visibility.Visible)
            DwmSystemBackdropNotice.Message = SwcaDwmBackdropNoticeCoordinator.FormatPopupGradientTintLimitNoticeMessage();
        UpdatePickerDefaultStates();
    }

    private void ApplyOpenGlassBorderPolicyUi() => ApplyPageTopNoticesUi();

    private void OnEffectChanged()
    {
        if (_loading || !EffectRow.IsGateOn)
            return;

        var index = (uint)_effectCombo.SelectedIndex;
        if (_isSeparateScope
            ? FlyoutSeparateScopeUi.IsStrictLocalEffectPresent(_regSub)
            : FlyoutEffectRegistry.IsPresent(_regSub))
        {
            if (GlassRegistry.ReadDword(_regSub, FlyoutEffectRegistry.ValueName) == index)
                return;
        }
        else if (ReadEffectTypeComboIndexForUi() == index)
        {
            return;
        }

        WriteDword(FlyoutEffectRegistry.ValueName, index);
        _effectGateArmed = false;
    }

    private void EffectGate_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading)
            return;

        if (EffectRow.IsGateOn)
        {
            _effectGateArmed = true;
            _effectCombo.IsEnabled = true;
            _effectCombo.SelectedIndex = (int)ReadEffectTypeComboIndexForUi();
            return;
        }

        _effectGateArmed = false;
        FlyoutEffectRegistry.Delete(_regSub);
        SettingsSession.Current.MarkDirty();
        Reload();
    }

    private void ShadowSwitch_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading || !CornerRow.IsGateOn) return;
        WriteDword("EnableDropShadow", _shadowSwitch.IsOn ? 1u : 0u);
    }

    private void NoBorderSwitch_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        WriteDword("NoBorderColor", _noBorderSwitch.IsOn ? 1u : 0u);
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
    }

    private void CornersGate_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        if (CornerRow.IsGateOn)
        {
            _cornerCombo.IsEnabled = true;
            _shadowSwitch.IsEnabled = true;
            return;
        }

        FlyoutCornerRegistry.DeleteAll(_regSub);
        SettingsSession.Current.MarkDirty();
        Reload();
    }

    private void AnimGate_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        if (!AnimationExpander.IsGateOn)
        {
            FlyoutAnimRegistry.DeleteAll(_regSub);
            SettingsSession.Current.MarkDirty();
            Reload();
            return;
        }

        SetAnimationControlsEnabled(true);
        if (_isSeparateScope && _kind is FlyoutKind.Menu or FlyoutKind.DropDown)
            WriteAnim("EnableFluentAnimation", 1);
        SettingsSession.Current.MarkDirty();
        UpdateAnimationDefaultState();
    }

    private void OnPopInStyleChanged()
    {
        if (_loading || !AnimationExpander.IsGateOn) return;
        WriteAnim("PopInStyle", (uint)_popInStyleCombo.SelectedIndex);
        UpdateAnimationDefaultState();
    }

    private void ImmediateSwitch_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading || !AnimationExpander.IsGateOn) return;
        WriteAnim("ImmediateInterrupting", _immediateSwitch.IsOn ? 1u : 0u);
        UpdateAnimationDefaultState();
    }

    private void WriteDword(string name, uint v)
    {
        SettingsSession.Current.WriteGlassDword(_regSub, name, v, liveFlyout: false);
        if (string.Equals(name, FlyoutEffectRegistry.ValueName, StringComparison.Ordinal))
            RefreshDwmBackdropNoticeUi();
    }

    private void WriteAnim(string name, uint v) =>
        SettingsSession.Current.WriteGlassDword(FlyoutAnimRegistry.AnimSubKey(_regSub), name, v, liveFlyout: false);

    private void WriteColor(string name, uint argb) =>
        SettingsSession.Current.WriteGlassDword(_regSub, name, argb, liveFlyout: false);

    private async void MenuItemExpander_OnGateToggled(object sender, RoutedEventArgs e)
    {
        if (_loading)
            return;

        if (MenuItemExpander.IsGateOn)
        {
            MenuCustomRenderingCoordinator.EnableGlobalGate(liveFlyout: false);
            _menuItemGateEngaged = false;
            SetMenuItemControlsEnabled(true);
            UpdateMenuItemDefaultButtonState();
            MenuAdvancedExpander.RefreshUnsavedIndicator();
            AfterMenuItemMutation();
            return;
        }

        if (!MenuCustomRenderingCoordinator.ShouldConfirmMenuGateOff(_menuItemKind))
        {
            _loading = true;
            try
            {
                MenuItemRegistry.DeleteAll(_menuItemKind, liveFlyout: false);
                MenuCustomRenderingCoordinator.DisableGlobalGate(liveFlyout: false);
                FinishMenuAdvancedGateOffUi();
            }
            finally
            {
                _loading = false;
            }

            return;
        }

        _loading = true;
        MenuItemExpander.IsGateOn = true;
        _loading = false;

        var confirmed = await MenuAdvancedRemoveDialogService.ConfirmThenAsync(async () =>
        {
            _loading = true;
            try
            {
                MenuCustomRenderingCoordinator.DeleteAllMenuCustomRendering(liveFlyout: false);
                MenuItemExpander.IsGateOn = false;
                FinishMenuAdvancedGateOffUi();
            }
            finally
            {
                _loading = false;
            }

            await Task.CompletedTask.ConfigureAwait(false);
        }).ConfigureAwait(true);

        if (!confirmed)
            return;
    }

    private void FinishMenuAdvancedGateOffUi()
    {
        _menuItemGateEngaged = false;
        MenuCustomRenderingCoordinator.SyncSessionBaseline();
        ReloadMenuItemSection();
        MenuAdvancedExpander.RefreshUnsavedIndicator();
        RefreshMenuItemPivotUnsavedIndicators();
        FlushPageSessionNotice();
    }

    private void FlushPageSessionNotice()
    {
        if (Content is SettingsPageShell shell)
            shell.FlushSessionNotice();
    }

    private void OnMenuItemEnabledComboChanged()
    {
        if (_loading || !IsMenuItemGateActive())
            return;

        if (_menuItemEnabledCombo.SelectedIndex == 1)
        {
            WriteMenuItemValue(MenuItemRegistry.DisabledValueName, 1u);
            return;
        }

        var sub = MenuItemRegistry.RegSub(_menuItemKind);
        SettingsSession.Current.DeleteGlassValue(sub, MenuItemRegistry.DisabledValueName, liveFlyout: false);
        AfterMenuItemMutation();
    }

    private void MenuItemThemeColorToggle_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading || !IsMenuItemGateActive())
            return;

        WriteMenuItemValue(
            MenuItemRegistry.EnableThemeColorizationValueName,
            _menuItemThemeColorToggle.IsOn ? 1u : 0u);

        var showThemeType = _menuItemThemeColorToggle.IsOn;
        _menuItemThemeColorTypeLabel.Visibility = showThemeType ? Visibility.Visible : Visibility.Collapsed;
        _menuItemThemeColorTypeCombo.Visibility = showThemeType ? Visibility.Visible : Visibility.Collapsed;
        _menuItemThemeColorTypeCombo.IsEnabled = showThemeType;
    }

    private void OnMenuItemThemeColorTypeChanged()
    {
        if (_loading || !IsMenuItemGateActive() || !_menuItemThemeColorToggle.IsOn)
            return;

        MenuItemRegistry.WriteThemeColorizationType(
            _menuItemKind,
            ThemeRegistry.IsDarkMode(),
            ThemeColorTypeFromComboIndex(_menuItemThemeColorTypeCombo.SelectedIndex));
        AfterMenuItemMutation();
    }

    private void AfterMenuRootMutation()
    {
        SettingsSession.Current.MarkDirty();
        UpdateMenuIconDefaultButtonState();
        MenuGeneralSettingsExpander.RefreshUnsavedIndicator();
        MenuAdvancedExpander.RefreshUnsavedIndicator();
    }

    private void WriteMenuRootValue(string valueName, uint value)
    {
        MenuRootRegistry.WriteDword(valueName, value, liveFlyout: false);
        AfterMenuRootMutation();
    }

    private void SetMenuImmersiveStyleControlsEnabled(bool enabled)
    {
        _menuImmersiveStyleToggle.IsEnabled = enabled;
    }

    private void UpdateMenuIconDefaultButtonState()
    {
        if (_loading)
            return;

        MenuGeneralPanel.SetDefaultColorActionState(
            true,
            !MenuRootRegistry.IsIconBackgroundPresent());
    }

    private void MenuGeneralSettingsExpander_OnGateToggled(object sender, RoutedEventArgs e)
    {
        if (_loading)
            return;

        if (MenuGeneralSettingsExpander.IsGateOn)
        {
            SetMenuImmersiveStyleControlsEnabled(true);
            return;
        }

        SettingsSession.Current.DeleteGlassValue(
            MenuRootRegistry.MenuScopeSubKey,
            MenuRootRegistry.EnableImmersiveStyleValueName,
            liveFlyout: false);
        AfterMenuRootMutation();
        ReloadMenuGeneralSection();
    }

    private void MenuNoSystemDropShadowToggle_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading)
            return;

        WriteMenuRootValue(
            MenuRootRegistry.NoSystemDropShadowValueName,
            _menuNoSystemDropShadowToggle.IsOn ? 1u : 0u);
    }

    private void MenuImmersiveStyleToggle_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading || !MenuGeneralSettingsExpander.IsGateOn)
            return;

        WriteMenuRootValue(
            MenuRootRegistry.EnableImmersiveStyleValueName,
            _menuImmersiveStyleToggle.IsOn ? 1u : 0u);
    }

    private void MenuNoModernAppBackgroundToggle_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading)
            return;

        WriteMenuRootValue(
            MenuRootRegistry.NoModernAppBackgroundColorValueName,
            _menuNoModernAppBackgroundToggle.IsOn ? 1u : 0u);
    }

    private void MenuGeneralPanel_OnDefaultClick(object sender, RoutedEventArgs e)
    {
        if (_loading)
            return;

        MenuRootRegistry.DeleteIconBackground(liveFlyout: false);
        AfterMenuRootMutation();
        MenuIconPicker.SetColor(MenuRootRegistry.ReadEffectiveIconColor());
        _menuIconThresholdBox.Value = MenuRootRegistry.ReadEffectiveIconThreshold();
        SettingsNumberBoxCommit.SyncCommittedValue(
            _menuIconThresholdBox,
            _menuIconThresholdBox.Value);
        UpdateMenuIconDefaultButtonState();
    }

    private void MenuIconThresholdBox_OnCommit(double value)
    {
        MenuRootRegistry.WriteIconBackground(
            MenuRootRegistry.ReadEffectiveIconColor(),
            (uint)value,
            liveFlyout: false);
        AfterMenuRootMutation();
    }

    private void MenuItemPanel_OnDefaultClick(object sender, RoutedEventArgs e)
    {
        if (_loading || !IsMenuItemGateActive())
            return;

        MenuItemRegistry.DeleteAll(_menuItemKind, liveFlyout: false);
        AfterMenuItemMutation();
        ReloadMenuItemSection();
    }

    private void TooltipRenderingExpander_OnGateToggled(object sender, RoutedEventArgs e)
    {
        if (_loading)
            return;

        if (TooltipRenderingExpander.IsGateOn)
        {
            _tooltipRenderingGateEngaged = true;
            SetTooltipRenderingControlsEnabled(true);
            UpdateTooltipRenderingDefaultButtonState();
            return;
        }

        FinishTooltipRenderingGateOffUi();
    }

    private void FinishTooltipRenderingGateOffUi()
    {
        _loading = true;
        try
        {
            _tooltipRenderingGateEngaged = false;
            if (TooltipRenderingRegistry.IsAnyPresent())
                TooltipRenderingRegistry.DeleteAll(liveFlyout: false);

            SettingsSession.Current.SyncScopeBaseline(SettingsCommitScope.ForTooltipRendering());
            ReloadTooltipRenderingSection();
            FlushPageSessionNotice();
        }
        finally
        {
            _loading = false;
        }
    }

    private void TooltipRenderingPanel_OnDefaultClick(object sender, RoutedEventArgs e)
    {
        if (_loading || !TooltipRenderingExpander.IsGateOn)
            return;

        TooltipRenderingRegistry.DeleteAll(liveFlyout: false);
        AfterTooltipRenderingMutation();
        ReloadTooltipRenderingSection();
    }

    private void OnTooltipMarginsTypeChanged()
    {
        if (_loading || !TooltipRenderingExpander.IsGateOn)
            return;

        WriteTooltipRenderingValue(
            TooltipRenderingRegistry.MarginsTypeValueName,
            (uint)_tooltipMarginsTypeCombo.SelectedIndex);
    }

}
