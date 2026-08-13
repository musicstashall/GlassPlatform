using GlassSettings.Core.Dwm;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Snapshot;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;

namespace GlassSettings.Controls;

public sealed partial class DwmOpacityGroupRow : UserControl, ISettingsUnsavedScopeHost
{
    private GlassSettingsLabeledToggleSwitch? _gate;
    private ComboBox? _modeCombo;
    private Slider? _slider;
    private TextBlock? _sliderValueText;
    private HubInlineLabel? _comboHeaderLabel;
    private FrameworkElement? _sliderActionsPanel;
    private readonly SpoilerHeaderLayoutHost _layoutHost = new();
    private bool _loading;
    private bool _hasParallelWarning;

    private const double SliderValueSuffixGap = 8;
    private const double SliderValueToToggleGap = 20;
    private const uint ThemeOpacitySentinel = 0xFFFFFFFF;

    public DwmOpacityGroupRow()
    {
        InitializeComponent();
        // Нет expand/navigate — card PointerOver не показываем.
        Loaded += (_, _) =>
        {
            _layoutHost.Attach(
                RootBorder,
                HeaderChrome,
                ActionsBelowHost,
                SpoilerHeaderRowLayoutBridge.CreateOptions(
                    HeaderChrome,
                    hasGateRail: !UseParentGate && !UseOpenGlassModeWithoutGate && _gate is not null),
                () => UseOpenGlassModeWithoutGate ? IsManualMode() : IsRowEnabled);
            UpdateHeaderControlsLayout();
            SettingsSession.Current.DirtyChanged += Session_OnDirtyChanged;
            RefreshUnsavedIndicator();
        };
        Unloaded += (_, _) =>
        {
            SettingsSession.Current.DirtyChanged -= Session_OnDirtyChanged;
            _layoutHost.Detach();
        };
    }

    public SettingsCommitScope? UnsavedScope { get; set; }

    public bool IsUnsavedPending
    {
        get => HeaderChrome.IsUnsavedPending;
        set => HeaderChrome.IsUnsavedPending = value;
    }

    public void RefreshUnsavedIndicator()
    {
        IsUnsavedPending = UnsavedScope is { } scope && SettingsSession.Current.HasUnsavedInScope(scope);
    }

    public void SetParallelBranchWarning(
        bool visible,
        string? toolTip,
        string? markerText = null,
        bool highlightBackground = false,
        string? conflictSuffix = null)
    {
        _hasParallelWarning = highlightBackground;
        HeaderChrome.SetParallelBranchWarning(
            visible,
            toolTip,
            FormatParallelDuplicateMarker(markerText, highlightBackground ? conflictSuffix : null));
        ApplyParallelWarningHighlight();
    }

    private void Session_OnDirtyChanged(object? sender, EventArgs e) => RefreshUnsavedIndicator();

    public string Title
    {
        get => HeaderChrome.Title;
        set => HeaderChrome.Title = value;
    }

    public string Description
    {
        get => HeaderChrome.Description;
        set => HeaderChrome.Description = value;
    }

    public string IconGlyph
    {
        get => HeaderChrome.IconGlyph;
        set => HeaderChrome.IconGlyph = value;
    }

    public string RegistryValueName { get; set; } = string.Empty;

    public DwmRegistryHive Hive { get; set; } = DwmRegistryHive.Hkcu;

    /// <summary>Deferred-commit слайдера (250 ms). По умолчанию false — мгновенная запись.</summary>
    public bool UseDeferredSliderCommit { get; set; }

    /// <summary>Доп. callback в конце жеста live-слайдера (после MarkDirty в Attach).</summary>
    public Action? OnLiveGestureCommitted { get; set; }

    /// <summary>OpenGlass Theme: Auto=delete, From theme=0xFFFFFFFF, Manual=0–100.</summary>
    public bool UseOpenGlassOpacitySemantics { get; set; }

    /// <summary>Debounced live-preview для OpenGlass opacity-слайдеров (0 = каждый tick).</summary>
    public int LivePreviewDebounceMs { get; set; }

