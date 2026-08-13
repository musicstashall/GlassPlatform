using GlassSettings.Controls;
using GlassSettings.Core.Registry;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;
using Windows.UI;

namespace GlassSettings.Pages;

public sealed partial class ThemesColorsPage : Page
{
    private bool _loading;
    private bool _accentApplyBusy;
    private ComboBox? _modeChoiceCombo;
    private ComboBox? _accentModeCombo;
    private ComboBox? _windowsModeCombo;
    private ComboBox? _appsModeCombo;
    private TextBlock? _windowsModeLabel;
    private TextBlock? _appsModeLabel;
    private TextBlock? _startTaskbarPrevalenceLabel;
    private TextBlock? _titleBarsPrevalenceLabel;
    private TextBlock? _recentColorsLabel;
    private StackPanel? _recentColorsStrip;
    private StackPanel? _recentColorsTiles;
    private ToggleSwitch? _startTaskbarPrevalenceToggle;
    private ToggleSwitch? _titleBarsPrevalenceToggle;
    private readonly List<AccentRecentColorSwatch> _recentColorSwatches = [];
    private EventHandler<DesktopEnvironmentChangedEventArgs>? _environmentChangedHandler;
    private DispatcherQueueTimer? _accentDefaultRefreshTimer;
    private string? _watchedThemePath;
    private bool _pendingThemeAccentBaselineCapture;

    public ThemesColorsPage()
    {
        InitializeComponent();
        BuildHeaderCombos();
        BuildModeParameterBody();
        BuildAccentFooter();
        Unloaded += OnUnloaded;
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        ApplyLocalizedStrings();
        HookEnvironmentWatcher();
        ReloadFromSystem();
    }

    protected override void OnNavigatedFrom(NavigationEventArgs e)
    {
        UnhookEnvironmentWatcher();
        base.OnNavigatedFrom(e);
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        if (_accentDefaultRefreshTimer is not null)
        {
            _accentDefaultRefreshTimer.Stop();
            _accentDefaultRefreshTimer.Tick -= AccentDefaultRefreshTimer_OnTick;
        }

        UnhookEnvironmentWatcher();
    }

    private void BuildHeaderCombos()
    {
        _modeChoiceCombo = new ComboBox { MinWidth = 160 };
        _modeChoiceCombo.SelectionChanged += ModeChoiceCombo_OnSelectionChanged;
        ModeExpander.HeaderActions = _modeChoiceCombo;

        _accentModeCombo = new ComboBox { MinWidth = 160 };
        _accentModeCombo.SelectionChanged += AccentModeCombo_OnSelectionChanged;
        AccentExpander.HeaderActions = _accentModeCombo;
    }

    private void BuildModeParameterBody()
    {
        // Паритет системной Персонализации: без body-заголовка, строки ~50,
        // combo справа вровень с combo шапки (ShowGateSwitch=false → combo в StaticHost).
        _windowsModeLabel = new TextBlock
        {
            VerticalAlignment = VerticalAlignment.Center,
            Style = (Style)Application.Current.Resources["BodyTextBlockStyle"],
            TextWrapping = TextWrapping.WrapWholeWords
        };
        _appsModeLabel = new TextBlock
        {
            VerticalAlignment = VerticalAlignment.Center,
            Style = (Style)Application.Current.Resources["BodyTextBlockStyle"],
            TextWrapping = TextWrapping.WrapWholeWords
        };

        _windowsModeCombo = new ComboBox
        {
            MinWidth = 160,
            HorizontalAlignment = HorizontalAlignment.Right,
            VerticalAlignment = VerticalAlignment.Center
        };
        _appsModeCombo = new ComboBox
        {
            MinWidth = 160,
            HorizontalAlignment = HorizontalAlignment.Right,
            VerticalAlignment = VerticalAlignment.Center
        };
        _windowsModeCombo.SelectionChanged += WindowsModeCombo_OnSelectionChanged;
        _appsModeCombo.SelectionChanged += AppsModeCombo_OnSelectionChanged;

        const double rowMinHeight = 50;
        var stack = new StackPanel
        {
            HorizontalAlignment = HorizontalAlignment.Stretch,
            Spacing = 0
        };
        stack.Children.Add(CreateModeSettingRow(_windowsModeLabel, _windowsModeCombo, rowMinHeight));
        // На всю ширину контента спойлера (компенсация BodyHost Margin 28/16).
        stack.Children.Add(new Border
        {
            Height = 1,
            Margin = new Thickness(-28, 0, -16, 0),
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Center,
            Background = Application.Current.Resources["DividerStrokeColorDefaultBrush"] as Brush
        });
        stack.Children.Add(CreateModeSettingRow(_appsModeLabel, _appsModeCombo, rowMinHeight));

        ModeParameterPanel.Description = string.Empty;
        ModeParameterPanel.BodyContent = stack;
        ModeParameterPanel.SetDefaultActionState(showDefaultAction: false, disableDefaultButton: true);

        ModeExpander.Loaded += (_, _) => SyncModeBodyComboAlignment();
        ModeExpander.SizeChanged += (_, _) => SyncModeBodyComboAlignment();
        ModeExpander.WireExpandedLayoutRefresh(SyncModeBodyComboAlignment);
    }

