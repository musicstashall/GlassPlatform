using System.Diagnostics;
using GlassSettings.Core.Layout;
using GlassSettings.Controls;
using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;
using Windows.Foundation;
using WinRT.Interop;

namespace GlassSettings.Pages;

public sealed partial class VisualStylesPage : Page
{
    private const double HeroColumnGap = 24;
    private const double QuickActionColumnGap = 8;
    private const double GridTileWidth = 168;
    private const double GridTileGap = 8;
    private const int MaxThemeColumns = 6;

    private TextBlock? _otherLabel;
    private CheckBox? _applyFontsCheck;
    private Border? _applyFontsCheckHost;
    private EventHandler<DesktopEnvironmentChangedEventArgs>? _environmentChangedHandler;
    private DispatcherQueueTimer? _wallpaperRefreshTimer;
    private int _themeCount;
    private string? _lastActiveThemePath;
    private bool _applyInProgress;
    private bool _saveInProgress;
    private bool _applyFontsLoading;
    private bool _signatureToggleLoading;
    private bool _signatureBusy;
    private EventHandler? _rebootActionHandler;
    private bool _heroActionsStacked;
    private bool _heroLayoutInitialized;
    private double _quickActionsCol0Width;
    private double _quickActionsCol1Width;
    private double _quickActionsMinCol0Width;
    private double _quickActionsMinCol1Width;
    private double _quickActionsMinTableWidth;
    private double _quickActionsPreferredTableWidth;
    private bool _quickActionsWidthsDirty = true;