    /// <summary>Слайдер + значение «NN%» справа (как SettingsFlatGateRow на странице текстур).</summary>
    public bool UseSliderValueSuffix { get; set; }

    /// <summary>Gate на родительском спойлере; локальный тумблер строки не создаётся.</summary>
    public bool UseParentGate { get; set; }

    /// <summary>OpenGlass ColorizationOpacity*: без локального gate; Auto = ключ отсутствует.</summary>
    public bool UseOpenGlassModeWithoutGate { get; set; }

    public event EventHandler? ValueChanged;

    private bool _parentGateOn;

    public void SetParentGateOn(bool on)
    {
        if (!UseParentGate)
            return;

        _parentGateOn = on;

        // При включении родительского gate разблокируем combo
        if (on && _modeCombo is not null)
            _modeCombo.IsEnabled = true;

        UpdateCardVisualState();
    }

    /// <summary>UI «включено» без записи в реестр (режим Auto, ключ отсутствует).</summary>
    public void ApplyParentGateEnableDefaults()
    {
        if (!UseParentGate || !_parentGateOn || _modeCombo is null || _slider is null)
            return;

        _loading = true;
        _modeCombo.IsEnabled = true;
        _modeCombo.SelectedIndex = 0;
        _slider.Value = DwmDefaults.DefaultWhenKeyAbsent(RegistryValueName);
        _slider.IsEnabled = false;
        UpdateSliderValueLabel(_slider.Value);
        _loading = false;
        UpdateCardVisualState();
    }

    private bool IsRowEnabled =>
        UseOpenGlassModeWithoutGate ? true :
        UseParentGate ? _parentGateOn :
        _gate?.IsOn == true;

    private bool UsesSplitHeaderComboLayout => UseOpenGlassModeWithoutGate && UseSliderValueSuffix;

    public void BuildControls()
    {
        _modeCombo = new ComboBox { MinWidth = 100 };
        _modeCombo.Items.Add(GlassSettingsResources.Get("Dwm_ModeAuto"));
        if (UseOpenGlassOpacitySemantics)
            _modeCombo.Items.Add(GlassSettingsResources.Get("Dwm_ModeFromTheme"));
        _modeCombo.Items.Add(GlassSettingsResources.Get("Dwm_ModeManual"));
        SettingsInteractionDeferral.WhenComboCommit(
            _modeCombo,
            () => !_loading && (UseOpenGlassModeWithoutGate || IsRowEnabled),
            OnModeChanged);

        _slider = SettingsSliderFactory.Create();

        if (UseOpenGlassOpacitySemantics)
        {
            SettingsSliderCommit.Attach(
                _slider,
                () => !_loading && IsRowEnabled && IsManualMode(),
                UpdateSliderValueLabel,
                value => SettingsSession.Current.WriteDwmDwordLivePreview(Hive, RegistryValueName, (uint)value),
                SettingsSliderCommit.CommitMode.LiveImmediate,
                onGestureCommitted: () =>
                {
                    SettingsSession.Current.MarkDirty();
                    OnLiveGestureCommitted?.Invoke();
                },
                liveDebounceMs: LivePreviewDebounceMs);
        }
        else if (UseDeferredSliderCommit)
        {
            SettingsSliderCommit.Attach(
                _slider,
                () => !_loading && IsRowEnabled && _modeCombo?.SelectedIndex == 1,
                UpdateSliderValueLabel,
                value =>
                {
                    SettingsSession.Current.WriteDwmDword(Hive, RegistryValueName, (uint)value, liveDwm: true, dwmNotifyImmediate: true);
                    ValueChanged?.Invoke(this, EventArgs.Empty);
                });
        }
        else
        {
            _slider.ValueChanged += (_, _) => OnSliderChanged();
        }

        if (!UseParentGate && !UseOpenGlassModeWithoutGate)
        {
            _gate = new GlassSettingsLabeledToggleSwitch();
            _gate.Toggled += (_, _) => OnGateToggled();
        }

        _comboHeaderLabel = new HubInlineLabel
        {
            Label = GlassSettingsResources.Get("Dwm_LabelMode"),
            Control = _modeCombo
        };

        if (UsesSplitHeaderComboLayout)
        {
            _sliderActionsPanel = BuildSliderValueSuffixPanel();
            if (_sliderActionsPanel is FrameworkElement sliderPanel)
            {
                sliderPanel.HorizontalAlignment = HorizontalAlignment.Stretch;
                sliderPanel.ClearValue(FrameworkElement.WidthProperty);
            }
        }
        else if (UseSliderValueSuffix)
        {
            _sliderActionsPanel = BuildSliderValueSuffixPanel();
        }

        if (UseSliderValueSuffix && _gate is not null)
            _gate.Margin = new Thickness(SliderValueToToggleGap, 0, 0, 0);

        HeaderChrome.Trailing = _gate;

        if (UseOpenGlassModeWithoutGate)
            HeaderChrome.DimHeaderActionsWithGate = false;

        UpdateSliderValueLabel(_slider.Value);
        UpdateHeaderControlsLayout();
    }

