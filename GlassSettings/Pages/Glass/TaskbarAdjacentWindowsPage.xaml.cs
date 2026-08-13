using System.Threading.Tasks;
using GlassSettings.Controls;
using GlassSettings.Core.Customization;
using GlassSettings.Core.Flyout;
using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Settings;
using GlassSettings.Core.Snapshot;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;

namespace GlassSettings.Pages.Glass;

/// <summary>
/// Смежные окна XAML-пути: Tray\Xaml\{Popup|Preview|Menu|Tooltip}.
/// Нет локального value → runtime fallback на Tray\Xaml (панель).
/// </summary>
public sealed partial class TaskbarAdjacentWindowsPage : Page
{
    private const string BarXamlSub = "Tray\\Xaml";

    private static readonly string[] SurfaceSubs =
    [
        "Tray\\Xaml\\Popup",
        "Tray\\Xaml\\Preview",
        "Tray\\Xaml\\Menu",
        "Tray\\Xaml\\Tooltip",
    ];

    private static readonly string[] PivotHeaderKeys =
    [
        "Glass_Adjacent_TabPopup",
        "Glass_Adjacent_TabPreview",
        "Glass_Adjacent_TabMenu",
        "Glass_Adjacent_TabTooltip",
    ];

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
    private readonly SettingsPivotTabHeader?[] _pivotHeaders = new SettingsPivotTabHeader?[4];
    private bool _loading;
    private bool _effectGateArmed;
    private bool _gradientGateArmed;
    private bool _borderGateArmed;
    private bool _cornerGateArmed;
    private bool _gradientSystemBackdropTipShownThisVisit;
    private int _lastPivotIndex;
    private bool _lastIsDarkMode;
    private EventHandler<DesktopEnvironmentChangedEventArgs>? _environmentChangedHandler;
    private readonly bool _cornersSupported = WindowsOsHelper.IsWindows11OrGreater();

    private string RegSub => SurfaceSubs[Math.Clamp(SurfacePivot.SelectedIndex, 0, SurfaceSubs.Length - 1)];

