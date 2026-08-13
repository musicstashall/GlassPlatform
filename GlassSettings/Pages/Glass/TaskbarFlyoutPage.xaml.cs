using System.Threading.Tasks;
using GlassSettings.Controls;
using GlassSettings.Core.Customization;
using GlassSettings.Core.Flyout;
using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Settings;
using GlassSettings.Core.Snapshot;
using GlassSettings.Navigation;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;

namespace GlassSettings.Pages.Glass;

public sealed partial class TaskbarFlyoutPage : Page
{
    private const string RegSub = "Tray";
    private const string XamlRegSub = "Tray\\Xaml";

    private readonly GlassSettingsLabeledToggleSwitch _zonesSwitch = new();
    private readonly TextBlock _noBorderLabel = new();
    private readonly ToggleSwitch _noBorderSwitch = new()
    {
        OnContent = string.Empty,
        OffContent = string.Empty,
        MinWidth = 0,
        HorizontalAlignment = HorizontalAlignment.Left,
        VerticalAlignment = VerticalAlignment.Center,
    };

    private ComboBox? _effectCombo;
    private ComboBox? _cornerCombo;
    private readonly HubInlineLabel _cornerInlineLabel = new();
    private bool _loading;
    private bool _effectGateArmed;
    private bool _gradientGateArmed;
    private bool _borderGateArmed;
    private bool _cornerGateArmed;
    /// <summary>Зелёный tip «нужны углы ≠ Системные» — один раз за визит на страницу.</summary>
    private bool _zonesDefaultCornersTipShownThisVisit;
    /// <summary>Зелёный tip «градиент недоступен при System backdrop» — один раз за визит.</summary>
    private bool _gradientSystemBackdropTipShownThisVisit;
    private bool _lastIsDarkMode;
    private EventHandler<DesktopEnvironmentChangedEventArgs>? _environmentChangedHandler;
    private readonly bool _isWin11 = WindowsOsHelper.IsWindows11OrGreater();
    private readonly bool _cornersSupported;