    private void UpdateHeaderControlsLayout()
    {
        if (_comboHeaderLabel is null)
            return;

        if (UsesSplitHeaderComboLayout)
        {
            SpoilerHeaderRowLayoutBridge.ApplyHeaderControlsWithoutGate(
                HeaderChrome,
                _layoutHost,
                _comboHeaderLabel,
                _sliderActionsPanel);
            return;
        }

        var sliderContent = UseSliderValueSuffix
            ? _sliderActionsPanel
            : (UIElement?)new HubInlineLabel
            {
                Label = GlassSettingsResources.Get("Dwm_LabelPercent"),
                Control = _slider
            };

        SpoilerHeaderRowLayoutBridge.ApplyHeaderControls(
            HeaderChrome,
            _layoutHost,
            _comboHeaderLabel,
            sliderContent);
    }

    private Grid BuildSliderValueSuffixPanel()
    {
        _sliderValueText = new TextBlock
        {
            Style = Application.Current.Resources["GlassSettingsSpoilerSecondaryTextStyle"] as Style,
            VerticalAlignment = VerticalAlignment.Center
        };
        return SettingsSliderFactory.CreateSliderValueSuffixPanel(_slider!, _sliderValueText, SliderValueSuffixGap);
    }

    private void UpdateSliderValueLabel(double value)
    {
        if (_sliderValueText is null)
            return;

        _sliderValueText.Text = GlassSettingsResources.Format("Dwm_ReflectionPercentFormat", (uint)value);
    }

    private bool IsManualMode() =>
        UseOpenGlassOpacitySemantics
            ? _modeCombo?.SelectedIndex == 2
            : _modeCombo?.SelectedIndex == 1;

    public void RefreshHeaderLayout()
    {
        _layoutHost.Invalidate();
    }

    private static string? FormatParallelDuplicateMarker(string? markerText, string? conflictSuffix)
    {
        if (string.IsNullOrWhiteSpace(conflictSuffix))
            return markerText;

        var baseText = string.IsNullOrWhiteSpace(markerText)
            ? GlassSettingsResources.Get("Dwm_ParallelDuplicateMarker")
            : markerText;
        return baseText + conflictSuffix;
    }

    private void ApplyParallelWarningHighlight()
    {
        RootBorder.Background = AppThemeVisualResolver.ResolveThemeBrush("LayerFillColorDefaultBrush");
        RootBorder.BorderBrush = _hasParallelWarning
            ? AppThemeVisualResolver.ResolveThemeBrush("SystemFillColorCautionBrush")
            : AppThemeVisualResolver.ResolveThemeBrush("CardStrokeColorDefaultBrush");
    }

    private void SyncActionsBelowGateChrome(bool? enabledOverride = null) =>
        _layoutHost.SyncBelowHostDim(enabledOverride ?? IsRowEnabled);

    /// <summary>Combo яркий; таблица контролов — по gate.</summary>
    private void SyncSliderVisualChrome(bool enabled)
    {
        _layoutHost.SyncBelowHostDim(enabled);
        ApplyHostDim(HeaderChrome.ControlsTablePresenter, enabled);
    }