    public VisualStylesPage()
    {
        InitializeComponent();
        ConfigureThemesExpander();
        ApplyLocalizedStrings();
        HeroLayoutHost.SizeChanged += (_, _) => UpdatePageLayout();
        ThemeItemsHost.SizeChanged += (_, _) => UpdateThemeTilesLayout();
        ThemesExpander.Loaded += ThemesExpander_OnLoaded;
        Loaded += (_, _) =>
        {
            UpdatePageLayout();
            UpdateBackgroundCardSubtitle();
        };
        Unloaded += OnUnloaded;
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        ThemesExpander.IsExpanded = true;
        HookEnvironmentWatcher();
        LoadThemes();
        if (ThemesExpander.IsLoaded)
            ThemesExpander_OnLoaded(ThemesExpander, new RoutedEventArgs());
        _quickActionsWidthsDirty = true;
        UpdatePageLayout();

        // Восстановить уведомление о перезагрузке при возвращении на страницу
        // (аналогично восстановлению yellow notice на детальных страницах).
        try
        {
            var state = SecureUxThemeService.Query().State;
            string? baseKey = state switch
            {
                SecureUxThemeState.ActivationPendingRestart => "Themes_SignatureActivated",
                SecureUxThemeState.DeactivationPendingRestart => "Themes_SignatureDeactivated",
                _ => null
            };
            if (baseKey is not null)
            {
                var message = GlassSettingsResources.Get(baseKey) + " " + GlassSettingsResources.Get("Themes_RestartRequired");
                DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, () =>
                {
                    if (IsLoaded)
                        _ = ShowRebootNoticeAsync(
                            message,
                            GlassSettingsResources.Get("Themes_SignatureRebootButton"));
                });
            }
        }
        catch
        {
            // SecureUxTheme query failed — skip notice restore.
        }
    }

    protected override void OnNavigatedFrom(NavigationEventArgs e)
    {
        UnhookEnvironmentWatcher();
        base.OnNavigatedFrom(e);
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
        if (_environmentChangedHandler is null ||
            DesktopEnvironmentWatcher.Current is not { } watcher)
        {
            return;
        }

        watcher.Changed -= _environmentChangedHandler;
        _environmentChangedHandler = null;
    }

    private void OnDesktopEnvironmentChanged(DesktopEnvironmentChangedEventArgs e)
    {
        if (!IsLoaded)
            return;

        try
        {
            if (e.ThemeChanged || e.AccentChanged)
            {
                RefreshThemePreview();
                UpdateColorCardSubtitle();
                UpdateBackgroundCardSubtitle();
                if (_quickActionsWidthsDirty)
                    UpdateHeroLayout();
                // Во время apply ThemeChanged приходит до Capture → ложный show Save.
                if (!_applyInProgress)
                    UpdateSaveOrPersonalizedVisibility();
            }

            if (e.WallpaperChanged)
                ScheduleWallpaperPreviewRefresh();
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"GlassSettings VisualStylesPage.OnDesktopEnvironmentChanged: {ex}");
        }
    }

    private void RefreshThemePreview()
    {
        var activeEntry = ResolveCurrentThemeEntry();
        var currentThemePath = VisualStyleThemeManager.TryGetCurrentThemeRegistryFilePath(out var rawThemePath)
            && !string.IsNullOrWhiteSpace(rawThemePath)
            ? rawThemePath
            : null;

        CurrentThemePreview.Visibility = Visibility.Visible;
        CurrentThemePreview.ApplyThemeChrome(activeEntry);
        UpdateExpanderHeaderLabel();
        if (!_applyInProgress)
            UpdateSaveOrPersonalizedVisibility();

        if (!string.Equals(_lastActiveThemePath, currentThemePath, StringComparison.OrdinalIgnoreCase))
        {
            _lastActiveThemePath = currentThemePath;
            ReloadThemeGrid(activeEntry);
        }
    }

    private void ScheduleWallpaperPreviewRefresh()
    {
        _wallpaperRefreshTimer ??= DispatcherQueue.CreateTimer();
        _wallpaperRefreshTimer.Interval = TimeSpan.FromMilliseconds(120);
        _wallpaperRefreshTimer.IsRepeating = false;
        _wallpaperRefreshTimer.Tick -= OnWallpaperPreviewRefreshTick;
        _wallpaperRefreshTimer.Tick += OnWallpaperPreviewRefreshTick;
        _wallpaperRefreshTimer.Stop();
        _wallpaperRefreshTimer.Start();
    }

    private void OnWallpaperPreviewRefreshTick(DispatcherQueueTimer sender, object args)
    {
        sender.Tick -= OnWallpaperPreviewRefreshTick;
        if (!IsLoaded)
            return;

        var activeEntry = ResolveCurrentThemeEntry();
        var path = CurrentWallpaperResolver.TryGetDesktopWallpaperPath() ?? activeEntry?.PreviewImagePath;
        CurrentThemePreview.ForceRefreshPreview(path);
        UpdateBackgroundCardSubtitle();
        if (_quickActionsWidthsDirty)
            UpdateHeroLayout();
    }

    private void RefreshLivePreview()
    {
        if (!IsLoaded)
            return;

        try
        {
            RefreshThemePreview();
            var activeEntry = ResolveCurrentThemeEntry();
            var path = CurrentWallpaperResolver.TryGetDesktopWallpaperPath() ?? activeEntry?.PreviewImagePath;
            CurrentThemePreview.ForceRefreshPreview(path);
            UpdateColorCardSubtitle();
            UpdateBackgroundCardSubtitle();
            UpdateSaveOrPersonalizedVisibility();
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"GlassSettings VisualStylesPage.RefreshLivePreview: {ex}");
        }
    }

    private void ReloadThemeGrid(VisualStyleThemeEntry? activeEntry)
    {
        if (ThemeItems.ItemsSource is not IList<VisualStyleTileModel> models || activeEntry is null)
        {
            LoadThemes();
            return;
        }

        var activeFullPath = Path.GetFullPath(activeEntry.ThemeFilePath);
        var changed = false;
        foreach (var model in models)
        {
            var isActive = string.Equals(
                Path.GetFullPath(model.Entry.ThemeFilePath),
                activeFullPath,
                StringComparison.OrdinalIgnoreCase);
            if (model.IsActive != isActive)
                changed = true;
        }

        if (!changed)
            return;

        ThemeItems.ItemsSource = models
            .Select(model => new VisualStyleTileModel
            {
                Entry = model.Entry,
                IsActive = string.Equals(
                    Path.GetFullPath(model.Entry.ThemeFilePath),
                    activeFullPath,
                    StringComparison.OrdinalIgnoreCase)
            })
            .ToList();
    }

    private void OnUnloaded(object sender, RoutedEventArgs e) =>
        UnhookEnvironmentWatcher();

    private void ConfigureThemesExpander()
    {
        ThemesExpander.IconGlyph = SettingsSectionGlyphs.Theme;
        ThemesExpander.IsExpanded = true;
        ThemesExpander.DimHeaderActionsWithGate = false;

        _otherLabel = new TextBlock
        {
            VerticalAlignment = VerticalAlignment.Center,
            // Тот же шрифт, что у Content чекбокса (WinUI CheckBox → Body).
            Style = (Style)Application.Current.Resources["BodyTextBlockStyle"]
        };

        _applyFontsCheck = new CheckBox
        {
            VerticalAlignment = VerticalAlignment.Center,
            MinWidth = 24,
            FlowDirection = FlowDirection.LeftToRight
        };
        _applyFontsCheck.Checked += ApplyFontsCheck_OnChanged;
        _applyFontsCheck.Unchecked += ApplyFontsCheck_OnChanged;

        var headerSpacing = Application.Current.Resources["GlassSettingsHeaderActionsSpacing"] is double spacing
            ? spacing
            : 12d;

        // Padding справа входит в DesiredSize → SpoilerHeaderLayoutEngine учитывает при переносе.
        _applyFontsCheckHost = new Border
        {
            Padding = new Thickness(0, 0, headerSpacing, 0),
            VerticalAlignment = VerticalAlignment.Center,
            Child = _applyFontsCheck
        };

        // Справа налево: шеврон → имя темы (StaticHost) → чекбокс (ControlsTable).
        ThemesExpander.HeaderSlider = _applyFontsCheckHost;
        ThemesExpander.HeaderActions = _otherLabel;
    }

    private void ThemesExpander_OnLoaded(object sender, RoutedEventArgs e)
    {
        if (_applyFontsCheckHost is not null)
            ThemesExpander.HeaderSlider = _applyFontsCheckHost;
        if (_otherLabel is not null)
            ThemesExpander.HeaderActions = _otherLabel;
        ThemesExpander.RefreshHeaderActionsLayout(resetStackState: true);
    }

    private void ApplyLocalizedStrings()
    {
        BackgroundCard.Title = GlassSettingsResources.Get("Themes_BackgroundCardTitle");
        BackgroundCard.IconGlyphChar = SettingsSectionGlyphs.Image;
        UpdateBackgroundCardSubtitle();

        ColorCard.Title = GlassSettingsResources.Get("Themes_ColorCardTitle");
        ColorCard.IconGlyphChar = SettingsSectionGlyphs.Colorization;
        UpdateColorCardSubtitle();

        SignatureCard.Title = GlassSettingsResources.Get("Themes_SignatureCardTitle");
        SignatureCard.Description = GlassSettingsResources.Get("Themes_SignatureCardDesc");
        SignatureCard.IconGlyphChar = SettingsSectionGlyphs.Signature;

        TextFontsCard.Title = GlassSettingsResources.Get("NavText");
        TextFontsCard.Description = GlassSettingsResources.Get("Themes_TextAndFontsDesc");
        TextFontsCard.IconGlyphChar = SettingsSectionGlyphs.Text;

        if (_applyFontsCheck is not null)
            _applyFontsCheck.Content = GlassSettingsResources.Get("Themes_ApplyFonts");

        UsePersonalizedThemeButton.Content = GlassSettingsResources.Get("Themes_UsePersonalizedTheme");
        SaveThemeButton.Content = GlassSettingsResources.Get("Themes_SaveTheme");

        ThemesExpander.Title = GlassSettingsResources.Get("Themes_CurrentSection");
        ThemesExpander.Description = GlassSettingsResources.Get("Themes_SectionDesc");
        EmptyText.Text = GlassSettingsResources.Get("Themes_Empty");
        FindThemesText.Text = GlassSettingsResources.Get("Themes_FindOnline");
        BrowseThemesButton.Content = GlassSettingsResources.Get("Themes_BrowseThemes");
        SecureUxThemeOnlineIcon.Glyph = SettingsSectionGlyphs.WebSearch;
        SecureUxThemeOnlineText.Text = GlassSettingsResources.Get("Themes_SecureUxThemeOnlineLink");

        _quickActionsWidthsDirty = true;
    }

    private void UpdateColorCardSubtitle()
    {
        try
        {
            ColorCard.Description = CapitalizeAccentSubtitle(
                AccentColorNameHelper.ResolveCurrentAccentDisplayName());
        }
        catch
        {
            ColorCard.Description = CapitalizeAccentSubtitle(
                GlassSettingsResources.Get("Themes_ColorName_Gray"));
        }

        _quickActionsWidthsDirty = true;
    }

    private void UpdateBackgroundCardSubtitle()
    {
        try
        {
            BackgroundCard.Description = CurrentWallpaperResolver.TryGetDesktopBackgroundSubtitle();
        }
        catch (Exception ex)
        {
            BackgroundCard.Description = string.Empty;
            Debug.WriteLine($"GlassSettings VisualStylesPage.UpdateBackgroundCardSubtitle: {ex}");
        }

        _quickActionsWidthsDirty = true;
    }

    private static string CapitalizeAccentSubtitle(string? text)
    {
        if (string.IsNullOrWhiteSpace(text))
            return string.Empty;

        var s = text.Trim();
        if (s.Length == 1)
            return char.ToUpperInvariant(s[0]).ToString();

        return char.ToUpper(s[0], System.Globalization.CultureInfo.CurrentUICulture) + s[1..];
    }

    private void UpdatePageLayout()
    {
        UpdateHeroLayout();
        UpdateThemeTilesLayout();
    }

    private void UpdateHeroLayout()
    {
        var hostWidth = HeroLayoutHost.ActualWidth;
        if (hostWidth <= 0)
            return;

        EnsureQuickActionsColumnWidths();
        if (_quickActionsMinTableWidth <= 0)
            return;

        var availableRight = hostWidth
            - HeroPreviewLayoutMetrics.FootprintWidth
            - HeroColumnGap;

        // Сжатие текста до 100 px; перенос при min−1, возврат при min+1.
        bool sideBySide;
        if (!_heroLayoutInitialized)
        {
            sideBySide = availableRight >= _quickActionsMinTableWidth;
        }
        else if (_heroActionsStacked)
        {
            sideBySide = availableRight >= _quickActionsMinTableWidth + 1;
        }
        else
        {
            sideBySide = availableRight >= _quickActionsMinTableWidth;
        }

        _heroActionsStacked = !sideBySide;
        _heroLayoutInitialized = true;

        if (sideBySide)
        {
            ApplySideBySideQuickActionColumns(availableRight);
            ColPreview.Width = GridLength.Auto;
            ColActions.Width = new GridLength(1, GridUnitType.Star);
            Grid.SetColumn(CurrentThemePreview, 0);
            Grid.SetRow(CurrentThemePreview, 0);
            Grid.SetColumnSpan(CurrentThemePreview, 1);
            Grid.SetColumn(QuickActionsHost, 1);
            Grid.SetRow(QuickActionsHost, 0);
            Grid.SetColumnSpan(QuickActionsHost, 1);
            CurrentThemePreview.MinWidth = HeroPreviewLayoutMetrics.FootprintWidth;
            CurrentThemePreview.HorizontalAlignment = HorizontalAlignment.Left;
            QuickActionsHost.VerticalAlignment = VerticalAlignment.Stretch;
            QuickActionsHost.MinHeight = HeroPreviewLayoutMetrics.OuterHeight;
            return;
        }

        ApplyStackedQuickActionColumns();
        ColPreview.Width = GridLength.Auto;
        ColActions.Width = new GridLength(1, GridUnitType.Star);
        QuickActionsHost.ClearValue(FrameworkElement.MinHeightProperty);
        CurrentThemePreview.ClearValue(FrameworkElement.MinWidthProperty);

        Grid.SetColumn(CurrentThemePreview, 0);
        Grid.SetRow(CurrentThemePreview, 0);
        Grid.SetColumnSpan(CurrentThemePreview, 2);
        Grid.SetColumn(QuickActionsHost, 0);
        Grid.SetRow(QuickActionsHost, 1);
        Grid.SetColumnSpan(QuickActionsHost, 2);
        CurrentThemePreview.HorizontalAlignment = HorizontalAlignment.Left;
        QuickActionsHost.VerticalAlignment = VerticalAlignment.Top;
    }

    private void ApplySideBySideQuickActionColumns(double availableRight)
    {
        QuickActionCol0.MinWidth = _quickActionsMinCol0Width;
        QuickActionCol1.MinWidth = _quickActionsMinCol1Width;
        QuickActionCol0.Width = new GridLength(1, GridUnitType.Star);
        QuickActionCol1.Width = new GridLength(1, GridUnitType.Star);
        QuickActionsHost.ColumnSpacing = QuickActionColumnGap;
        QuickActionsHost.MinWidth = _quickActionsMinTableWidth;

        // Шире preferred — растягиваем до колонки (паритет с notice/expander).
        if (availableRight >= _quickActionsPreferredTableWidth)
        {
            QuickActionCol0.MaxWidth = double.PositiveInfinity;
            QuickActionCol1.MaxWidth = double.PositiveInfinity;
            QuickActionsHost.ClearValue(FrameworkElement.WidthProperty);
            QuickActionsHost.ClearValue(FrameworkElement.MaxWidthProperty);
            QuickActionsHost.HorizontalAlignment = HorizontalAlignment.Stretch;
            return;
        }

        QuickActionCol0.MaxWidth = _quickActionsCol0Width;
        QuickActionCol1.MaxWidth = _quickActionsCol1Width;
        QuickActionsHost.MaxWidth = _quickActionsPreferredTableWidth;
        QuickActionsHost.Width = Math.Max(availableRight, _quickActionsMinTableWidth);
        QuickActionsHost.HorizontalAlignment = HorizontalAlignment.Left;
    }

    private void ApplyStackedQuickActionColumns()
    {
        QuickActionCol0.MinWidth = _quickActionsMinCol0Width;
        QuickActionCol1.MinWidth = _quickActionsMinCol1Width;
        QuickActionCol0.MaxWidth = double.PositiveInfinity;
        QuickActionCol1.MaxWidth = double.PositiveInfinity;
        QuickActionCol0.Width = new GridLength(1, GridUnitType.Star);
        QuickActionCol1.Width = new GridLength(1, GridUnitType.Star);

        QuickActionsHost.ColumnSpacing = QuickActionColumnGap;
        QuickActionsHost.ClearValue(FrameworkElement.WidthProperty);
        QuickActionsHost.ClearValue(FrameworkElement.MinWidthProperty);
        QuickActionsHost.ClearValue(FrameworkElement.MaxWidthProperty);
        QuickActionsHost.HorizontalAlignment = HorizontalAlignment.Stretch;
    }

    private void EnsureQuickActionsColumnWidths()
    {
        if (!_quickActionsWidthsDirty && _quickActionsMinTableWidth > 0)
            return;

        var pref0 = Math.Max(BackgroundCard.MeasurePreferredWidth(), SignatureCard.MeasurePreferredWidth());
        var pref1 = Math.Max(ColorCard.MeasurePreferredWidth(), TextFontsCard.MeasurePreferredWidth());
        var min0 = Math.Max(BackgroundCard.MeasureMinWidth(), SignatureCard.MeasureMinWidth());
        var min1 = Math.Max(ColorCard.MeasureMinWidth(), TextFontsCard.MeasureMinWidth());
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

    private void UpdateThemeTilesLayout()
    {
        if (_themeCount <= 0)
            return;

        var hostWidth = ThemeItemsHost.ActualWidth;
        if (hostWidth <= 0)
            return;

        var tileUnit = GridTileWidth + GridTileGap;
        var maxColumns = Math.Clamp((int)((hostWidth + GridTileGap) / tileUnit), 1, MaxThemeColumns);
        var columns = Math.Min(_themeCount, maxColumns);
        var gridWidth = columns * GridTileWidth + Math.Max(0, columns - 1) * GridTileGap;

        if (ThemeItems.Layout is UniformGridLayout layout)
            layout.MaximumRowsOrColumns = columns;

        ThemeItems.Width = gridWidth;
        ThemeItems.HorizontalAlignment = HorizontalAlignment.Center;
    }

    private void LoadThemes()
    {
        var activeEntry = ResolveCurrentThemeEntry();
        var currentThemePath = VisualStyleThemeManager.TryGetCurrentThemeFilePath(out var rawThemePath)
            && !string.IsNullOrWhiteSpace(rawThemePath)
            ? Path.GetFullPath(rawThemePath)
            : null;

        BindCurrentThemePreview(activeEntry);
        _lastActiveThemePath = currentThemePath;
        UpdateExpanderHeaderLabel();
        OpenGlassThemeSection.EnsureSaveBaselineSeeded();
        UpdateSaveOrPersonalizedVisibility();
        UpdateColorCardSubtitle();
        UpdateBackgroundCardSubtitle();
        SyncApplyFontsCheckFromRegistry();
        SyncSignatureToggleFromSystem();

        var themes = UserThemeCatalog.GetThemesForGrid();
        _themeCount = themes.Count;
        if (themes.Count == 0)
        {
            ThemeItems.Visibility = Visibility.Collapsed;
            EmptyText.Visibility = Visibility.Visible;
            return;
        }

        EmptyText.Visibility = Visibility.Collapsed;
        ThemeItems.Visibility = Visibility.Visible;

        ThemeItems.ItemsSource = themes
            .Select(theme => new VisualStyleTileModel
            {
                Entry = theme,
                IsActive = currentThemePath is not null &&
                           string.Equals(Path.GetFullPath(theme.ThemeFilePath), currentThemePath, StringComparison.OrdinalIgnoreCase)
            })
            .ToList();

        UpdateThemeTilesLayout();
    }

    private static VisualStyleThemeEntry? ResolveCurrentThemeEntry()
    {
        if (!VisualStyleThemeManager.TryGetCurrentThemeFilePath(out var currentPath))
            return null;

        var entry = VisualStyleCatalog.TryGetEntryForThemeFile(currentPath);
        if (entry is not null)
            return entry;

        if (VisualStyleThemeManager.TryGetActiveTheme(out var catalogEntry) && catalogEntry is not null)
            return catalogEntry;

        return VisualStyleCatalog.TryGetFallbackEntryForThemeFile(currentPath);
    }

    private void BindCurrentThemePreview(VisualStyleThemeEntry? entry, bool forceImageRefresh = true)
    {
        CurrentThemePreview.Visibility = Visibility.Visible;
        CurrentThemePreview.ApplyThemeChrome(entry);
        var path = CurrentWallpaperResolver.TryGetDesktopWallpaperPath() ?? entry?.PreviewImagePath;
        if (forceImageRefresh)
            CurrentThemePreview.ForceRefreshPreview(path);
        else
            CurrentThemePreview.RefreshPreview(path);
    }

    private void UpdateExpanderHeaderLabel()
    {
        if (_otherLabel is null)
            return;

        var name = ThemeDisplayNameResolver.TryResolveForCurrentThemeFile();

        _otherLabel.Text = !string.IsNullOrWhiteSpace(name)
            ? name
            : GlassSettingsResources.Get("Themes_OtherLabel");
    }

    private void UpdateSaveOrPersonalizedVisibility(bool force = false)
    {
        if (_applyInProgress && !force)
            return;

        var showSave = UserThemeSaveService.ShouldShowSaveButton();
        var showPersonalized = !showSave && UserThemeCatalog.ShouldShowPersonalizedThemeLink();
        SaveThemeButton.Visibility = showSave ? Visibility.Visible : Visibility.Collapsed;
        UsePersonalizedThemeButton.Visibility = showPersonalized
            ? Visibility.Visible
            : Visibility.Collapsed;
    }

    private void ThemeItems_OnItemInvoked(ItemsView sender, ItemsViewItemInvokedEventArgs e)
    {
        if (e.InvokedItem is VisualStyleTileModel model)
            _ = OnThemeSelectedAsync(model.Entry);
    }

    private async Task OnThemeSelectedAsync(VisualStyleThemeEntry entry)
    {
        if (_applyInProgress || entry is null)
            return;

        if (SecureUxThemeService.ShouldBlockCustomThemeApply(entry))
        {
            await ShowThemesNoticeAsync(
                SettingsNoticeSeverity.Error,
                GlassSettingsResources.Get("Themes_SignatureApplyBlockedPending")).ConfigureAwait(true);
            return;
        }

        _applyInProgress = true;
        ThemeItems.IsEnabled = false;
        try
        {
            var result = await VisualStyleThemeManager.ApplyThemeAsync(entry).ConfigureAwait(true);
            if (!IsLoaded)
                return;

            if (result.Success)
            {
                // Host пишет DWM defaults с debounce — не settle и не wallpaper-heal до этого.
                if (ThemeDwmBaselineSync.IsGlassCompatHostWritingPolicy())
                {
                    await Task.Delay(ThemeDwmBaselineSync.HostPolicySettleDelayMs)
                        .ConfigureAwait(true);
                    if (!IsLoaded)
                        return;
                }

                await ThemeDwmBaselineSync.SettleAndReloadBaselineAsync().ConfigureAwait(true);
                if (!IsLoaded)
                    return;

                // Host THEMECHANGED уже шлёт TaskbarBoth; после settle — ещё раз (uxtheme #132+#139).
                _ = ReloadRenderService.SignalReloadRender(ReloadRenderFlags.TaskbarBoth);

                // На случай, когда OG policy не в ShouldSyncDwmBaseline — всё равно снять Save-baseline.
                OpenGlassThemeSection.CaptureSaveBaselineFromLive();
                UpdateSaveOrPersonalizedVisibility(force: true);

                // Список доступен до анимации success-зеркала (после чистого baseline).
                EndThemeApplyGate();
                await ShowThemesNoticeAsync(
                    SettingsNoticeSeverity.Success,
                    GlassSettingsResources.Format("Themes_ApplySuccessFormat", entry.DisplayName)).ConfigureAwait(true);

                // Wallpaper-heal после baseline — иначе ложный fingerprint / жёлтый unsaved.
                await StaThreadRunner.RunAsync(DesktopBackgroundService.TryNormalizeDegenerateSlideshowToPicture)
                    .ConfigureAwait(true);
                if (IsLoaded)
                    RefreshLivePreview();
            }
            else
            {
                EndThemeApplyGate();
                await ShowThemesNoticeAsync(
                    SettingsNoticeSeverity.Error,
                    GlassSettingsResources.Format(
                        "Themes_ApplyFailedFormat",
                        entry.DisplayName,
                        unchecked((uint)result.HResult))).ConfigureAwait(true);
            }
        }
        catch (Exception ex)
        {
            if (!IsLoaded)
                return;

            EndThemeApplyGate();
            await ShowThemesNoticeAsync(
                SettingsNoticeSeverity.Error,
                GlassSettingsResources.Format(
                    "Themes_ApplyFailedFormat",
                    entry.DisplayName,
                    unchecked((uint)ex.HResult))).ConfigureAwait(true);
            Debug.WriteLine($"GlassSettings VisualStylesPage.OnThemeSelectedAsync: {ex}");
        }
        finally
        {
            EndThemeApplyGate();
        }
    }

    /// <summary>Снять блокировку сетки тем (до показа notice, не после).</summary>
    private void EndThemeApplyGate()
    {
        _applyInProgress = false;
        if (IsLoaded)
            ThemeItems.IsEnabled = true;
    }

    private void SyncApplyFontsCheckFromRegistry()
    {
        if (_applyFontsCheck is null)
            return;

        _applyFontsLoading = true;
        try
        {
            _applyFontsCheck.IsChecked = GlassSettingsRegistry.ReadEnableSystemFont();
        }
        finally
        {
            _applyFontsLoading = false;
        }
    }

    private void ApplyFontsCheck_OnChanged(object sender, RoutedEventArgs e)
    {
        if (_applyFontsLoading || _applyFontsCheck is null)
            return;

        GlassSettingsRegistry.WriteEnableSystemFont(_applyFontsCheck.IsChecked == true);
    }

    private void SyncSignatureToggleFromSystem()
    {
        _signatureToggleLoading = true;
        try
        {
            var snapshot = SecureUxThemeService.Query();
            SignatureToggle.IsEnabled = snapshot.SwitchEnabled && !_signatureBusy;
            SignatureToggle.IsOn = snapshot.SwitchOn;
            SignatureToggle.SyncInnerEnabled();

            var cardBlocked = !snapshot.LocalDllPresent
                && snapshot.State is not SecureUxThemeState.Active
                    and not SecureUxThemeState.ActivationPendingRestart
                    and not SecureUxThemeState.DeactivationPendingRestart;
            SignatureCard.IsEnabled = !cardBlocked && !_signatureBusy;

            GlassSettingsToolTipService.SetToolTip(SignatureCard, null);

            if (snapshot.State == SecureUxThemeState.Active)
                _ = SecureUxThemeService.TryEnsureLoaded();
        }
        finally
        {
            _signatureToggleLoading = false;
        }
    }

    private async void SignatureToggle_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_signatureToggleLoading || _signatureBusy)
            return;

        var wantOn = SignatureToggle.IsOn;
        var snapshot = SecureUxThemeService.Query();
        if (wantOn)
        {
            var obstacle = SecureUxThemeService.EvaluateActivationObstacle(snapshot);
            if (obstacle != SecureUxThemeObstacle.None)
            {
                await ShowSignatureMessageAsync(
                    SecureUxThemeService.ObstacleMessage(obstacle, snapshot.LocalDllPath)).ConfigureAwait(true);
                SyncSignatureToggleFromSystem();
                return;
            }

            var confirmed = await HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
            {
                Title = GlassSettingsResources.Get("Dialog_SignatureTitle"),
                Body = HostedConfirmationDialogService.CreateBody(
                    GlassSettingsResources.Get("Dialog_SignatureActivateConfirm")),
                Footer = HostedConfirmationDialogService.CreateCancelConfirmFooter(
                    GlassSettingsResources.Get("Dialog_SignatureConfirm"))
            }).ConfigureAwait(true);
            if (!confirmed || !IsLoaded)
            {
                SyncSignatureToggleFromSystem();
                return;
            }

            var reactivate = SecureUxThemeService.CheckCleanupPending();
            var requiresRestart = SecureUxThemeService.ActivationRequiresRestart(snapshot, reactivate);
            var activateBase = GlassSettingsResources.Get("Themes_SignatureActivated");
            var restartSuffix = requiresRestart
                ? GlassSettingsResources.Get("Themes_RestartRequired")
                : GlassSettingsResources.Get("Themes_RestartNotRequired");
            var activateMessage = activateBase + " " + restartSuffix;
            await RunSignatureOperationAsync(
                () => SecureUxThemeService.Activate(snapshot, reactivate),
                SettingsNoticeSeverity.Success,
                activateMessage,
                "Themes_SignatureActivateFailedFormat",
                requiresRestart: requiresRestart,
                restartMessageKey: null,
                restartButtonTextKey: null).ConfigureAwait(true);
            return;
        }

        var deactivateConfirmed = await HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
        {
            Title = GlassSettingsResources.Get("Dialog_SignatureTitle"),
            Body = HostedConfirmationDialogService.CreateBody(
                GlassSettingsResources.Get("Dialog_SignatureDeactivateConfirm")),
            Footer = HostedConfirmationDialogService.CreateCancelConfirmFooter(
                GlassSettingsResources.Get("Dialog_SignatureConfirm"))
        }).ConfigureAwait(true);
        if (!deactivateConfirmed || !IsLoaded)
        {
            SyncSignatureToggleFromSystem();
            return;
        }

        if (IsCustomThemeActive())
        {
            var stock = SecureUxThemeService.TryGetStockThemeEntry();
            if (stock is null)
            {
                await ShowThemesNoticeAsync(
                    SettingsNoticeSeverity.Error,
                    GlassSettingsResources.Get("Themes_SignatureStockThemeFailed")).ConfigureAwait(true);
                SyncSignatureToggleFromSystem();
                return;
            }

            var apply = await VisualStyleThemeManager.ApplyThemeAsync(stock).ConfigureAwait(true);
            if (!apply.Success)
            {
                await ShowThemesNoticeAsync(
                    SettingsNoticeSeverity.Error,
                    GlassSettingsResources.Get("Themes_SignatureStockThemeFailed")).ConfigureAwait(true);
                SyncSignatureToggleFromSystem();
                return;
            }
        }

        _signatureBusy = true;
        SignatureToggle.IsEnabled = false;
        SignatureToggle.SyncInnerEnabled();
        try
        {
            var result = await Task.Run(SecureUxThemeService.DeactivateWithCleanup).ConfigureAwait(true);
            if (!IsLoaded)
                return;

            if (result.Hr < 0)
            {
                await ShowThemesNoticeAsync(
                    SettingsNoticeSeverity.Error,
                    GlassSettingsResources.Format(
                        "Themes_SignatureDeactivateFailedFormat",
                        unchecked((uint)result.Hr))).ConfigureAwait(true);
                return;
            }

            var deactivateBase = GlassSettingsResources.Get("Themes_SignatureDeactivated");
            var restartSuffix = result.RebootRequired
                ? GlassSettingsResources.Get("Themes_RestartRequired")
                : GlassSettingsResources.Get("Themes_RestartNotRequired");
            var deactivateMessage = deactivateBase + " " + restartSuffix;
            if (result.RebootRequired)
            {
                await ShowRebootNoticeAsync(
                    deactivateMessage,
                    GlassSettingsResources.Get("Themes_SignatureRebootButton")).ConfigureAwait(true);
            }
            else
            {
                await ShowThemesNoticeAsync(
                    SettingsNoticeSeverity.Success,
                    deactivateMessage).ConfigureAwait(true);
            }
        }
        catch (Exception ex)
        {
            if (!IsLoaded)
                return;

            await ShowThemesNoticeAsync(
                SettingsNoticeSeverity.Error,
                GlassSettingsResources.Format(
                    "Themes_SignatureDeactivateFailedFormat",
                    unchecked((uint)ex.HResult))).ConfigureAwait(true);
            Debug.WriteLine($"GlassSettings VisualStylesPage.SignatureToggle_OnToggled: {ex}");
        }
        finally
        {
            _signatureBusy = false;
            if (IsLoaded)
                SyncSignatureToggleFromSystem();
        }
    }

    private async Task RunSignatureOperationAsync(
        Func<int> operation,
        SettingsNoticeSeverity successSeverity,
        string successMessage,
        string failedFormatKey,
        bool requiresRestart = false,
        string? restartMessageKey = null,
        string? restartButtonTextKey = null)
    {
        _signatureBusy = true;
        SignatureToggle.IsEnabled = false;
        SignatureToggle.SyncInnerEnabled();
        try
        {
            var hr = await Task.Run(operation).ConfigureAwait(true);
            if (!IsLoaded)
                return;

            if (hr < 0)
            {
                await ShowThemesNoticeAsync(
                    SettingsNoticeSeverity.Error,
                    GlassSettingsResources.Format(failedFormatKey, unchecked((uint)hr))).ConfigureAwait(true);
                return;
            }

            if (requiresRestart && restartMessageKey is not null && restartButtonTextKey is not null)
            {
                await ShowRebootNoticeAsync(
                    GlassSettingsResources.Get(restartMessageKey),
                    GlassSettingsResources.Get(restartButtonTextKey)).ConfigureAwait(true);
            }
            else
            {
                await ShowThemesNoticeAsync(successSeverity, successMessage).ConfigureAwait(true);
            }
        }
        catch (Exception ex)
        {
            if (!IsLoaded)
                return;

            await ShowThemesNoticeAsync(
                SettingsNoticeSeverity.Error,
                GlassSettingsResources.Format(failedFormatKey, unchecked((uint)ex.HResult))).ConfigureAwait(true);
            Debug.WriteLine($"GlassSettings VisualStylesPage.RunSignatureOperationAsync: {ex}");
        }
        finally
        {
            _signatureBusy = false;
            if (IsLoaded)
                SyncSignatureToggleFromSystem();
        }
    }

    private static bool IsCustomThemeActive()
    {
        if (!VisualStyleThemeManager.TryGetCurrentThemeFilePath(out var themePath))
            return false;

        if (VisualStyleCatalog.TryReadVisualStylePath(themePath, out var msstyles)
            && VisualStyleCatalog.IsCustomVisualStylePath(msstyles))
        {
            return true;
        }

        var entry = VisualStyleCatalog.TryGetEntryForThemeFile(themePath);
        return entry?.IsCustom == true;
    }

    private async Task ShowSignatureMessageAsync(string body)
    {
        await HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
        {
            Title = GlassSettingsResources.Get("Dialog_SignatureTitle"),
            Body = HostedConfirmationDialogService.CreateBody(body),
            Footer = HostedConfirmationDialogService.CreateFooter(
                GlassSettingsResources.Get("CmdOk"))
        }).ConfigureAwait(true);
    }

    private void BackgroundCard_OnClick(object sender, RoutedEventArgs e) =>
        App.ShellWindow?.NavigateToThemesDesktopBackgroundPage();

    private void TextFontsCard_OnClick(object sender, RoutedEventArgs e) =>
        App.ShellWindow?.NavigateToTextPage();

    private void ColorCard_OnClick(object sender, RoutedEventArgs e) =>
        App.ShellWindow?.NavigateToThemesColorsPage();

    private void UsePersonalizedThemeButton_OnClick(object sender, RoutedEventArgs e)
    {
        var entry = UserThemeCatalog.TryGetPersonalizedThemeEntry();
        if (entry is null)
        {
            _ = ShowThemesNoticeAsync(
                SettingsNoticeSeverity.Error,
                GlassSettingsResources.Get("Themes_UsePersonalizedThemeMissing"));
            return;
        }

        _ = OnThemeSelectedAsync(entry);
    }

    private async void SaveThemeButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (_saveInProgress)
            return;

        var nameBox = new TextBox
        {
            Text = UserThemeSaveService.SuggestThemeName(),
            MinWidth = 280
        };

        var dialog = new ContentDialog
        {
            Title = GlassSettingsResources.Get("Themes_SaveThemeDialogTitle"),
            Content = nameBox,
            PrimaryButtonText = GlassSettingsResources.Get("Themes_SaveTheme"),
            CloseButtonText = GlassSettingsResources.Get("Themes_SaveThemeCancel"),
            DefaultButton = ContentDialogButton.Primary,
            XamlRoot = XamlRoot
        };

        var result = await dialog.ShowAsync();
        if (result != ContentDialogResult.Primary || !IsLoaded)
            return;

        var name = nameBox.Text?.Trim();
        if (string.IsNullOrWhiteSpace(name))
            return;

        _saveInProgress = true;
        SaveThemeButton.IsEnabled = false;
        try
        {
            var hwnd = IntPtr.Zero;
            if (App.ShellWindow is { } window)
                hwnd = WindowNative.GetWindowHandle(window);

            var save = await UserThemeSaveService.SaveNamedThemeAsync(hwnd, name).ConfigureAwait(true);
            if (!IsLoaded)
                return;

            if (save.Success)
            {
                OpenGlassThemeSection.CaptureSaveBaselineFromLive();
                await ShowThemesNoticeAsync(
                    SettingsNoticeSeverity.Success,
                    GlassSettingsResources.Format("Themes_SaveThemeSuccessFormat", name)).ConfigureAwait(true);
                LoadThemes();
            }
            else
            {
                await ShowThemesNoticeAsync(
                    SettingsNoticeSeverity.Error,
                    GlassSettingsResources.Format(
                        "Themes_SaveThemeFailedFormat",
                        save.ErrorDetail ?? string.Empty)).ConfigureAwait(true);
            }
        }
        catch (Exception ex)
        {
            if (!IsLoaded)
                return;

            await ShowThemesNoticeAsync(
                SettingsNoticeSeverity.Error,
                GlassSettingsResources.Format("Themes_SaveThemeFailedFormat", ex.Message)).ConfigureAwait(true);
            Debug.WriteLine($"GlassSettings VisualStylesPage.SaveThemeButton_OnClick: {ex}");
        }
        finally
        {
            _saveInProgress = false;
            if (IsLoaded)
            {
                SaveThemeButton.IsEnabled = true;
                UpdateSaveOrPersonalizedVisibility();
            }
        }
    }

    private async void VisualStyleTile_OnDeleteRequested(object sender, VisualStyleThemeEntry entry)
    {
        var plan = ThemeDeleteService.TryCreatePlan(entry);
        if (plan is null)
            return;

        var confirmed = await ThemeDeleteDialogService.ConfirmAsync(plan).ConfigureAwait(true);
        if (!confirmed || !IsLoaded)
            return;

        var delete = ThemeDeleteService.Execute(plan);
        if (!IsLoaded)
            return;

        if (delete.Success)
        {
            await ShowThemesNoticeAsync(
                SettingsNoticeSeverity.Success,
                GlassSettingsResources.Format("Themes_DeleteSuccessFormat", entry.DisplayName)).ConfigureAwait(true);
            LoadThemes();
            UpdateSaveOrPersonalizedVisibility();
        }
        else
        {
            await ShowThemesNoticeAsync(
                SettingsNoticeSeverity.Error,
                GlassSettingsResources.Format(
                    "Themes_DeleteFailedFormat",
                    delete.ErrorDetail ?? string.Empty)).ConfigureAwait(true);
        }
    }

    private void BrowseThemesButton_OnClick(object sender, RoutedEventArgs e)
    {
        var flyout = new MenuFlyout();
        flyout.Items.Add(CreateBrowseItem(
            GlassSettingsResources.Get("Themes_BrowseSite_7themes"),
            "https://7themes.su/load"));
        flyout.Items.Add(CreateBrowseItem(
            GlassSettingsResources.Get("Themes_BrowseSite_vsthemes"),
            "https://vsthemes.org/en/themes/"));
        flyout.ShowAt(BrowseThemesButton);
    }

    private static MenuFlyoutItem CreateBrowseItem(string label, string url)
    {
        var item = new MenuFlyoutItem { Text = label };
        item.Click += (_, _) =>
        {
            try
            {
                Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"GlassSettings BrowseThemes: {ex}");
            }
        };
        return item;
    }

    private void SecureUxThemeOnlineLink_OnClick(object sender, RoutedEventArgs e)
    {
        try
        {
            Process.Start(new ProcessStartInfo("https://github.com/namazso/SecureUxTheme")
            {
                UseShellExecute = true
            });
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"GlassSettings SecureUxThemeOnline: {ex}");
        }
    }

    /// <summary>
    /// Action-слот стека: Success без кнопок — MirrorOnly; Error — PinnedLayout (полоса + зеркало off-screen).
    /// </summary>
    private async Task ShowThemesNoticeAsync(SettingsNoticeSeverity severity, string message)
    {
        var stack = PageShell.PageNoticeStack;
        var strip = stack.ActionNotice;
        strip.AllowDismiss = true;
        strip.ShowSaveButton = false;
        strip.ShowActionButton = false;

        if (strip.IsVisuallyOpen)
            NoticeViewportMirrorService.ClosePinnedSourcePreservingScroll(strip);

        if (NoticePresentationPolicy.IsInformationalMirrorOnly(severity, showSave: false, showAction: false))
            strip.Visibility = Visibility.Collapsed;
        else
            strip.Visibility = Visibility.Visible;

        var host = SettingsNoticeCoordinator.CreateShellHost(stack, PageShell.PageScrollViewer);
        await SettingsNoticeCoordinator.PresentAsync(host, new SettingsNoticeCoordinator.NoticePresentRequest
        {
            Strip = strip,
            Severity = severity,
            Message = message,
            ShowSave = false,
            ShowAction = false,
            Animate = true
        }).ConfigureAwait(true);
    }

    private async Task ShowRebootNoticeAsync(string message, string buttonText)
    {
        var stack = PageShell.PageNoticeStack;
        var strip = stack.ActionNotice;

        if (_rebootActionHandler is not null)
            strip.ActionRequested -= _rebootActionHandler;

        strip.AllowDismiss = true;
        strip.ShowSaveButton = false;
        strip.ShowActionButton = true;
        strip.ActionButtonText = buttonText;

        if (strip.IsVisuallyOpen)
            NoticeViewportMirrorService.ClosePinnedSourcePreservingScroll(strip);

        strip.Visibility = Visibility.Visible;

        _rebootActionHandler = (_, _) =>
        {
            _ = RebootSystemAsync();
        };
        strip.ActionRequested += _rebootActionHandler;

        var host = SettingsNoticeCoordinator.CreateShellHost(stack, PageShell.PageScrollViewer);
        await SettingsNoticeCoordinator.PresentAsync(host, new SettingsNoticeCoordinator.NoticePresentRequest
        {
            Strip = strip,
            Severity = SettingsNoticeSeverity.Warning,
            Message = message,
            ShowSave = false,
            ShowAction = true,
            ActionButtonText = buttonText,
            Animate = true
        }).ConfigureAwait(true);
    }

    private static async Task RebootSystemAsync()
    {
        await Task.Run(() =>
        {
            using var process = new Process();
            process.StartInfo = new ProcessStartInfo
            {
                FileName = "shutdown",
                Arguments = "/r /t 0 /d p:0:0",
                CreateNoWindow = true,
                UseShellExecute = false
            };
            process.Start();
        }).ConfigureAwait(false);
    }
}