    public TaskbarAdjacentWindowsPage()
    {
        InitializeComponent();
        BuildNoBorderFooter();
        ApplyLocalizedStrings();
        GradientExpander.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.Gradient);
        BorderExpander.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.Border);
        EffectRow.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.EffectType);
        CornerRow.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.Corners);

        if (!_cornersSupported)
            CornerRow.Visibility = Visibility.Collapsed;

        _effectCombo = new ComboBox { MinWidth = 200 };
        FlyoutXamlEffectNames.PopulateCombo(_effectCombo);
        SettingsInteractionDeferral.WhenComboCommit(
            _effectCombo,
            () => !_loading && EffectRow.IsGateOn,
            OnEffectChanged);
        EffectRow.HeaderActions = _effectCombo;

        if (_cornersSupported)
        {
            _cornerCombo = new ComboBox { MinWidth = 200 };
            FlyoutCornerNames.PopulateCombo(_cornerCombo, xamlPath: true);
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
            FlyoutLiveReload.ScheduleTaskbarGradientPreview(RegSub, argb);
        };
        BorderPicker.ColorLivePreview += (_, argb) =>
        {
            UpdatePickerDefaultStates();
            FlyoutLiveReload.ScheduleTaskbarBorderColorPreview(RegSub, argb);
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
            SettingsSession.Current.DirtyChanged += OnSessionDirtyChanged;
            FlyoutThemeRegistry.PurgeStaleCachedSystemSamples();
            ApplyExpanderUnsavedScopes();
            _lastIsDarkMode = ThemeRegistry.IsTrayXamlDarkMode();
            HookEnvironmentWatcher();
            Reload();
        };
        Unloaded += (_, _) =>
        {
            SettingsSession.Current.DirtyChanged -= OnSessionDirtyChanged;
            UnhookEnvironmentWatcher();
        };
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        _gradientSystemBackdropTipShownThisVisit = false;
        _lastIsDarkMode = ThemeRegistry.IsTrayXamlDarkMode();
        ApplyLocalizedStrings();
        ApplyExpanderUnsavedScopes();
        Reload();
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
            return;

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
        Reload();
    }

    private void OnSessionDirtyChanged(object? sender, EventArgs e) =>
        DispatcherQueue.TryEnqueue(() =>
        {
            ApplyExpanderUnsavedScopes();
            RefreshPivotUnsavedIndicators();
        });

    private void RefreshPivotUnsavedIndicators()
    {
        for (var i = 0; i < _pivotHeaders.Length && i < SurfaceSubs.Length; i++)
        {
            if (_pivotHeaders[i] is not { } header)
                continue;
            var pending = SettingsSession.Current.HasUnsavedInScope(
                TaskbarScopeCatalog.AdjacentSurfaceScope(SurfaceSubs[i]));
            header.SetUnsavedVisible(pending);
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
        BorderExpander.WireExpandedLayoutRefresh(() => BorderPanel.RefreshLayout());
    }

    private void ApplyLocalizedStrings()
    {
        for (var i = 0; i < SurfacePivot.Items.Count && i < PivotHeaderKeys.Length; i++)
        {
            if (SurfacePivot.Items[i] is not PivotItem item)
                continue;
            var label = GlassSettingsResources.Get(PivotHeaderKeys[i]);
            if (_pivotHeaders[i] is { } existing)
            {
                existing.SetLabel(label);
                item.Header = existing.Root;
            }
            else
            {
                var header = SettingsPivotTabHeader.Create(label);
                _pivotHeaders[i] = header;
                item.Header = header.Root;
            }
        }

        GradientExpander.Title = GlassSettingsResources.Get("Flyout_GradientColor");
        GradientExpander.Description = GlassSettingsResources.Get("Flyout_GradientColorSubtitle");
        GradientPanel.Description = GlassSettingsResources.Get("Flyout_GradientColorDesc");

        BorderExpander.Title = GlassSettingsResources.Get("Flyout_BorderColor");
        BorderExpander.Description = GlassSettingsResources.Get("Flyout_BorderColorSubtitle");
        BorderPanel.Description = GlassSettingsResources.Get("Flyout_BorderColorDesc");
        _noBorderLabel.Text = GlassSettingsResources.Get("Flyout_NoBorder");

        EffectRow.Title = GlassSettingsResources.Get("Flyout_EffectType");
        EffectRow.Description = GlassSettingsResources.Get("Glass_Adjacent_EffectDesc");

        if (_cornersSupported)
        {
            CornerRow.Title = GlassSettingsResources.Get("Flyout_CornerType");
            CornerRow.Description = GlassSettingsResources.Get("Flyout_CornerTypeDesc");
            _cornerInlineLabel.Label = GlassSettingsResources.Get("ParamLabel_CornerType");
        }
    }

    private void ApplyExpanderUnsavedScopes()
    {
        var sub = RegSub;
        EffectRow.UnsavedScope = SettingsCommitScope.ForFlyoutParameterGroup(
            sub, FlyoutParameterGroup.Effect);
        GradientExpander.UnsavedScope = SettingsCommitScope.ForFlyoutParameterGroup(
            sub, FlyoutParameterGroup.Gradient);
        BorderExpander.UnsavedScope = SettingsCommitScope.ForFlyoutParameterGroup(
            sub, FlyoutParameterGroup.Border);
        if (_cornersSupported)
            CornerRow.UnsavedScope = SettingsCommitScope.ForFlyoutParameterGroup(
                sub, FlyoutParameterGroup.Corners);
    }

    private async void SurfacePivot_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || !IsLoaded)
            return;

        var newIndex = SurfacePivot.SelectedIndex;
        if (newIndex < 0 || newIndex == _lastPivotIndex)
            return;

        var previous = _lastPivotIndex;
        await PivotTabContentTransition.AnimateTabChangeAsync(
            TabContentHost,
            previous,
            newIndex,
            () =>
            {
                _lastPivotIndex = newIndex;
                _effectGateArmed = false;
                _gradientGateArmed = false;
                _borderGateArmed = false;
                _cornerGateArmed = false;
                ApplyLocalizedStrings();
                ApplyExpanderUnsavedScopes();
                Reload();
            });
    }

    private void Reload()
    {
        _loading = true;
        var sub = RegSub;

        EffectRow.IsGateOn = FlyoutEffectRegistry.IsPresent(sub) || _effectGateArmed;
        _effectCombo!.SelectedIndex = (int)Math.Clamp(ReadEffectTypeEffective(sub), 0u, 5u);
        _effectCombo.IsEnabled = EffectRow.IsGateOn;
        if (FlyoutEffectRegistry.IsPresent(sub))
            _effectGateArmed = false;

        GradientExpander.IsGateOn = FlyoutThemeRegistry.IsGradientPresent(sub) || _gradientGateArmed;
        GradientPicker.SetColor(FlyoutThemeRegistry.ReadGradientArgbForUi(sub));
        if (FlyoutThemeRegistry.IsGradientPresent(sub))
            _gradientGateArmed = false;

        BorderExpander.IsGateOn = FlyoutThemeRegistry.IsBorderPresent(sub) || _borderGateArmed;
        BorderPicker.SetColor(FlyoutThemeRegistry.ReadBorderArgbForUi(sub, ThemeRegistry.TrayXamlBorderColorName()));
        if (FlyoutThemeRegistry.IsBorderPresent(sub))
            _borderGateArmed = false;
        _noBorderSwitch.IsOn = ReadNoBorderEffective(sub);

        SyncCornerRowUi(sub);

        _noBorderSwitch.IsEnabled = BorderExpander.IsGateOn;
        BorderPicker.IsEnabled = BorderExpander.IsGateOn;

        ApplyExpanderUnsavedScopes();
        _loading = false;
        ApplyGradientBackdropPolicyUi();
        UpdatePickerDefaultStates();
        RefreshPivotUnsavedIndicators();
    }

    private void SyncCornerRowUi(string sub)
    {
        if (!_cornersSupported || _cornerCombo is null)
            return;

        CornerRow.IsEnabled = true;
        CornerRow.GateSwitchControl.IsEnabled = true;
        _cornerInlineLabel.IsEnabled = true;
        var cornerPresent = GlassRegistry.ReadDword(sub, "CornerType").HasValue || _cornerGateArmed;
        CornerRow.IsGateOn = cornerPresent;
        if (GlassRegistry.ReadDword(sub, "CornerType").HasValue)
            _cornerGateArmed = false;
        FlyoutCornerNames.PopulateCombo(_cornerCombo, xamlPath: true);
        var cornerVal = ReadCornerTypeEffective(sub);
        _cornerCombo.SelectedIndex = (int)Math.Clamp(cornerVal, 0u, 5u);
        _cornerCombo.IsEnabled = CornerRow.IsGateOn;
    }

    private static uint ReadEffectTypeEffective(string sub) =>
        GlassRegistry.ReadDword(sub, "EffectType")
        ?? ReadEffectTypeFromBar();

    private static uint ReadEffectTypeFromBar() =>
        GlassRegistry.ReadDword(BarXamlSub, "EffectType") ?? 4u;

    private static uint ReadCornerTypeEffective(string sub)
    {
        MigrateBarCornerSchemaIfNeeded();
        return GlassRegistry.ReadDword(sub, "CornerType")
            ?? ReadCornerTypeFromBar();
    }

    private static uint ReadCornerTypeFromBar()
    {
        MigrateBarCornerSchemaIfNeeded();
        return GlassRegistry.ReadDword(BarXamlSub, "CornerType") ?? 2u;
    }

    private static bool ReadNoBorderEffective(string sub)
    {
        if (GlassRegistry.ReadDword(sub, "NoBorderColor") is { } local)
            return local != 0;
        return ReadNoBorderFromBar();
    }

    private static bool ReadNoBorderFromBar() =>
        GlassRegistry.ReadDword(BarXamlSub, "NoBorderColor") is { } bar && bar != 0;

    private static void MigrateBarCornerSchemaIfNeeded()
    {
        var schema = GlassRegistry.ReadDword(BarXamlSub, "CornerTypeSchema") ?? 0u;
        if (schema >= 3u)
            return;

        var cornerMigrated = false;
        if (schema < 2u && GlassRegistry.ReadDword(BarXamlSub, "CornerType") is 4u)
        {
            GlassRegistry.WriteDword(BarXamlSub, "CornerType", 5u);
            cornerMigrated = true;
        }

        // Schema вне snapshot dirty (GlassNeverSnapshotValueNames).
        GlassRegistry.WriteDword(BarXamlSub, "CornerTypeSchema", 3u);

        // Тихая миграция CornerType не должна оставлять вечный yellow notice.
        if (cornerMigrated)
            SettingsSession.Current.AbsorbGlassBaseline(BarXamlSub, "CornerType");
    }

    /// <summary>
    /// Surface: baseline absent + значение = bar/default → Delete (inherit), иначе Write.
    /// Иначе откат комбо «к виду до правки» оставляет локальный ключ и notice не гаснет.
    /// </summary>
    private static void WriteOrInheritSurfaceDword(string sub, string valueName, uint value, uint inheritValue)
    {
        var baselineAbsent = !SettingsSession.Current.HasGlassSnapshotValue(sub, valueName);
        if (baselineAbsent && value == inheritValue)
        {
            SettingsSession.Current.DeleteGlassValue(sub, valueName);
            return;
        }

        SettingsSession.Current.WriteGlassDword(sub, valueName, value, liveFlyout: false);
    }

    private bool IsSystemBackdropPolicyActive() =>
        ReadEffectTypeEffective(RegSub) == 5;

    private void ApplyGradientBackdropPolicyUi()
    {
        if (IsSystemBackdropPolicyActive() && EffectRow.IsGateOn)
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
    }

    private void UpdatePickerDefaultStates()
    {
        if (_loading)
            return;
        var sub = RegSub;
        var borderName = ThemeRegistry.TrayXamlBorderColorName();
        GradientPanel.SetDefaultColorActionState(
            GradientExpander.IsGateOn,
            FlyoutThemeRegistry.ShouldDisableGradientDefaultButton(sub));
        BorderPanel.SetDefaultColorActionState(
            BorderExpander.IsGateOn,
            FlyoutThemeRegistry.ShouldDisableBorderDefaultButton(sub, borderName));
    }

    private void NotifyVisualNow() => FlyoutLiveReload.NotifyTaskbarBorderModeNow();

    private void NoBorderSwitch_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        var value = _noBorderSwitch.IsOn ? 1u : 0u;
        var inherit = ReadNoBorderFromBar() ? 1u : 0u;
        WriteOrInheritSurfaceDword(RegSub, "NoBorderColor", value, inherit);
        NotifyVisualNow();
    }

    private void OnEffectChanged()
    {
        if (_loading || !EffectRow.IsGateOn)
            return;

        var index = (uint)_effectCombo!.SelectedIndex;
        var sub = RegSub;
        if (FlyoutPartRegistry.TryReadLocalDword(sub, "EffectType", out var local) && local == index)
            return;

        WriteOrInheritSurfaceDword(sub, "EffectType", index, ReadEffectTypeFromBar());
        _effectGateArmed = false;
        NotifyVisualNow();
        ApplyGradientBackdropPolicyUi();
        UpdatePickerDefaultStates();
    }

    private void OnCornerTypeCommitted()
    {
        if (_cornerCombo is null || _cornerCombo.SelectedIndex < 0)
            return;

        var index = (uint)_cornerCombo.SelectedIndex;
        if (index > 5u)
            return;

        var sub = RegSub;
        WriteOrInheritSurfaceDword(sub, "CornerType", index, ReadCornerTypeFromBar());
        // Schema вне snapshot dirty (см. RegistryPaths.GlassNeverSnapshotValueNames).
        GlassRegistry.WriteDword(sub, "CornerTypeSchema", 3);
        _cornerGateArmed = false;
        NotifyVisualNow();
    }

    private void EffectGate_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        var sub = RegSub;
        if (EffectRow.IsGateOn)
        {
            _effectGateArmed = true;
            _effectCombo!.IsEnabled = true;
            _effectCombo.SelectedIndex = (int)Math.Clamp(ReadEffectTypeEffective(sub), 0u, 5u);
            return;
        }

        _effectGateArmed = false;
        FlyoutEffectRegistry.Delete(sub);
        SettingsSession.Current.DeleteGlassValue(sub, FlyoutEffectRegistry.ValueName);
        GlassRegistry.DeleteValue(sub, FlyoutEffectRegistry.ValueName);
        SettingsSession.Current.MarkDirty();
        _loading = true;
        try
        {
            EffectRow.IsGateOn = false;
            _effectCombo!.IsEnabled = false;
            _effectCombo.SelectedIndex = (int)Math.Clamp(ReadEffectTypeEffective(sub), 0u, 5u);
        }
        finally
        {
            _loading = false;
        }
        NotifyVisualNow();
        ApplyGradientBackdropPolicyUi();
    }

    private void GradientGate_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        var sub = RegSub;
        if (GradientExpander.IsGateOn)
        {
            if (IsSystemBackdropPolicyActive() && EffectRow.IsGateOn)
            {
                _loading = true;
                try { GradientExpander.IsGateOn = false; }
                finally { _loading = false; }
                _gradientGateArmed = false;
                UpdatePickerDefaultStates();
                _ = TryShowGradientSystemBackdropTipAsync();
                return;
            }

            _gradientGateArmed = true;
            GradientPicker.SetColor(FlyoutThemeRegistry.ReadGradientArgbForUi(sub));
            UpdatePickerDefaultStates();
            return;
        }

        _gradientGateArmed = false;
        FlyoutThemeRegistry.DeleteGradient(sub);
        SettingsSession.Current.MarkDirty();
        Reload();
        NotifyVisualNow();
    }

    private void BorderGate_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        var sub = RegSub;
        var borderName = ThemeRegistry.TrayXamlBorderColorName();
        if (BorderExpander.IsGateOn)
        {
            _borderGateArmed = true;
            BorderPicker.SetColor(FlyoutThemeRegistry.ReadBorderArgbForUi(sub, borderName));
            _noBorderSwitch.IsEnabled = true;
            BorderPicker.IsEnabled = true;
            UpdatePickerDefaultStates();
            return;
        }

        _borderGateArmed = false;
        FlyoutThemeRegistry.DeleteBorder(sub);
        SettingsSession.Current.DeleteGlassValue(sub, "NoBorderColor");
        GlassRegistry.DeleteValue(sub, "NoBorderColor");
        SettingsSession.Current.MarkDirty();
        Reload();
        NotifyVisualNow();
    }

    private void CornerGate_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        var sub = RegSub;
        if (CornerRow.IsGateOn)
        {
            _cornerGateArmed = true;
            _loading = true;
            try
            {
                _cornerCombo!.IsEnabled = true;
                FlyoutCornerNames.PopulateCombo(_cornerCombo, xamlPath: true);
                _cornerCombo.SelectedIndex = (int)Math.Clamp(ReadCornerTypeEffective(sub), 0u, 5u);
            }
            finally
            {
                _loading = false;
            }
            return;
        }

        _cornerGateArmed = false;
        SettingsSession.Current.DeleteGlassValue(sub, "CornerType");
        Reload();
        NotifyVisualNow();
    }

    private void OnGradientColorCommitted(uint argb)
    {
        if (_loading) return;
        var name = ThemeRegistry.TrayXamlGradientColorName();
        var inherit = FlyoutThemeRegistry.ReadGradientArgbForUi(BarXamlSub);
        WriteOrInheritSurfaceDword(RegSub, name, argb, inherit);
        _gradientGateArmed = false;
        NotifyVisualNow();
        UpdatePickerDefaultStates();
    }

    private void OnBorderColorCommitted(uint argb)
    {
        if (_loading) return;
        var name = ThemeRegistry.TrayXamlBorderColorName();
        var inherit = FlyoutThemeRegistry.ReadBorderArgbForUi(BarXamlSub, name);
        WriteOrInheritSurfaceDword(RegSub, name, argb, inherit);
        _borderGateArmed = false;
        FlyoutLiveReload.NotifyTaskbarBorderColorPreviewEnd();
        UpdatePickerDefaultStates();
    }

    private void OnGradientDefaultColor()
    {
        if (_loading || !GradientExpander.IsGateOn)
            return;
        if (!FlyoutThemeRegistry.IsGradientPresent(RegSub))
            return;

        var def = FlyoutThemeRegistry.ResetGradientToDefault(RegSub);
        GradientPicker.SetColor(def);
        SettingsSession.Current.MarkDirty();
        NotifyVisualNow();
        UpdatePickerDefaultStates();
    }

    private void OnBorderDefaultColor()
    {
        if (_loading || !BorderExpander.IsGateOn)
            return;
        if (!FlyoutThemeRegistry.IsBorderPresent(RegSub))
            return;

        var def = FlyoutThemeRegistry.ResetBorderToDefault(RegSub, ThemeRegistry.TrayXamlBorderColorName());
        BorderPicker.SetColor(def);
        SettingsSession.Current.MarkDirty();
        FlyoutLiveReload.NotifyTaskbarBorderColorPreviewEnd();
        NotifyVisualNow();
        UpdatePickerDefaultStates();
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
}