    private void UpdateCardVisualState()
    {
        if (UseOpenGlassModeWithoutGate)
        {
            var manual = IsManualMode();
            HeaderChrome.ApplyGateVisual(manual);
            SyncSliderVisualChrome(manual);
            VisualStateManager.GoToState(this, "Normal", false);
            return;
        }

        if (!UseParentGate && _gate is null)
            return;

        if (_gate is not null)
            _gate.IsEnabled = true;
        HeaderChrome.ApplyGateVisual(IsRowEnabled);
        SyncActionsBelowGateChrome(IsRowEnabled);
        VisualStateManager.GoToState(this, "Normal", false);
    }

    private static void ApplyHostDim(FrameworkElement host, bool enabled)
    {
        if (host.Visibility != Visibility.Visible)
        {
            host.ClearValue(UIElement.OpacityProperty);
            return;
        }

        if (enabled)
            host.ClearValue(UIElement.OpacityProperty);
        else
            host.Opacity = 0.55;
    }

    public void Reload()
    {
        if (string.IsNullOrEmpty(RegistryValueName))
            return;

        if (!UseParentGate && !UseOpenGlassModeWithoutGate && _gate is null)
            return;

        _loading = true;
        var opt = DwmRegistryStore.ReadDwordForPage(Hive, RegistryValueName);

        if (UseOpenGlassOpacitySemantics)
            ReloadOpenGlass(opt);
        else
            ReloadLegacy(opt);

        _loading = false;
        if (_slider is not null && (UseOpenGlassOpacitySemantics || UseDeferredSliderCommit))
            SettingsSliderCommit.SyncCommittedValue(_slider, _slider.Value);
        UpdateSliderValueLabel(_slider?.Value ?? 0);
        UpdateCardVisualState();
    }

    private void ReloadOpenGlass(uint? opt)
    {
        if (UseOpenGlassModeWithoutGate || UseParentGate)
        {
            ReloadOpenGlassModeOnly(opt);
            return;
        }

        if (opt is null)
        {
            _gate!.IsOn = false;
            _modeCombo!.SelectedIndex = 0;
            _slider!.Value = DwmDefaults.DefaultWhenKeyAbsent(RegistryValueName);
            _slider.IsEnabled = false;
            _modeCombo.IsEnabled = false;
            return;
        }

        _gate!.IsOn = true;
        _modeCombo!.IsEnabled = true;

        if (opt == ThemeOpacitySentinel)
        {
            _modeCombo.SelectedIndex = 1;
            _slider!.Value = 50;
            _slider.IsEnabled = false;
        }
        else if (DwmDefaults.IsOpacityAutoValue(opt.Value))
        {
            _modeCombo.SelectedIndex = 0;
            _slider!.Value = DwmDefaults.DefaultWhenKeyAbsent(RegistryValueName);
            _slider.IsEnabled = false;
        }
        else
        {
            _modeCombo.SelectedIndex = 2;
            _slider!.Value = opt.Value > 100 ? 100 : opt.Value;
            _slider.IsEnabled = true;
        }
    }

    private void ReloadOpenGlassModeOnly(uint? opt)
    {
        if (UseParentGate && !_parentGateOn)
        {
            _modeCombo!.SelectedIndex = 0;
            _slider!.Value = DwmDefaults.DefaultWhenKeyAbsent(RegistryValueName);
            _slider.IsEnabled = false;
            _modeCombo.IsEnabled = false;
            return;
        }

        _modeCombo!.IsEnabled = true;

        if (opt is null)
        {
            _modeCombo.SelectedIndex = 0;
            _slider!.Value = DwmDefaults.DefaultWhenKeyAbsent(RegistryValueName);
            _slider.IsEnabled = false;
            return;
        }

        if (opt == ThemeOpacitySentinel)
        {
            _modeCombo.SelectedIndex = 1;
            _slider!.Value = 50;
            _slider.IsEnabled = false;
        }
        else if (DwmDefaults.IsOpacityAutoValue(opt.Value))
        {
            _modeCombo.SelectedIndex = 0;
            _slider!.Value = DwmDefaults.DefaultWhenKeyAbsent(RegistryValueName);
            _slider.IsEnabled = false;
        }
        else
        {
            _modeCombo.SelectedIndex = 2;
            _slider!.Value = opt.Value > 100 ? 100 : opt.Value;
            _slider.IsEnabled = true;
        }
    }