    private void BuildAccentFooter()
    {
        // Footer: палитра сверху, spacer *, prevalence-тумблеры у нижнего края (над Default).
        var labelStyle = Application.Current.Resources["GlassSettingsSpoilerSecondaryTextStyle"] as Style;
        _recentColorsLabel = new TextBlock
        {
            Style = labelStyle,
            TextWrapping = TextWrapping.WrapWholeWords,
            Margin = new Thickness(0, 0, 0, 4)
        };
        _recentColorsTiles = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 0
        };
        _recentColorsStrip = new StackPanel
        {
            Orientation = Orientation.Vertical,
            Spacing = 0,
            HorizontalAlignment = HorizontalAlignment.Left,
            VerticalAlignment = VerticalAlignment.Top
        };
        _recentColorsStrip.Children.Add(_recentColorsLabel);
        _recentColorsStrip.Children.Add(_recentColorsTiles);

        _startTaskbarPrevalenceLabel = new TextBlock
        {
            Style = labelStyle,
            TextWrapping = TextWrapping.WrapWholeWords,
            VerticalAlignment = VerticalAlignment.Center
        };
        _titleBarsPrevalenceLabel = new TextBlock
        {
            Style = labelStyle,
            TextWrapping = TextWrapping.WrapWholeWords,
            VerticalAlignment = VerticalAlignment.Center
        };

        var toggleStyle = Application.Current.Resources["GlassSettingsCompactToggleSwitchStyle"] as Style;
        _startTaskbarPrevalenceToggle = CreateCompactFooterToggle(toggleStyle);
        _titleBarsPrevalenceToggle = CreateCompactFooterToggle(toggleStyle);
        _startTaskbarPrevalenceToggle.Toggled += StartTaskbarPrevalenceToggle_OnToggled;
        _titleBarsPrevalenceToggle.Toggled += TitleBarsPrevalenceToggle_OnToggled;

        var table = SettingsPickerBodyLayout.CreateBodyTable();
        table.HorizontalAlignment = HorizontalAlignment.Stretch;
        table.VerticalAlignment = VerticalAlignment.Stretch;
        table.RowDefinitions.Clear();
        table.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        table.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        table.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        table.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        Grid.SetRow(_recentColorsStrip, 0);
        Grid.SetColumn(_recentColorsStrip, 0);
        Grid.SetColumnSpan(_recentColorsStrip, 3);
        table.Children.Add(_recentColorsStrip);

        SettingsPickerBodyLayout.PlaceLabeledRow(
            table, row: 2, _startTaskbarPrevalenceLabel, _startTaskbarPrevalenceToggle, topGap: 2);
        SettingsPickerBodyLayout.PlaceLabeledRow(
            table, row: 3, _titleBarsPrevalenceLabel, _titleBarsPrevalenceToggle, topGap: 2);

        AccentColorPanel.FooterContent = table;