    public TaskbarFlyoutPage()
    {
        _cornersSupported = _isWin11;
        InitializeComponent();
        ConfigureZonesRow();
        BuildNoBorderFooter();
        ApplyLocalizedStrings();
        GradientExpander.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.Gradient);
        BorderExpander.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.Border);
        EffectRow.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.EffectType);
        CornerRow.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.Corners);
        ZonesRow.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.Corners);

        if (!_cornersSupported)
        {
            CornerRow.Visibility = Visibility.Collapsed;
            ZonesRow.Visibility = Visibility.Collapsed;
        }

        PaintPathBar.Visibility = _isWin11 ? Visibility.Visible : Visibility.Collapsed;

        _effectCombo = new ComboBox { MinWidth = 200 };
        PopulateEffectComboForCurrentPath();
        SettingsInteractionDeferral.WhenComboCommit(
            _effectCombo,
            () => !_loading && EffectRow.IsGateOn,
            OnEffectChanged);
        EffectRow.HeaderActions = _effectCombo;

        if (_cornersSupported)
        {
            _cornerCombo = new ComboBox { MinWidth = 200 };
            PopulateCornerComboForCurrentPath();
            _cornerCombo.SelectionChanged += (_, _) =>
            {
                if (_loading || !CornerRow.IsGateOn)
                    return;
                if (_cornerCombo.SelectedIndex < 0)
                    return;

                OnCornerTypeCommitted();
            };
            _cornerInlineLabel.Control = _cornerCombo;
            CornerRow.HeaderActions = _cornerInlineLabel;
        }

        GradientPicker.UseLivePreview = true;
        BorderPicker.UseLivePreview = true;
        GradientPicker.ColorLivePreview += (_, argb) =>
        {
            UpdatePickerDefaultStates();
            FlyoutLiveReload.ScheduleTaskbarGradientPreview(argb);
        };
        BorderPicker.ColorLivePreview += (_, argb) =>
        {
            UpdatePickerDefaultStates();
            FlyoutLiveReload.ScheduleTaskbarBorderColorPreview(argb);
        };
        GradientPicker.ColorChanged += (_, argb) =>
        {
            OnGradientColorCommitted(argb);
            UpdatePickerDefaultStates();
        };
        BorderPicker.ColorChanged += (_, argb) =>
        {
            OnBorderColorCommitted(argb);
            UpdatePickerDefaultStates();
        };
        GradientPanel.DefaultColorClick += (_, _) => OnGradientDefaultColor();
        BorderPanel.DefaultColorClick += (_, _) => OnBorderDefaultColor();
        Loaded += (_, _) =>
        {
            OpenGlassBorderPolicyCoordinator.Changed += OnPageTopNoticesChanged;
            SwcaDwmBackdropNoticeCoordinator.Changed += OnPageTopNoticesChanged;
            SettingsSession.Current.DirtyChanged += OnSessionDirtyChanged;
            FlyoutThemeRegistry.PurgeStaleCachedSystemSamples();
            ApplyExpanderUnsavedScopes();
            _lastIsDarkMode = ThemeRegistry.IsTrayXamlDarkMode();
            HookEnvironmentWatcher();
            Reload();
        };
        Unloaded += (_, _) =>
        {
            OpenGlassBorderPolicyCoordinator.Changed -= OnPageTopNoticesChanged;
            SwcaDwmBackdropNoticeCoordinator.Changed -= OnPageTopNoticesChanged;
            SettingsSession.Current.DirtyChanged -= OnSessionDirtyChanged;
            UnhookEnvironmentWatcher();
        };
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
        var isDark = ThemeRegistry.IsTrayXamlDarkMode();
        if (isDark == _lastIsDarkMode)
            return;

        _lastIsDarkMode = isDark;
        if (_loading || !IsLoaded)
            return;

        // Только UI pickers (LightMode_/DarkMode_); runtime reapply — в explorer GetMsgHook.
        Reload();
    }

    private void OnSessionDirtyChanged(object? sender, EventArgs e) =>
        DispatcherQueue.TryEnqueue(RefreshPaintPathUnsavedIndicators);

    private void ConfigureZonesRow()
    {
        _zonesSwitch.VerticalAlignment = VerticalAlignment.Center;
        _zonesSwitch.Toggled += ZonesSwitch_OnToggled;
        ZonesRow.HeaderActions = _zonesSwitch;
        ZonesRow.DimHeaderActionsWhenInactive = false;
        ZonesRow.Visibility = Visibility.Collapsed;
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

    private void OnPageTopNoticesChanged(object? sender, EventArgs e) =>
        DispatcherQueue.TryEnqueue(ApplyPageTopNoticesUi);

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        _zonesDefaultCornersTipShownThisVisit = false;
        _gradientSystemBackdropTipShownThisVisit = false;
        _lastIsDarkMode = ThemeRegistry.IsTrayXamlDarkMode();
        ApplyExpanderUnsavedScopes();
        Reload();
    }

    private void ApplyExpanderUnsavedScopes()
    {
        EffectRow.UnsavedScope = SettingsCommitScope.ForFlyoutParameterGroup(
            EffectRegSub(),
            FlyoutParameterGroup.Effect);
        GradientExpander.UnsavedScope = SettingsCommitScope.ForFlyoutParameterGroup(
            ColorRegSub(), FlyoutParameterGroup.Gradient);
        BorderExpander.UnsavedScope = SettingsCommitScope.ForFlyoutParameterGroup(
            ColorRegSub(), FlyoutParameterGroup.Border);
        if (_cornersSupported)
            CornerRow.UnsavedScope = SettingsCommitScope.ForFlyoutParameterGroup(
                CornerRegSub(), FlyoutParameterGroup.Corners);
        if (_isWin11)
            ZonesRow.UnsavedScope = SettingsCommitScope.ForFlyoutParameterGroup(
                XamlRegSub, FlyoutParameterGroup.Zones);
        AdjacentWindowsRow.UnsavedScope = TaskbarScopeCatalog.AdjacentPageScope;
        RefreshPaintPathUnsavedIndicators();
    }

    private void RefreshPaintPathUnsavedIndicators()
    {
        if (!_isWin11)
            return;

        // Точка только при изменении BorderDrawMode, не параметров внутри DWM/XAML.
        var pathUnsaved = SettingsSession.Current.HasUnsavedInScope(
            SettingsCommitScope.ForFlyoutParameterGroup(RegSub, FlyoutParameterGroup.PaintPath));
        PaintPathBar.SetPathUnsaved(pathUnsaved);
    }

    private string EffectRegSub() => IsBorderDrawModeXaml() ? XamlRegSub : RegSub;
    private string ColorRegSub() => IsBorderDrawModeXaml() ? XamlRegSub : RegSub;
    private string CornerRegSub() => IsBorderDrawModeXaml() ? XamlRegSub : RegSub;

    private void ApplyLocalizedStrings()
    {
        var gradientDesc = GlassSettingsResources.Get("Flyout_GradientColorDesc");
        GradientExpander.Title = GlassSettingsResources.Get("Flyout_GradientColor");
        GradientExpander.Description = GlassSettingsResources.Get("Flyout_GradientColorSubtitle");
        GradientPanel.Description = gradientDesc;

        var borderDesc = GlassSettingsResources.Get("Flyout_BorderColorDesc");
        BorderExpander.Title = GlassSettingsResources.Get("Flyout_BorderColor");
        BorderExpander.Description = GlassSettingsResources.Get("Flyout_BorderColorSubtitle");
        BorderPanel.Description = borderDesc;

        _noBorderLabel.Text = GlassSettingsResources.Get("Flyout_NoBorder");

        ZonesRow.Title = GlassSettingsResources.Get("Flyout_SeparateZones");
        ZonesRow.Description = GlassSettingsResources.Get("Flyout_SeparateZonesDesc");

        AdjacentWindowsRow.Title = GlassSettingsResources.Get("Glass_AdjacentWindows");
        AdjacentWindowsRow.Description = GlassSettingsResources.Get("Glass_AdjacentWindowsDesc");
        AdjacentWindowsRow.IconGlyph = SettingsSectionGlyphs.TryForNavigateId("popup");

        OpenGlassTrayBorderNotice.Title = GlassSettingsResources.Get("Notice_OpenGlassBorderTitle");
        OpenGlassTrayBorderNotice.Message =
            OpenGlassBorderPolicyCoordinator.FormatBorderLimitNoticeMessage(includeTaskbarSuffix: true);

        DwmSystemBackdropNotice.Title = GlassSettingsResources.Get("Notice_SwcaDwmBackdropTitle");
        DwmSystemBackdropNotice.Message = GlassSettingsResources.Get("Notice_SwcaDwmBackdropBody");

        EffectRow.Title = GlassSettingsResources.Get("Flyout_EffectType");
        EffectRow.Description = FlyoutEffectScopeDescriptions.ForTaskbar();

        PaintPathBar.ApplyLocalizedStrings();

        if (_cornersSupported)
        {
            CornerRow.Title = GlassSettingsResources.Get("Flyout_CornerType");
            CornerRow.Description = GlassSettingsResources.Get("Flyout_CornerTypeDesc");
            _cornerInlineLabel.Label = GlassSettingsResources.Get("ParamLabel_CornerType");

            var cornerIndex = _cornerCombo?.SelectedIndex ?? -1;
            if (_cornerCombo is not null)
            {
                PopulateCornerComboForCurrentPath();
                if (cornerIndex >= 0 && cornerIndex < _cornerCombo.Items.Count)
                    _cornerCombo.SelectedIndex = cornerIndex;
            }
        }
    }

    private void PopulateCornerComboForCurrentPath()
    {
        if (_cornerCombo is null)
            return;

        FlyoutCornerNames.PopulateCombo(_cornerCombo, IsBorderDrawModeXaml());
    }

    private void PopulateEffectComboForCurrentPath()
    {
        if (_effectCombo is null)
            return;

        if (IsBorderDrawModeXaml())
            FlyoutXamlEffectNames.PopulateCombo(_effectCombo);
        else
            FlyoutEffectNames.PopulateCombo(_effectCombo);
    }

    private void Reload()
    {
        _loading = true;
        // Не вызывать ApplyPolicy/Notify* — открытие страницы не должно дёргать TaskbarCompat.
        // Баннеры обновляются в ApplyPageTopNoticesUi в конце Reload.
        var paintXaml = IsBorderDrawModeXaml();

        if (_isWin11)
            PaintPathBar.IsSeparateScope = paintXaml;

        PopulateEffectComboForCurrentPath();

        var effectSub = EffectRegSub();
        EffectRow.IsGateOn = FlyoutEffectRegistry.IsPresent(effectSub) || _effectGateArmed;
        _effectCombo!.SelectedIndex = paintXaml
            ? (int)Math.Clamp(ReadXamlEffectTypeLocalOrDefault(), 0u, 5u)
            : (int)FlyoutEffectRegistry.ReadUiIndex(RegSub);
        _effectCombo.IsEnabled = EffectRow.IsGateOn;
        if (FlyoutEffectRegistry.IsPresent(effectSub))
            _effectGateArmed = false;

        var colorSub = ColorRegSub();
        GradientExpander.IsGateOn = FlyoutThemeRegistry.IsGradientPresent(colorSub) || _gradientGateArmed;
        GradientPicker.SetColor(FlyoutThemeRegistry.ReadGradientArgbForUi(colorSub));
        if (FlyoutThemeRegistry.IsGradientPresent(colorSub))
            _gradientGateArmed = false;

        BorderExpander.IsGateOn = FlyoutThemeRegistry.IsBorderPresent(colorSub) || _borderGateArmed;
        BorderPicker.SetColor(FlyoutThemeRegistry.ReadBorderArgbForUi(colorSub, ThemeRegistry.TrayXamlBorderColorName()));
        if (FlyoutThemeRegistry.IsBorderPresent(colorSub))
            _borderGateArmed = false;
        _noBorderSwitch.IsOn = FlyoutPartRegistry.ReadNoBorderColorForUi(colorSub);

        if (_cornersSupported)
            CornerRow.UnsavedScope = SettingsCommitScope.ForFlyoutParameterGroup(
                CornerRegSub(), FlyoutParameterGroup.Corners);

        var cornerSub = CornerRegSub();
        // Только локальный ключ (GlassRegistry) — PartRegistry fallthrough давал ложный present.
        var cornerPresent = GlassRegistry.ReadDword(cornerSub, "CornerType").HasValue
            || _cornerGateArmed;
        CornerRow.IsGateOn = _cornersSupported && cornerPresent;
        if (GlassRegistry.ReadDword(cornerSub, "CornerType").HasValue)
            _cornerGateArmed = false;
        if (_cornersSupported && _cornerCombo is not null)
        {
            PopulateCornerComboForCurrentPath();
            var maxCorner = paintXaml ? 5u : 3u;
            var cornerVal = ReadCornerTypeLocalOrDefault(cornerSub, paintXaml);
            _cornerCombo.SelectedIndex = (int)Math.Clamp(cornerVal, 0u, maxCorner);
            _cornerCombo.IsEnabled = CornerRow.IsGateOn;
        }

        SyncSeparateZonesUi(paintXaml);
        SyncAdjacentWindowsRowUi(paintXaml);

        _noBorderSwitch.IsEnabled = BorderExpander.IsGateOn;
        BorderPicker.IsEnabled = BorderExpander.IsGateOn;

        ApplyExpanderUnsavedScopes();

        _loading = false;
        ApplyPageTopNoticesUi();
        UpdatePickerDefaultStates();
        RefreshPaintPathUnsavedIndicators();
    }

    private void SyncAdjacentWindowsRowUi(bool paintXaml)
    {
        AdjacentWindowsRow.Visibility =
            _isWin11 && paintXaml ? Visibility.Visible : Visibility.Collapsed;
    }

    private void AdjacentWindowsRow_OnNavigateRequested(object sender, EventArgs e)
    {
        SettingsNavigationService.Current.Push(
            typeof(TaskbarAdjacentWindowsPage),
            null,
            GlassSettingsResources.Get("Breadcrumb_AdjacentWindows"));
    }

    private void SyncSeparateZonesUi(bool paintXaml)
    {
        if (!_isWin11 || !paintXaml)
        {
            ZonesRow.Visibility = Visibility.Collapsed;
            ZonesRow.SetParallelBranchWarning(false, null);
            return;
        }

        ZonesRow.Visibility = Visibility.Visible;
        // Сегменты пишутся только при CornerType ≠ 0 (нет ключа = крупное = 2).
        // Тумблер остаётся включённым при CornerType=0 — попытка показать tip (раз за визит).
        var cornerType = ReadXamlCornerTypeLocalOrDefault();
        var zonesAllowed = cornerType != 0;
        ZonesRow.IsEnabled = true;
        _zonesSwitch.IsEnabled = true;
        var zonesOn = zonesAllowed
            && GlassRegistry.ReadDword(XamlRegSub, "SeparateZones") is { } zones
            && zones != 0;
        _zonesSwitch.IsOn = zonesOn;
        // Выкл — приглушённый спойлер (как gate OFF), тумблер остаётся ярким через StaticHost.
        ZonesRow.ApplyActiveVisual(zonesOn);
        var zonesLimit = GlassSettingsResources.Get("Flyout_ZonesCornersPolicyLimit");
        ZonesRow.SetParallelBranchWarning(!zonesAllowed, zonesLimit, zonesLimit);
    }

    /// <summary>XAML EffectType=5 («Системный») — градиент UI-заблокирован, ключи не удаляются.</summary>
    private bool IsXamlSystemBackdropPolicyActive() =>
        IsBorderDrawModeXaml() && ReadXamlEffectTypeLocalOrDefault() == 5;

    private void ApplyGradientBackdropPolicyUi()
    {
        if (IsXamlSystemBackdropPolicyActive())
        {
            var wasLoading = _loading;
            _loading = true;
            try
            {
                _gradientGateArmed = false;
                GradientExpander.IsGateOn = false;
            }
            finally
            {
                _loading = wasLoading;
            }

            var marker = GlassSettingsResources.Get("Flyout_GradientSystemBackdropLimit");
            GradientExpander.SetParallelBranchWarning(true, marker, marker);
            return;
        }

        GradientExpander.SetParallelBranchWarning(false, null);
        if (_loading)
            return;

        var present = FlyoutThemeRegistry.IsGradientPresent(ColorRegSub()) || _gradientGateArmed;
        if (GradientExpander.IsGateOn == present)
            return;

        _loading = true;
        try
        {
            GradientExpander.IsGateOn = present;
        }
        finally
        {
            _loading = false;
        }
    }

    /// <summary>Локальный CornerType; нет ключа → 2 (крупное скругление).</summary>
    private uint ReadXamlCornerTypeLocalOrDefault()
    {
        MigrateXamlCornerTypeSchemaIfNeeded();
        return GlassRegistry.ReadDword(XamlRegSub, "CornerType") ?? 2u;
    }

    /// <summary>
    /// Schema&lt;2: индекс 4 был MaxRound → 5. Schema≥2: 4 = Increased, только bump schema.
    /// </summary>
    private static void MigrateXamlCornerTypeSchemaIfNeeded()
    {
        var schema = GlassRegistry.ReadDword(XamlRegSub, "CornerTypeSchema") ?? 0u;
        if (schema >= 3u)
            return;

        // Только layout до Increased: 4 был MaxRound.
        if (schema < 2u && GlassRegistry.ReadDword(XamlRegSub, "CornerType") is 4u)
            GlassRegistry.WriteDword(XamlRegSub, "CornerType", 5u);

        GlassRegistry.WriteDword(XamlRegSub, "CornerTypeSchema", 3u);
    }

    private static uint ReadCornerTypeLocalOrDefault(string cornerSub, bool paintXaml)
    {
        if (paintXaml)
            MigrateXamlCornerTypeSchemaIfNeeded();
        if (GlassRegistry.ReadDword(cornerSub, "CornerType") is { } local)
            return local;
        if (paintXaml && GlassRegistry.ReadDword(RegSub, "CornerType") is { } legacy)
            return legacy;
        return 2u;
    }

    private void UpdatePickerDefaultStates()
    {
        if (_loading)
            return;

        var borderName = ThemeRegistry.TrayXamlBorderColorName();
        var colorSub = ColorRegSub();
        GradientPanel.SetDefaultColorActionState(
            GradientExpander.IsGateOn,
            FlyoutThemeRegistry.ShouldDisableGradientDefaultButton(colorSub));
        BorderPanel.SetDefaultColorActionState(
            BorderExpander.IsGateOn,
            FlyoutThemeRegistry.ShouldDisableBorderDefaultButton(colorSub, borderName));
    }

    private void OnGradientDefaultColor()
    {
        if (_loading || !GradientExpander.IsGateOn)
            return;
        // Только если ключ уже есть (кнопка иначе disabled).
        if (!FlyoutThemeRegistry.IsGradientPresent(ColorRegSub()))
            return;

        var def = FlyoutThemeRegistry.ResetGradientToDefault(ColorRegSub());
        GradientPicker.SetColor(def);
        SettingsSession.Current.MarkDirty();
        NotifyTaskbarVisualNow();
        UpdatePickerDefaultStates();
    }

    private void OnBorderDefaultColor()
    {
        if (_loading || !BorderExpander.IsGateOn)
            return;
        if (!FlyoutThemeRegistry.IsBorderPresent(ColorRegSub()))
            return;

        var def = FlyoutThemeRegistry.ResetBorderToDefault(ColorRegSub(), ThemeRegistry.TrayXamlBorderColorName());
        BorderPicker.SetColor(def);
        SettingsSession.Current.MarkDirty();
        FlyoutLiveReload.NotifyTaskbarBorderColorPreviewEnd();
        FlyoutLiveReload.NotifyTaskbarBorderModeNow();
        UpdatePickerDefaultStates();
    }

    private void ApplyPageTopNoticesUi()
    {
        var og = OpenGlassBorderPolicyCoordinator.ShouldShowBorderColorLimitNotice();
        var ogPipe = OpenGlassBorderPolicyCoordinator.IsOpenGlassAffectingBorders();
        OpenGlassTrayBorderNotice.Visibility = og ? Visibility.Visible : Visibility.Collapsed;
        DwmSystemBackdropNotice.Visibility = SwcaDwmBackdropNoticeCoordinator.ShouldShowGradientTintLimitNotice(RegSub)
            ? Visibility.Visible
            : Visibility.Collapsed;
        PaintPathBar.IsEnabled = _isWin11;
        // OG: только info-баннер, переключатель пути не блокируем.
        var paintXaml = IsBorderDrawModeXaml();
        SyncSeparateZonesUi(paintXaml);
        SyncAdjacentWindowsRowUi(paintXaml);
        ApplyGradientBackdropPolicyUi();
        UpdatePickerDefaultStates();
    }

    private void NoBorderSwitch_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        var value = _noBorderSwitch.IsOn ? 1u : 0u;
        // Политика: OFF = отсутствие ключа (не NoBorderColor=0).
        if (value == 0)
        {
            SettingsSession.Current.DeleteGlassValue(ColorRegSub(), "NoBorderColor");
            GlassRegistry.DeleteValue(ColorRegSub(), "NoBorderColor");
        }
        else
        {
            WriteColorDword("NoBorderColor", value);
        }
        FlyoutLiveReload.NotifyTaskbarBorderModeNow();
    }

    private void OnEffectChanged()
    {
        if (_loading || !EffectRow.IsGateOn)
            return;

        var index = (uint)_effectCombo!.SelectedIndex;
        var effectSub = EffectRegSub();
        if (FlyoutPartRegistry.TryReadLocalDword(effectSub, "EffectType", out var local))
        {
            if (local == index)
                return;
        }
        else if (!IsBorderDrawModeXaml() && FlyoutEffectRegistry.ReadUiIndex(RegSub) == index)
        {
            return;
        }

        SettingsSession.Current.WriteGlassDword(effectSub, "EffectType", index, liveFlyout: false);
        _effectGateArmed = false;
        SwcaDwmBackdropNoticeCoordinator.NotifyEffectOrScopeChanged();
        FlyoutLiveReload.NotifyTaskbarBorderModeNow();
        ApplyPageTopNoticesUi();
    }

    private void OnCornerTypeCommitted()
    {
        if (_cornerCombo is null || _cornerCombo.SelectedIndex < 0)
            return;

        var maxCorner = IsBorderDrawModeXaml() ? 5 : 3;
        var index = (uint)_cornerCombo.SelectedIndex;
        if (index > (uint)maxCorner)
            return;

        if (IsBorderDrawModeXaml())
        {
            SettingsSession.Current.WriteGlassDword(XamlRegSub, "CornerType", index, liveFlyout: false);
            // Schema 2: Increased=4, MaxRound=5 (миграция старого MaxRound=4).
            GlassRegistry.WriteDword(XamlRegSub, "CornerTypeSchema", 3);
            _cornerGateArmed = false;
            // «Системные»: без сегментов — удаляем ключ, не пишем 0.
            if (index == 0)
                SettingsSession.Current.DeleteGlassValue(XamlRegSub, "SeparateZones");
            SyncSeparateZonesUi(true);
            NotifyTaskbarVisualNow();
            return;
        }

        WriteTraySwcaDword("CornerType", index);
        _cornerGateArmed = false;
    }

    private async void PaintPathBar_OnScopeChanged(object sender, RoutedEventArgs e)
    {
        if (_loading)
            return;

        var toXaml = PaintPathBar.IsSeparateScope;
        var previousIndex = toXaml ? 0 : 1;
        var newIndex = toXaml ? 1 : 0;

        await PivotTabContentTransition.AnimateTabChangeAsync(
            ScopeContentHost,
            previousIndex,
            newIndex,
            () =>
            {
                WriteTrayDword("BorderDrawMode", toXaml ? 1u : 0u);
                // SWCA + XAML reapply (оба слоя).
                FlyoutLiveReload.NotifyTaskbarBorderModeNow();
                Reload();
            });
    }

    private void ZonesSwitch_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading || !IsBorderDrawModeXaml())
            return;
        if (!_zonesSwitch.IsEnabled)
            return;

        // CornerType=0 («Системные») — сегменты недоступны; tip один раз за визит.
        if (ReadXamlCornerTypeLocalOrDefault() == 0)
        {
            _loading = true;
            try
            {
                _zonesSwitch.IsOn = false;
            }
            finally
            {
                _loading = false;
            }
            ZonesRow.ApplyActiveVisual(false);
            _ = TryShowZonesDefaultCornersTipAsync();
            return;
        }

        if (_zonesSwitch.IsOn)
            SettingsSession.Current.WriteGlassDword(XamlRegSub, "SeparateZones", 1u, liveFlyout: false);
        else
            SettingsSession.Current.DeleteGlassValue(XamlRegSub, "SeparateZones");
        SettingsSession.Current.MarkDirty();
        FlyoutLiveReload.NotifyTaskbarBorderModeNow();
        ZonesRow.ApplyActiveVisual(_zonesSwitch.IsOn);
    }

    private async Task TryShowZonesDefaultCornersTipAsync()
    {
        if (_zonesDefaultCornersTipShownThisVisit)
            return;
        _zonesDefaultCornersTipShownThisVisit = true;

        var stack = PageShell.PageNoticeStack;
        var strip = stack.ActionNotice;
        strip.AllowDismiss = true;
        strip.ShowSaveButton = false;
        strip.ShowActionButton = false;
        strip.Visibility = Visibility.Collapsed;

        var host = SettingsNoticeCoordinator.CreateShellHost(stack, PageShell.PageScrollViewer);
        await SettingsNoticeCoordinator.PresentAsync(host, new SettingsNoticeCoordinator.NoticePresentRequest
        {
            Strip = strip,
            Severity = SettingsNoticeSeverity.Success,
            Message = GlassSettingsResources.Get("Flyout_ZonesNeedCornersTip"),
            ShowSave = false,
            ShowAction = false,
            Animate = true
        }).ConfigureAwait(true);
    }

    private void GradientGate_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        var colorSub = ColorRegSub();
        if (GradientExpander.IsGateOn)
        {
            if (IsXamlSystemBackdropPolicyActive())
            {
                _loading = true;
                try
                {
                    GradientExpander.IsGateOn = false;
                }
                finally
                {
                    _loading = false;
                }
                _gradientGateArmed = false;
                UpdatePickerDefaultStates();
                _ = TryShowGradientSystemBackdropTipAsync();
                return;
            }

            // Только UI: открыть спойлер/picker, без записи в реестр.
            _gradientGateArmed = true;
            GradientPicker.SetColor(FlyoutThemeRegistry.ReadGradientArgbForUi(colorSub));
            UpdatePickerDefaultStates();
            return;
        }

        _gradientGateArmed = false;
        FlyoutThemeRegistry.DeleteGradient(colorSub);
        SettingsSession.Current.MarkDirty();
        Reload();
        NotifyTaskbarVisualNow();
    }

    private async Task TryShowGradientSystemBackdropTipAsync()
    {
        if (_gradientSystemBackdropTipShownThisVisit)
            return;
        _gradientSystemBackdropTipShownThisVisit = true;

        var stack = PageShell.PageNoticeStack;
        var strip = stack.ActionNotice;
        strip.AllowDismiss = true;
        strip.ShowSaveButton = false;
        strip.ShowActionButton = false;
        strip.Visibility = Visibility.Collapsed;

        var host = SettingsNoticeCoordinator.CreateShellHost(stack, PageShell.PageScrollViewer);
        await SettingsNoticeCoordinator.PresentAsync(host, new SettingsNoticeCoordinator.NoticePresentRequest
        {
            Strip = strip,
            Severity = SettingsNoticeSeverity.Success,
            Message = GlassSettingsResources.Get("Flyout_GradientSystemBackdropTip"),
            ShowSave = false,
            ShowAction = false,
            Animate = true
        }).ConfigureAwait(true);
    }

    private void BorderGate_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        var colorSub = ColorRegSub();
        var borderName = ThemeRegistry.TrayXamlBorderColorName();
        if (BorderExpander.IsGateOn)
        {
            // Только UI: без записи ключа до явного изменения цвета пользователем.
            _borderGateArmed = true;
            BorderPicker.SetColor(FlyoutThemeRegistry.ReadBorderArgbForUi(colorSub, borderName));
            _noBorderSwitch.IsEnabled = true;
            BorderPicker.IsEnabled = true;
            SyncSeparateZonesUi(IsBorderDrawModeXaml());
            UpdatePickerDefaultStates();
            return;
        }

        _borderGateArmed = false;
        FlyoutThemeRegistry.DeleteBorder(colorSub);
        SettingsSession.Current.MarkDirty();
        Reload();
        FlyoutLiveReload.NotifyTaskbarBorderModeNow();
    }

    private void EffectGate_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        var effectSub = EffectRegSub();
        if (EffectRow.IsGateOn)
        {
            // Только UI: без EnsureEnabled/записи до смены комбо пользователем.
            _effectGateArmed = true;
            _effectCombo!.IsEnabled = true;
            _effectCombo.SelectedIndex = IsBorderDrawModeXaml()
                ? (int)Math.Clamp(ReadXamlEffectTypeLocalOrDefault(), 0u, 5u)
                : (int)FlyoutEffectRegistry.ReadUiIndex(RegSub);
            return;
        }

        _effectGateArmed = false;
        FlyoutEffectRegistry.Delete(effectSub);
        SettingsSession.Current.DeleteGlassValue(effectSub, FlyoutEffectRegistry.ValueName);
        GlassRegistry.DeleteValue(effectSub, FlyoutEffectRegistry.ValueName);
        SettingsSession.Current.MarkDirty();
        _loading = true;
        try
        {
            EffectRow.IsGateOn = false;
            _effectCombo!.IsEnabled = false;
        }
        finally
        {
            _loading = false;
        }
        FlyoutLiveReload.NotifyTaskbarBorderModeNow();
        ApplyPageTopNoticesUi();
    }

    private void CornerGate_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        var cornerSub = CornerRegSub();
        if (CornerRow.IsGateOn)
        {
            // Только UI: без Ensure/записи. Дефолт комбо = крупное (2).
            _cornerGateArmed = true;
            _loading = true;
            try
            {
                _cornerCombo!.IsEnabled = true;
                PopulateCornerComboForCurrentPath();
                var maxCorner = IsBorderDrawModeXaml() ? 5u : 3u;
                var ct = ReadCornerTypeLocalOrDefault(cornerSub, IsBorderDrawModeXaml());
                _cornerCombo.SelectedIndex = (int)Math.Clamp(ct, 0u, maxCorner);
            }
            finally
            {
                _loading = false;
            }
            SyncSeparateZonesUi(IsBorderDrawModeXaml());
            return;
        }

        _cornerGateArmed = false;
        GlassRegistry.DeleteValue(cornerSub, "CornerType");
        // Legacy: при XAML снять и старый Tray\CornerType, если остался.
        if (IsBorderDrawModeXaml())
            GlassRegistry.DeleteValue(RegSub, "CornerType");
        SettingsSession.Current.MarkDirty();
        Reload();
        FlyoutLiveReload.NotifyTaskbarBorderModeNow();
    }

    private void OnGradientColorCommitted(uint argb)
    {
        if (_loading) return;
        SettingsSession.Current.WriteGlassDword(ColorRegSub(), ThemeRegistry.TrayXamlGradientColorName(), argb, liveFlyout: false);
        _gradientGateArmed = false;
        NotifyTaskbarVisualNow();
        UpdatePickerDefaultStates();
    }

    private void OnBorderColorCommitted(uint argb)
    {
        if (_loading) return;
        SettingsSession.Current.WriteGlassDword(ColorRegSub(), ThemeRegistry.TrayXamlBorderColorName(), argb, liveFlyout: false);
        _borderGateArmed = false;
        FlyoutLiveReload.NotifyTaskbarBorderColorPreviewEnd();
        UpdatePickerDefaultStates();
    }

    private static bool IsBorderDrawModeXaml() =>
        FlyoutPartRegistry.ReadEffectiveDword(RegSub, "BorderDrawMode", 0) != 0;

    /// <summary>Локальный Tray\Xaml\EffectType; нет ключа → Acrylic (4).</summary>
    private static uint ReadXamlEffectTypeLocalOrDefault() =>
        GlassRegistry.ReadDword(XamlRegSub, "EffectType") ?? 4u;

    /// <summary>XAML path: TaskbarBoth; DWM: SWCA only.</summary>
    private void NotifyTaskbarVisualNow()
    {
        if (IsBorderDrawModeXaml())
            FlyoutLiveReload.NotifyTaskbarBorderModeNow();
        else
            FlyoutLiveReload.NotifyTaskbarBackdropNow();
    }

    private void WriteTraySwcaDword(string name, uint value)
    {
        SettingsSession.Current.WriteGlassDword(RegSub, name, value, liveFlyout: false);
        NotifyTaskbarVisualNow();
    }

    private void WriteColorDword(string name, uint value) =>
        SettingsSession.Current.WriteGlassDword(ColorRegSub(), name, value, liveFlyout: false);

    private void WriteTrayDword(string name, uint value) =>
        SettingsSession.Current.WriteGlassDword(RegSub, name, value, liveFlyout: false);
}