    private void ReloadLegacy(uint? opt)
    {
        var isAuto = opt is null || DwmDefaults.IsOpacityAutoValue(opt.Value);
        _gate!.IsOn = opt is not null;
        _modeCombo!.SelectedIndex = isAuto ? 0 : 1;
        var showVal = isAuto
            ? DwmDefaults.DefaultWhenKeyAbsent(RegistryValueName)
            : (opt!.Value > 100 ? 100u : opt.Value);
        _slider!.Value = showVal;
        _slider.IsEnabled = _gate.IsOn && !isAuto;
        _modeCombo.IsEnabled = _gate.IsOn;
    }

    private void OnGateToggled()
    {
        if (_loading || _gate is null) return;
        if (_gate.IsOn)
        {
            _modeCombo!.IsEnabled = true;
            _modeCombo.SelectedIndex = 0;
            _slider!.Value = DwmDefaults.DefaultWhenKeyAbsent(RegistryValueName);
            _slider.IsEnabled = false;
            UpdateCardVisualState();
            return;
        }

        var live = UseOpenGlassOpacitySemantics;
        SettingsSession.Current.DeleteDwmValue(Hive, RegistryValueName, liveDwm: live);
        Reload();
        ValueChanged?.Invoke(this, EventArgs.Empty);
    }

    private void OnModeChanged()
    {
        if (_loading || (!UseOpenGlassModeWithoutGate && !IsRowEnabled))
            return;

        if (UseOpenGlassOpacitySemantics)
        {
            switch (_modeCombo!.SelectedIndex)
            {
                case 0:
                    SettingsSession.Current.DeleteDwmValue(Hive, RegistryValueName, liveDwm: true);
                    _slider!.IsEnabled = false;
                    _slider.Value = DwmDefaults.DefaultWhenKeyAbsent(RegistryValueName);
                    break;
                case 1:
                    SettingsSession.Current.WriteDwmDword(
                        Hive,
                        RegistryValueName, ThemeOpacitySentinel, liveDwm: true, dwmNotifyImmediate: true);
                    _slider!.IsEnabled = false;
                    break;
                default:
                    var v = (uint)_slider!.Value;
                    SettingsSession.Current.WriteDwmDword(Hive, RegistryValueName, v, liveDwm: true, dwmNotifyImmediate: true);
                    _slider.IsEnabled = true;
                    if (UseDeferredSliderCommit || UseOpenGlassOpacitySemantics)
                        SettingsSliderCommit.SyncCommittedValue(_slider, _slider.Value);
                    break;
            }
        }
        else
        {
            if (_modeCombo!.SelectedIndex == 0)
            {
                SettingsSession.Current.WriteDwmDword(Hive, RegistryValueName, DwmDefaults.OpacityAuto, liveDwm: true);
                _slider!.IsEnabled = false;
                _slider.Value = DwmDefaults.DefaultWhenKeyAbsent(RegistryValueName);
            }
            else
            {
                var v = (uint)_slider!.Value;
                SettingsSession.Current.WriteDwmDword(Hive, RegistryValueName, v, liveDwm: true, dwmNotifyImmediate: true);
                _slider.IsEnabled = true;
            }
        }

        UpdateSliderValueLabel(_slider?.Value ?? 0);
        UpdateCardVisualState();
        ValueChanged?.Invoke(this, EventArgs.Empty);
    }

    private void OnSliderChanged()
    {
        if (_loading || !IsRowEnabled || !IsManualMode()) return;
        UpdateSliderValueLabel(_slider!.Value);
        SettingsSession.Current.WriteDwmDword(Hive, RegistryValueName, (uint)_slider.Value, liveDwm: true, dwmNotifyImmediate: true);
        ValueChanged?.Invoke(this, EventArgs.Empty);
    }
}