        AccentExpander.WireExpandedLayoutRefresh(() => AccentColorPanel.RefreshLayout());
    }

    private void RefreshRecentAccentPalette()
    {
        if (_recentColorsStrip is null || _recentColorsTiles is null)
            return;

        var colors = WindowsPersonalizationService.ReadRecentAccentColors();
        var selected = WindowsPersonalizationService.ResolveRecentPaletteSelection(colors);

        _recentColorSwatches.Clear();
        _recentColorsTiles.Children.Clear();

        if (colors.Count == 0)
        {
            _recentColorsStrip.Visibility = Visibility.Collapsed;
            return;
        }

        _recentColorsStrip.Visibility = Visibility.Visible;
        foreach (var color in colors)
        {
            var swatch = new AccentRecentColorSwatch
            {
                SwatchColor = color,
                IsSelected = selected is { } s && ThemeAccentColorHelper.RgbEquals(color, s)
            };
            swatch.Clicked += RecentColorSwatch_OnClicked;
            _recentColorSwatches.Add(swatch);
            _recentColorsTiles.Children.Add(swatch);
        }
    }

    private void RecentColorSwatch_OnClicked(object? sender, EventArgs e)
    {
        if (_loading || _accentApplyBusy || sender is not AccentRecentColorSwatch swatch)
            return;

        _loading = true;
        try
        {
            if (_accentModeCombo is not null
                && _accentModeCombo.SelectedIndex
                    != (int)WindowsPersonalizationService.AccentModeChoice.Manual)
            {
                _accentModeCombo.SelectedIndex =
                    (int)WindowsPersonalizationService.AccentModeChoice.Manual;
            }

            ApplyAccentExpandState(true);
            AccentColorPicker.SetColor(ThemeAccentColorHelper.ToArgbDword(swatch.SwatchColor));
            foreach (var item in _recentColorSwatches)
                item.IsSelected = ReferenceEquals(item, swatch);
        }
        finally
        {
            _loading = false;
        }

        _ = ApplyAccentAsync(swatch.SwatchColor);
    }

    private static ToggleSwitch CreateCompactFooterToggle(Style? style) => new()
    {
        OnContent = string.Empty,
        OffContent = string.Empty,
        MinWidth = 0,
        HorizontalAlignment = HorizontalAlignment.Left,
        VerticalAlignment = VerticalAlignment.Center,
        Style = style
    };

    /// <summary>
    /// BodyHost.Margin.Right=16 совпадает с HeaderPadding; дополнительно —
    /// spacing+chevron справа от header combo (он в StaticHost при ShowGateSwitch=false).
    /// </summary>
    private void SyncModeBodyComboAlignment()
    {
        if (_windowsModeCombo is null || _appsModeCombo is null)
            return;

        var inset = ModeExpander.MeasureHeaderActionsBodyAlignInset();
        var margin = new Thickness(0, 0, inset, 0);
        if (_windowsModeCombo.Margin != margin)
            _windowsModeCombo.Margin = margin;
        if (_appsModeCombo.Margin != margin)
            _appsModeCombo.Margin = margin;
    }

    private static Grid CreateModeSettingRow(TextBlock label, ComboBox combo, double minHeight)
    {
        var grid = new Grid
        {
            MinHeight = minHeight,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Center
        };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        label.HorizontalAlignment = HorizontalAlignment.Left;
        label.VerticalAlignment = VerticalAlignment.Center;
        combo.VerticalAlignment = VerticalAlignment.Center;

        Grid.SetColumn(label, 0);
        Grid.SetColumn(combo, 1);
        grid.Children.Add(label);
        grid.Children.Add(combo);
        return grid;
    }

    private void ApplyLocalizedStrings()
    {
        ModeExpander.Title = GlassSettingsResources.Get("Themes_Colors_ModeTitle");
        ModeExpander.Description = GlassSettingsResources.Get("Themes_Colors_ModeDesc");
        ModeExpander.IconGlyph = SettingsSectionGlyphs.Theme;
        // Body без призыва — паритет окна Персонализации (только две строки режимов).
        ModeParameterPanel.Description = string.Empty;

        TransparencyRow.Title = GlassSettingsResources.Get("Themes_Colors_TransparencyTitle");
        TransparencyRow.Description = GlassSettingsResources.Get("Themes_Colors_TransparencyDesc");
        TransparencyRow.IconGlyph = SettingsSectionGlyphs.Colorization;

        AccentExpander.Title = GlassSettingsResources.Get("Themes_Colors_AccentTitle");
        AccentExpander.Description = GlassSettingsResources.Get("Themes_Colors_AccentDesc");
        AccentExpander.IconGlyph = SettingsSectionGlyphs.Colorization;
        AccentColorPanel.Description = GlassSettingsResources.Get("Themes_Colors_AccentBodyDesc");

        if (_windowsModeLabel is not null)
            _windowsModeLabel.Text = GlassSettingsResources.Get("Themes_Colors_WindowsMode");
        if (_appsModeLabel is not null)
            _appsModeLabel.Text = GlassSettingsResources.Get("Themes_Colors_AppsMode");
        if (_startTaskbarPrevalenceLabel is not null)
            _startTaskbarPrevalenceLabel.Text = GlassSettingsResources.Get("Themes_Colors_AccentStartTaskbar");
        if (_titleBarsPrevalenceLabel is not null)
            _titleBarsPrevalenceLabel.Text = GlassSettingsResources.Get("Themes_Colors_AccentTitleBars");
        if (_recentColorsLabel is not null)
            _recentColorsLabel.Text = GlassSettingsResources.Get("Themes_Colors_RecentColors");

        FillLightDarkCombo(_windowsModeCombo);
        FillLightDarkCombo(_appsModeCombo);
        FillModeChoiceCombo(_modeChoiceCombo);
        FillAccentModeCombo(_accentModeCombo);
    }

    private static void FillLightDarkCombo(ComboBox? combo)
    {
        if (combo is null)
            return;

        var items = GlassSettingsResources.GetSemicolonList("Themes_Colors_LightDarkItems");
        combo.Items.Clear();
        foreach (var item in items)
            combo.Items.Add(item);
    }

    private static void FillModeChoiceCombo(ComboBox? combo)
    {
        if (combo is null)
            return;

        var items = GlassSettingsResources.GetSemicolonList("Themes_Colors_ModeChoiceItems");
        combo.Items.Clear();
        foreach (var item in items)
            combo.Items.Add(item);
    }

    private static void FillAccentModeCombo(ComboBox? combo)
    {
        if (combo is null)
            return;

        var items = GlassSettingsResources.GetSemicolonList("Themes_Colors_AccentModeItems");
        combo.Items.Clear();
        foreach (var item in items)
            combo.Items.Add(item);
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
            DispatcherQueue.TryEnqueue(() =>
            {
                if (!IsLoaded || _accentApplyBusy)
                    return;
                if (!e.ThemeChanged && !e.AccentChanged)
                    return;

                if (e.ThemeChanged)
                    NoteThemeFileChangeForBaselineCapture();

                ReloadFromSystem();
                // После apply .theme ColorHistory/Colorization/Accent дописываются не одновременно.
                if (e.AccentChanged || e.ThemeChanged)
                    ScheduleAccentUiRefresh(delayedMs: 450);
            });
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

    private void ReloadFromSystem()
    {
        _loading = true;
        try
        {
            var mode = WindowsPersonalizationService.ReadAppModeChoice();
            if (_modeChoiceCombo is not null)
                _modeChoiceCombo.SelectedIndex = (int)mode;

            ApplyCustomModeBodyVisibility(mode == WindowsPersonalizationService.AppModeChoice.Custom);

            if (_windowsModeCombo is not null)
                _windowsModeCombo.SelectedIndex = WindowsPersonalizationService.ReadSystemUsesLightTheme() ? 0 : 1;
            if (_appsModeCombo is not null)
                _appsModeCombo.SelectedIndex = WindowsPersonalizationService.ReadAppsUseLightTheme() ? 0 : 1;

            TransparencyRow.IsGateOn = WindowsPersonalizationService.ReadTransparencyEffects();

            var accentMode = WindowsPersonalizationService.ReadAccentMode();
            if (_accentModeCombo is not null)
                _accentModeCombo.SelectedIndex = (int)accentMode;

            ApplyAccentExpandState(accentMode == WindowsPersonalizationService.AccentModeChoice.Manual);
            SyncAccentPickerFromSystem();
            SyncAccentPrevalenceFromSystem();
            RefreshRecentAccentPalette();
            ReconcileThemeAccentBaseline();
            RefreshAccentDefaultActionState();
        }
        finally
        {
            _loading = false;
        }
    }

    private void SyncAccentPrevalenceFromSystem()
    {
        if (_startTaskbarPrevalenceToggle is not null)
            _startTaskbarPrevalenceToggle.IsOn = WindowsPersonalizationService.ReadStartTaskbarColorPrevalence();
        if (_titleBarsPrevalenceToggle is not null)
            _titleBarsPrevalenceToggle.IsOn = WindowsPersonalizationService.ReadTitleBarsColorPrevalence();
        RefreshStartTaskbarPrevalenceEnabled();
    }

    private void RefreshStartTaskbarPrevalenceEnabled()
    {
        if (_startTaskbarPrevalenceToggle is null)
            return;

        // Как в Параметрах: в светлом режиме Windows тумблер Пуск/панель недоступен.
        _startTaskbarPrevalenceToggle.IsEnabled = !WindowsPersonalizationService.ReadSystemUsesLightTheme();
    }

    private void ApplyCustomModeBodyVisibility(bool custom)
    {
        ModeExpander.IsExpansionEnabled = true;
        ModeExpander.AllowBodyExpand = custom;
        ModeExpander.IsExpanded = custom;
        if (custom)
            DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Normal, SyncModeBodyComboAlignment);
    }

    private void ApplyAccentExpandState(bool manual)
    {
        // Тело (пикер + prevalence) разворачивается и в Авто, и во Вручную; пикер — только Manual.
        AccentExpander.IsExpansionEnabled = true;
        AccentExpander.AllowBodyExpand = true;
        AccentExpander.IsExpanded = true;
        AccentColorPicker.IsEnabled = manual && !_accentApplyBusy;
        RefreshAccentDefaultActionState();
    }

    private void SyncAccentPickerFromSystem()
    {
        // Пикер — цвет из Параметров (AccentPalette[3]), не DWM\AccentColor.
        var accent = WindowsPersonalizationService.ReadUserAccentColor();
        AccentColorPicker.SetColor(ThemeAccentColorHelper.ToArgbDword(accent));
    }

    private bool IsAccentManualSelected() =>
        _accentModeCombo is not null
        && _accentModeCombo.SelectedIndex == (int)WindowsPersonalizationService.AccentModeChoice.Manual;

    private void RefreshAccentDefaultActionState(bool? disableDefaultButton = null)
    {
        var manual = IsAccentManualSelected();
        AccentColorPanel.SetDefaultColorActionState(
            showDefaultAction: manual,
            disableDefaultButton: disableDefaultButton
                ?? (!manual || _accentApplyBusy || IsAccentAtThemeDefault()),
            isEnabled: !_accentApplyBusy);
    }

    /// <summary>Повторный sync пикера/Default после запоздалой записи Colorization* системой.</summary>
    private void ScheduleAccentUiRefresh(int delayedMs)
    {
        DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, () =>
        {
            if (!IsLoaded || _loading || _accentApplyBusy)
                return;

            SyncAccentPickerFromSystem();
            RefreshRecentAccentPalette();
            CaptureThemeAccentBaselineIfPending();
            RefreshAccentDefaultActionState();
        });

        _accentDefaultRefreshTimer ??= DispatcherQueue.CreateTimer();
        _accentDefaultRefreshTimer.IsRepeating = false;
        _accentDefaultRefreshTimer.Interval = TimeSpan.FromMilliseconds(delayedMs);
        _accentDefaultRefreshTimer.Tick -= AccentDefaultRefreshTimer_OnTick;
        _accentDefaultRefreshTimer.Tick += AccentDefaultRefreshTimer_OnTick;
        _accentDefaultRefreshTimer.Start();
    }

    private void AccentDefaultRefreshTimer_OnTick(DispatcherQueueTimer sender, object args)
    {
        sender.Tick -= AccentDefaultRefreshTimer_OnTick;
        if (!IsLoaded || _loading || _accentApplyBusy)
            return;

        SyncAccentPickerFromSystem();
        RefreshRecentAccentPalette();
        CaptureThemeAccentBaselineIfPending();
        RefreshAccentDefaultActionState();
    }

    private void ModeChoiceCombo_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || _modeChoiceCombo is null)
            return;

        var index = _modeChoiceCombo.SelectedIndex;
        if (index < 0)
            return;

        var choice = (WindowsPersonalizationService.AppModeChoice)index;
        if (choice == WindowsPersonalizationService.AppModeChoice.Custom)
        {
            ApplyCustomModeBodyVisibility(true);
            WindowsPersonalizationService.SetCustomModes(
                systemLight: _windowsModeCombo?.SelectedIndex == 0,
                appsLight: _appsModeCombo?.SelectedIndex == 0);
            RefreshStartTaskbarPrevalenceEnabled();
            return;
        }

        ApplyCustomModeBodyVisibility(false);
        WindowsPersonalizationService.SetLightDarkMode(light: choice == WindowsPersonalizationService.AppModeChoice.Light);
        if (_windowsModeCombo is not null)
            _windowsModeCombo.SelectedIndex = choice == WindowsPersonalizationService.AppModeChoice.Light ? 0 : 1;
        if (_appsModeCombo is not null)
            _appsModeCombo.SelectedIndex = _windowsModeCombo?.SelectedIndex ?? 0;
        RefreshStartTaskbarPrevalenceEnabled();
    }

    private void WindowsModeCombo_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading)
            return;

        CommitCustomModes();
    }

    private void AppsModeCombo_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading)
            return;

        CommitCustomModes();
    }

    private void CommitCustomModes()
    {
        if (_modeChoiceCombo?.SelectedIndex != (int)WindowsPersonalizationService.AppModeChoice.Custom)
            return;

        WindowsPersonalizationService.SetCustomModes(
            systemLight: _windowsModeCombo?.SelectedIndex == 0,
            appsLight: _appsModeCombo?.SelectedIndex == 0);
        RefreshStartTaskbarPrevalenceEnabled();
    }

    private void StartTaskbarPrevalenceToggle_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading || _startTaskbarPrevalenceToggle is null)
            return;

        WindowsPersonalizationService.SetStartTaskbarColorPrevalence(_startTaskbarPrevalenceToggle.IsOn);
    }

    private void TitleBarsPrevalenceToggle_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading || _titleBarsPrevalenceToggle is null)
            return;

        WindowsPersonalizationService.SetTitleBarsColorPrevalence(_titleBarsPrevalenceToggle.IsOn);
    }

    private void TransparencyRow_OnGateToggled(object sender, RoutedEventArgs e)
    {
        if (_loading)
            return;

        WindowsPersonalizationService.SetTransparencyEffects(TransparencyRow.IsGateOn);
    }

    private void AccentModeCombo_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || _accentApplyBusy || _accentModeCombo is null)
            return;

        var mode = _accentModeCombo.SelectedIndex == 0
            ? WindowsPersonalizationService.AccentModeChoice.Automatic
            : WindowsPersonalizationService.AccentModeChoice.Manual;
        ApplyAccentExpandState(mode == WindowsPersonalizationService.AccentModeChoice.Manual);
        if (mode == WindowsPersonalizationService.AccentModeChoice.Manual)
        {
            SyncAccentPickerFromSystem();
            RefreshAccentDefaultActionState();
            _ = ApplyAccentAsync(WindowsPersonalizationService.ReadUserAccentColor());
            return;
        }

        WindowsPersonalizationService.SetAccentMode(mode);
        RefreshAccentDefaultActionState();
        // После Авто система может сменить accent — подтянуть пикер/Default при возврате на Вручную.
        ScheduleAccentUiRefresh(delayedMs: 450);
    }

    private void AccentColorPicker_OnColorChanged(object? sender, uint argb)
    {
        if (_loading || _accentApplyBusy)
            return;

        if (_accentModeCombo is not null
            && _accentModeCombo.SelectedIndex != (int)WindowsPersonalizationService.AccentModeChoice.Manual)
        {
            _loading = true;
            _accentModeCombo.SelectedIndex = (int)WindowsPersonalizationService.AccentModeChoice.Manual;
            ApplyAccentExpandState(true);
            _loading = false;
        }

        var color = Color.FromArgb(
            255,
            (byte)((argb >> 16) & 0xFF),
            (byte)((argb >> 8) & 0xFF),
            (byte)(argb & 0xFF));
        _ = ApplyAccentAsync(color);
    }

    private void AccentColorPanel_OnDefaultColorClick(object sender, RoutedEventArgs e)
    {
        if (_loading || _accentApplyBusy)
            return;

        if (ResolveThemeDefaultAccentColor() is not { } color)
            return;

        _loading = true;
        try
        {
            // Default всегда переводит в Вручную (как системные Параметры).
            if (_accentModeCombo is not null)
                _accentModeCombo.SelectedIndex = (int)WindowsPersonalizationService.AccentModeChoice.Manual;
            ApplyAccentExpandState(true);
            AccentColorPicker.SetColor(ThemeAccentColorHelper.ToArgbDword(color));
        }
        finally
        {
            _loading = false;
        }

        _ = ApplyAccentAsync(color, disableDefaultButton: true);
    }

    private async Task ApplyAccentAsync(Color color, bool? disableDefaultButton = null)
    {
        if (_accentApplyBusy)
            return;

        _accentApplyBusy = true;
        SetAccentControlsBusy(true);
        try
        {
            await WindowsPersonalizationService.ApplyAccentColorAsync(color).ConfigureAwait(true);
            _loading = true;
            try
            {
                SyncAccentPickerFromSystem();
                RefreshRecentAccentPalette();
            }
            finally
            {
                _loading = false;
            }

            RefreshAccentDefaultActionState(disableDefaultButton);
        }
        finally
        {
            _accentApplyBusy = false;
            SetAccentControlsBusy(false);
            RefreshAccentDefaultActionState(disableDefaultButton);
        }
    }

    private void SetAccentControlsBusy(bool busy)
    {
        AccentColorPicker.IsEnabled = !busy && IsAccentManualSelected();
        if (_accentModeCombo is not null)
            _accentModeCombo.IsEnabled = !busy;
        RefreshAccentDefaultActionState();
    }

    /// <summary>
    /// Исходный .theme для Default: LastApplied → non-Custom CurrentTheme →
    /// тема с тем же VisualStyles Path (не Custom).
    /// </summary>
    private static bool TryResolveSourceThemeFilePath(out string themeFilePath)
    {
        themeFilePath = string.Empty;

        var lastApplied = GlassSettingsRegistry.ReadLastAppliedThemeFilePath();
        if (!string.IsNullOrWhiteSpace(lastApplied) && File.Exists(lastApplied)
            && !IsCustomThemeFile(lastApplied))
        {
            themeFilePath = lastApplied;
            return true;
        }

        if (VisualStyleThemeManager.TryGetCurrentThemeFilePath(out var current)
            && !string.IsNullOrWhiteSpace(current)
            && File.Exists(current)
            && !IsCustomThemeFile(current))
        {
            themeFilePath = current;
            return true;
        }

        return TryFindRelatedThemeFilePath(out themeFilePath);
    }

    private static bool IsCustomThemeFile(string path) =>
        string.Equals(Path.GetFileName(path), "Custom.theme", StringComparison.OrdinalIgnoreCase);

    private static bool TryFindRelatedThemeFilePath(out string themeFilePath)
    {
        themeFilePath = string.Empty;
        if (!VisualStyleThemeManager.TryGetCurrentThemeFilePath(out var current)
            || string.IsNullOrWhiteSpace(current)
            || !File.Exists(current))
        {
            return false;
        }

        if (!VisualStyleCatalog.TryReadVisualStylePath(current, out var vsRaw)
            || string.IsNullOrWhiteSpace(vsRaw))
        {
            return false;
        }

        var targetMsstyles = VisualStyleCatalog.NormalizeResourcePath(vsRaw, current);
        if (string.IsNullOrWhiteSpace(targetMsstyles))
            return false;

        string? targetFull = null;
        try
        {
            targetFull = Path.GetFullPath(targetMsstyles);
        }
        catch
        {
            return false;
        }

        foreach (var entry in UserThemeCatalog.GetThemesForGrid())
        {
            if (entry is null || string.IsNullOrWhiteSpace(entry.ThemeFilePath) || IsCustomThemeFile(entry.ThemeFilePath))
                continue;

            var candidateMs = entry.VisualStylePath;
            if (string.IsNullOrWhiteSpace(candidateMs))
                continue;

            try
            {
                if (!string.Equals(Path.GetFullPath(candidateMs), targetFull, StringComparison.OrdinalIgnoreCase))
                    continue;
            }
            catch
            {
                continue;
            }

            if (ThemeAccentColorHelper.TryParseColorizationColor(entry.ThemeFilePath) is null)
                continue;

            themeFilePath = entry.ThemeFilePath;
            return true;
        }

        return false;
    }

    private static Color? ResolveThemeDefaultAccentColor()
    {
        // Default = baseline после apply темы (AccentPalette[3]), не ColorizationColor из .theme.
        if (GlassSettingsRegistry.ReadLastAppliedThemeAccentColor() is { } dword)
        {
            return ThemeAccentColorHelper.ToOpaqueForeground(
                ThemeAccentColorHelper.FromArgbDword(dword));
        }

        // Seed: live user accent, иначе Colorization из исходного .theme.
        var live = WindowsPersonalizationService.ReadUserAccentColor();
        PersistThemeAccentBaseline(live, overwrite: false);
        if (GlassSettingsRegistry.ReadLastAppliedThemeAccentColor() is { } seeded)
        {
            return ThemeAccentColorHelper.ToOpaqueForeground(
                ThemeAccentColorHelper.FromArgbDword(seeded));
        }

        if (TryResolveSourceThemeFilePath(out var path)
            && ThemeAccentColorHelper.TryParseColorizationColor(path) is { } fromTheme)
        {
            var opaque = ThemeAccentColorHelper.ToOpaqueForeground(fromTheme);
            PersistThemeAccentBaseline(opaque, overwrite: false);
            return opaque;
        }

        return null;
    }

    private static void PersistThemeAccentBaseline(Color color, bool overwrite = false)
    {
        if (!overwrite && GlassSettingsRegistry.ReadLastAppliedThemeAccentColor() is not null)
            return;

        GlassSettingsRegistry.WriteLastAppliedThemeAccentColor(
            ThemeAccentColorHelper.ToArgbDword(color));
    }

    private void NoteThemeFileChangeForBaselineCapture()
    {
        if (!VisualStyleThemeManager.TryGetCurrentThemeRegistryFilePath(out var path)
            || string.IsNullOrWhiteSpace(path))
        {
            return;
        }

        if (string.Equals(_watchedThemePath, path, StringComparison.OrdinalIgnoreCase))
            return;

        _watchedThemePath = path;
        _pendingThemeAccentBaselineCapture = true;
    }

    private void CaptureThemeAccentBaselineIfPending()
    {
        if (!_pendingThemeAccentBaselineCapture)
            return;

        _pendingThemeAccentBaselineCapture = false;
        PersistThemeAccentBaseline(
            WindowsPersonalizationService.ReadUserAccentColor(),
            overwrite: true);
    }

    /// <summary>
    /// Старый baseline = Colorization из .theme при другом AccentPalette[3] — заменить на live.
    /// </summary>
    private void ReconcileThemeAccentBaseline()
    {
        var live = WindowsPersonalizationService.ReadUserAccentColor();
        if (GlassSettingsRegistry.ReadLastAppliedThemeAccentColor() is not { } dword)
        {
            PersistThemeAccentBaseline(live, overwrite: false);
            return;
        }

        var baseline = ThemeAccentColorHelper.ToOpaqueForeground(
            ThemeAccentColorHelper.FromArgbDword(dword));
        if (ThemeAccentColorHelper.RgbEquals(live, baseline))
            return;

        if (!TryResolveSourceThemeFilePath(out var path)
            || ThemeAccentColorHelper.TryParseColorizationColor(path) is not { } fromTheme)
        {
            return;
        }

        var themeColorization = ThemeAccentColorHelper.ToOpaqueForeground(fromTheme);
        if (!ThemeAccentColorHelper.RgbEquals(baseline, themeColorization))
            return;

        // Baseline устарел (.theme Colorization), live — фактический акцент темы.
        PersistThemeAccentBaseline(live, overwrite: true);
    }

    private bool IsAccentAtThemeDefault()
    {
        if (ResolveThemeDefaultAccentColor() is not { } themeDefault)
            return true;

        var current = WindowsPersonalizationService.ReadUserAccentColor();
        return ThemeAccentColorHelper.RgbEquals(current, themeDefault);
    }
}
