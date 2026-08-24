using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Threading.Tasks;
using GlassSettings.Core.Customization;
using GlassSettings.Core.Layout;
using GlassSettings.Controls;
using GlassSettings.Core.Dwm;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Snapshot;
using GlassSettings.Strings;
using GlassSettings.Services;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;
using Windows.Foundation;

namespace GlassSettings.Pages.Dwm;

public sealed partial class DwmThemeDetailPage : Page, ISettingsCommitScopeProvider, IExternalRegistrySyncReloadable
{
    private const string ReflectionRegistryKey = "CustomThemeReflection";
    private const string MaterialRegistryKey = "CustomThemeMaterial";
    private const string AtlasRegistryKey = "CustomThemeAtlas";
    private const string LegacyAtlasRegistryKey = "AtlasThemeImage";
    private const string ReflectionIntensityKey = "ColorizationGlassReflectionIntensity";
    private const string MaterialOpacityKey = "MaterialOpacity";
    private const string ParallaxKey = "ColorizationGlassReflectionParallaxIntensity";
    private const string PolicyRegistryKey = "ColorizationGlassReflectionPolicy";
    private const string DefaultReflectionTilePath = "__glasssettings_default_reflection__";
    private const string DefaultMaterialTilePath = "__glasssettings_default_material__";
    private const double HeroColumnGap = 24;
    private const double PolicyCardStackedThreshold = 450;
    private const double PolicyCardBesidePreviewMinWidth = 340;
    private const double PolicyLayoutHysteresis = 16;
    private const double GridTileWidth = 150;
    private const double GridTileGap = 12;
    private const int MaxTileColumns = 5;

    private readonly ObservableCollection<ReflectionImageTileModel> _reflectionItems = [];
    private readonly ObservableCollection<ReflectionImageTileModel> _materialItems = [];
    private readonly ObservableCollection<AtlasImageTileModel> _atlasItems = [];
    private readonly DwmOpacityGroupRow _reflectionRow;
    private readonly DwmOpacityGroupRow _reflectionMaxRow;
    private Slider _intensitySlider = null!;
    private TextBlock _intensityValueText = null!;
    private Slider _parallaxSlider = null!;
    private TextBlock _parallaxValueText = null!;
    private Slider _materialOpacitySlider = null!;
    private TextBlock _materialOpacityValueText = null!;
    private Grid _reflectionHeaderSlider = null!;
    private Grid _materialHeaderSlider = null!;
    private Grid _parallaxHeaderSlider = null!;
    private Button? _exportAtlasButton;
    private FontIcon? _exportAtlasIcon;
    private TextBlock? _exportAtlasLabel;
    private const string ExportAtlasGlyph = "\uE74E";
    private bool _inactive;
    private bool _loading;
    private int _lastPivotIndex;
    private string? _currentReflectionPath;
    private string? _currentMaterialPath;
    private string? _currentAtlasPath;
    private bool _heroSideBySide = true;
    private bool _policyLayoutUpdating;
    private PolicyLayoutMode _policyLayoutMode = PolicyLayoutMode.Inline;
    private PolicyChromeLayoutState? _appliedLayoutState;
    private bool? _policyCardHeightLocked;
    private EventHandler<DesktopEnvironmentChangedEventArgs>? _environmentChangedHandler;
    private DispatcherQueueTimer? _wallpaperRefreshTimer;
    private DispatcherQueueTimer? _themeReloadTimer;
    private int _themeReloadGeneration;
    private string? _lastThemeFilePath;
    private DwmRegistryHive _hive = DwmRegistryHive.Hkcu;

    private readonly struct PolicyChromeLayoutState(
        bool heroSideBySide,
        PolicyLayoutMode mode) : IEquatable<PolicyChromeLayoutState>
    {
        public bool HeroSideBySide { get; } = heroSideBySide;
        public PolicyLayoutMode Mode { get; } = mode;

        public bool Equals(PolicyChromeLayoutState other) =>
            HeroSideBySide == other.HeroSideBySide && Mode == other.Mode;

        public override bool Equals(object? obj) => obj is PolicyChromeLayoutState other && Equals(other);

        public override int GetHashCode() => HashCode.Combine(HeroSideBySide, Mode);
    }

    public DwmThemeDetailPage()
    {
        InitializeComponent();
        ConfigureSliderUi();
        ApplyLocalizedStrings();
        ConfigureCollectionExpanders();

        _reflectionRow = CreateOpacityRow(GlassSettingsResources.Get("Dwm_ReflectionOpacity"), "ColorizationGlassReflectionOpacity");
        _reflectionMaxRow = CreateOpacityRow(GlassSettingsResources.Get("Dwm_ReflectionOpacityMax"), "ColorizationGlassReflectionOpacityMaximized");
        _reflectionRow.Hive = _hive;
        _reflectionMaxRow.Hive = _hive;
        OpacityTabPanel.Children.Add(_reflectionRow);
        OpacityTabPanel.Children.Add(_reflectionMaxRow);
        ApplyUnsavedScopes();

        ReflectionItems.ItemsSource = _reflectionItems;
        MaterialItems.ItemsSource = _materialItems;
        AtlasItems.ItemsSource = _atlasItems;

        HeroLayoutHost.SizeChanged += (_, _) => UpdateHeroLayout();
        SizeChanged += (_, _) => SyncCollectionHeaderSliderLayout();

        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        _hive = DwmNavigationContext.FromNavigationParameter(e.Parameter).Hive;
        HiveScopeBanner.Hive = _hive;
        ApplyUnsavedScopes();
        _reflectionRow.Hive = _hive;
        _reflectionMaxRow.Hive = _hive;
        RefreshParallelBranchWarnings();
        _ = RefreshHkcuConflictNoticeAsync();
    }

    private void ConfigureSliderUi()
    {
        ParallaxRow.IconGlyph = SettingsSectionGlyphs.Animation;

        _intensitySlider = SettingsSliderFactory.Create();
        _intensityValueText = CreateSliderValueText();
        _reflectionHeaderSlider = SettingsSliderFactory.CreateSliderValueSuffixPanel(
            _intensitySlider,
            _intensityValueText);

        _materialOpacitySlider = SettingsSliderFactory.Create();
        _materialOpacityValueText = CreateSliderValueText();
        _materialHeaderSlider = SettingsSliderFactory.CreateSliderValueSuffixPanel(
            _materialOpacitySlider,
            _materialOpacityValueText);

        ReflectionCollectionExpander.HeaderControlsTable.SetCell(
            0,
            GlassSettingsResources.Get("Dwm_Intensity"),
            _reflectionHeaderSlider);
        MaterialCollectionExpander.HeaderControlsTable.SetCell(
            0,
            GlassSettingsResources.Get("Dwm_Intensity"),
            _materialHeaderSlider);

        _parallaxSlider = SettingsSliderFactory.Create();
        _parallaxValueText = CreateSliderValueText();
        _parallaxHeaderSlider = SettingsSliderFactory.CreateSliderValueSuffixPanel(
            _parallaxSlider,
            _parallaxValueText);
        ParallaxRow.HeaderControlsTable.SetCell(
            0,
            GlassSettingsResources.Get("Dwm_Intensity"),
            _parallaxHeaderSlider);

        AttachGateSliders();
    }

    private void AttachGateSliders()
    {
        SettingsSliderCommit.Attach(
            _intensitySlider,
            () => !_loading && ReflectionCollectionExpander.IsGateOn,
            _ =>
            {
                UpdateValueLabels();
                LayeredPreview.RefreshLayerEffects();
            },
            value => SettingsSession.Current.WriteDwmDwordOrDelete(
                _hive,
                ReflectionIntensityKey, (uint)value, v => v == 0, livePreview: true),
            SettingsSliderCommit.CommitMode.LiveImmediate,
            onGestureCommitted: () => SettingsSession.Current.MarkDirty());

        SettingsSliderCommit.Attach(
            _parallaxSlider,
            () => !_loading && ParallaxRow.IsGateOn,
            _ => UpdateValueLabels(),
            value => SettingsSession.Current.WriteDwmDwordOrDelete(
                _hive,
                ParallaxKey, (uint)value, v => v == 13, livePreview: true),
            SettingsSliderCommit.CommitMode.LiveImmediate,
            onGestureCommitted: () => SettingsSession.Current.MarkDirty());

        SettingsSliderCommit.Attach(
            _materialOpacitySlider,
            () => !_loading && MaterialCollectionExpander.IsGateOn,
            _ =>
            {
                UpdateValueLabels();
                LayeredPreview.RefreshLayerEffects();
            },
            value => SettingsSession.Current.WriteDwmDwordOrDelete(
                _hive,
                MaterialOpacityKey, (uint)value, v => v == 0, livePreview: true),
            SettingsSliderCommit.CommitMode.LiveImmediate,
            onGestureCommitted: () => SettingsSession.Current.MarkDirty());
    }

    private static TextBlock CreateSliderValueText() =>
        new() { Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"], VerticalAlignment = VerticalAlignment.Center };

    private void ApplyLocalizedStrings()
    {
        ParallaxRow.Title = GlassSettingsResources.Get("Dwm_ReflectionParallax");
        ParallaxRow.Description = GlassSettingsResources.Get("Dwm_ReflectionParallaxDesc");
        PolicyTitleText.Text = GlassSettingsResources.Get("Dwm_ReflectionPolicyLabel");
        PolicyDescText.Text = GlassSettingsResources.Get("Dwm_ReflectionPolicyDesc");
        SetPolicyCheckboxLabel(PolicyTitlebarCheck, GlassSettingsResources.Get("Dwm_ReflectionPolicyTitlebar"));
        SetPolicyCheckboxLabel(PolicyAeroPeekCheck, GlassSettingsResources.Get("Dwm_ReflectionPolicyAeroPeek"));
        SetPolicyCheckboxLabel(PolicyAeroSnapCheck, GlassSettingsResources.Get("Dwm_ReflectionPolicyAeroSnap"));
        EmptyText.Text = GlassSettingsResources.Get("Dwm_ReflectionEmpty");
        MaterialEmptyText.Text = GlassSettingsResources.Get("Dwm_MaterialEmpty");
        AtlasEmptyText.Text = GlassSettingsResources.Get("Dwm_AtlasEmpty");
        ReflectionInfoBlock.Title = GlassSettingsResources.Get("Dwm_ReflectionInfoTitle");
        ReflectionInfoBlock.Message = GlassSettingsResources.Get("Dwm_ReflectionInfoMessage");
        AdvancedExpander.Title = GlassSettingsResources.Get("Dwm_AdvancedColorizationTitle");

        ReflectionCollectionExpander.HeaderControlsTable.GetCell(0)!.Label =
            GlassSettingsResources.Get("Dwm_Intensity");
        MaterialCollectionExpander.HeaderControlsTable.GetCell(0)!.Label =
            GlassSettingsResources.Get("Dwm_Intensity");
        ParallaxRow.HeaderControlsTable.GetCell(0)!.Label =
            GlassSettingsResources.Get("Dwm_Intensity");
        SyncCollectionHeaderSliderLayout();

        if (ActivePivot.Items.Count >= 2)
        {
            ((PivotItem)ActivePivot.Items[0]).Header = GlassSettingsResources.Get("Dwm_PivotActive");
            ((PivotItem)ActivePivot.Items[1]).Header = GlassSettingsResources.Get("Dwm_PivotInactive");
        }
    }

    private static void SetPolicyCheckboxLabel(CheckBox checkBox, string text)
    {
        checkBox.Content = new TextBlock
        {
            Text = text,
            TextTrimming = TextTrimming.CharacterEllipsis,
            TextWrapping = TextWrapping.NoWrap,
        };
    }

    private void ConfigureCollectionExpanders()
    {
        ReflectionCollectionExpander.IconGlyph = SettingsSectionGlyphs.Image;
        ReflectionCollectionExpander.Title = GlassSettingsResources.Get("Dwm_ReflectionCollection");
        ReflectionCollectionExpander.Description = GlassSettingsResources.Get("Dwm_ReflectionImageDesc");

        MaterialCollectionExpander.IconGlyph = SettingsSectionGlyphs.Image;
        MaterialCollectionExpander.Title = GlassSettingsResources.Get("Dwm_MaterialCollection");
        MaterialCollectionExpander.Description = GlassSettingsResources.Get("Dwm_MaterialCollectionDesc");

        AtlasCollectionExpander.IconGlyph = SettingsSectionGlyphs.Image;
        AtlasCollectionExpander.Title = GlassSettingsResources.Get("Dwm_AtlasCollection");
        AtlasCollectionExpander.Description = GlassSettingsResources.Get("Dwm_AtlasImageDesc");
        AtlasCollectionExpander.HeaderActions = BuildAtlasExportButton();
    }

    private Button BuildAtlasExportButton()
    {
        _exportAtlasIcon = new FontIcon { Glyph = ExportAtlasGlyph, FontSize = 14, VerticalAlignment = VerticalAlignment.Center };
        _exportAtlasLabel = new TextBlock
        {
            Text = GlassSettingsResources.Get("Dwm_ExportAtlasButton"),
            VerticalAlignment = VerticalAlignment.Center
        };

        var content = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 8,
            VerticalAlignment = VerticalAlignment.Center
        };
        content.Children.Add(_exportAtlasIcon);
        content.Children.Add(_exportAtlasLabel);

        _exportAtlasButton = new Button
        {
            Content = content,
            VerticalAlignment = VerticalAlignment.Center
        };
        ToolTipService.SetToolTip(_exportAtlasButton, GlassSettingsResources.Get("Dwm_ExportAtlasTooltip"));
        _exportAtlasButton.Click += ExportAtlasButton_OnClick;
        return _exportAtlasButton;
    }

    private async void ExportAtlasButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (_exportAtlasButton is null)
            return;

        _exportAtlasButton.IsEnabled = false;
        try
        {
            var result = await ThemeAtlasExportService.SaveCurrentThemeAtlasAsync();
            switch (result.Status)
            {
                case ThemeAtlasExportService.ExportStatus.Saved:
                    LoadAtlasImages();
                    UpdateAtlasTilesLayout();
                    await ShowExportFeedbackAsync("\uE73E", GlassSettingsResources.Get("Dwm_ExportAtlasSaved"));
                    break;
                case ThemeAtlasExportService.ExportStatus.Failed:
                    await ShowExportFeedbackAsync("\uEA39", GlassSettingsResources.Get("Dwm_ExportAtlasFailed"));
                    break;
            }
        }
        finally
        {
            _exportAtlasButton.IsEnabled = true;
        }
    }

    private async Task ShowExportFeedbackAsync(string glyph, string text)
    {
        if (_exportAtlasIcon is null || _exportAtlasLabel is null)
            return;

        _exportAtlasIcon.Glyph = glyph;
        _exportAtlasLabel.Text = text;
        await Task.Delay(2200);
        _exportAtlasIcon.Glyph = ExportAtlasGlyph;
        _exportAtlasLabel.Text = GlassSettingsResources.Get("Dwm_ExportAtlasButton");
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        SettingsSession.Current.DirtyChanged += Session_OnDirtyChanged;
        LoadReflectionImages();
        LoadMaterialImages();
        LoadAtlasImages();
        Reload();
        _lastPivotIndex = ActivePivot.SelectedIndex;
        DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, UpdateHeroLayout);
        RefreshPolicyUnsavedIndicator();
        AdvancedExpander.RefreshUnsavedIndicator();
        UpdateReflectionTilesLayout();
        UpdateMaterialTilesLayout();
        UpdateAtlasTilesLayout();
        RefreshParallelBranchWarnings();
        HookEnvironmentWatcher();
        _ = RefreshHkcuConflictNoticeAsync();
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        SettingsSession.Current.DirtyChanged -= Session_OnDirtyChanged;
        UnhookEnvironmentWatcher();
        _themeReloadTimer?.Stop();
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

        _lastThemeFilePath = TryGetCurrentThemeRegistryPathNormalized();

        _environmentChangedHandler = (_, e) =>
            DispatcherQueue.TryEnqueue(() => OnDesktopEnvironmentChanged(e));
        watcher.Changed += _environmentChangedHandler;
    }

    private static string? TryGetCurrentThemeRegistryPathNormalized()
    {
        if (!VisualStyleThemeManager.TryGetCurrentThemeRegistryFilePath(out var path)
            || string.IsNullOrWhiteSpace(path))
        {
            return null;
        }

        return path;
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

        if (e.ThemeChanged)
            ScheduleThemeChangeReload();

        if (e.WallpaperChanged)
            ScheduleWallpaperLayerRefresh();
    }

    private void ScheduleThemeChangeReload()
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
        if (!IsLoaded)
            return;

        var generation = _themeReloadGeneration;
        try
        {
            if (!IsLoaded || generation != _themeReloadGeneration)
                return;

            _lastThemeFilePath = TryGetCurrentThemeRegistryPathNormalized();
            Reload();
            RefreshPolicyUnsavedIndicator();
            PageShell.RefreshSessionNoticeProgrammatic();
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings DwmThemeDetailPage.OnThemeReloadTick: {ex}");
        }
    }

    private void ScheduleWallpaperLayerRefresh()
    {
        _wallpaperRefreshTimer ??= DispatcherQueue.CreateTimer();
        _wallpaperRefreshTimer.Interval = TimeSpan.FromMilliseconds(120);
        _wallpaperRefreshTimer.IsRepeating = false;
        _wallpaperRefreshTimer.Tick -= OnWallpaperLayerRefreshTick;
        _wallpaperRefreshTimer.Tick += OnWallpaperLayerRefreshTick;
        _wallpaperRefreshTimer.Stop();
        _wallpaperRefreshTimer.Start();
    }

    private void OnWallpaperLayerRefreshTick(DispatcherQueueTimer sender, object args)
    {
        sender.Tick -= OnWallpaperLayerRefreshTick;
        if (!IsLoaded)
            return;

        LayeredPreview.RefreshWallpaperLayer();
    }

    private void Session_OnDirtyChanged(object? sender, EventArgs e)
    {
        RefreshPolicyUnsavedIndicator();
        AdvancedExpander.RefreshUnsavedIndicator();
        RefreshParallelBranchWarnings();
        _ = RefreshHkcuConflictNoticeAsync();
    }

    private void ApplyUnsavedScopes()
    {
        AdvancedExpander.UnsavedScope = DwmThemeScopeCatalog.AdvancedScope(_hive);
        ParallaxRow.UnsavedScope = DwmThemeScopeCatalog.ParallaxScope(_hive);
        _reflectionRow.UnsavedScope = DwmThemeScopeCatalog.ReflectionOpacityScope(_hive);
        _reflectionMaxRow.UnsavedScope = DwmThemeScopeCatalog.ReflectionMaxOpacityScope(_hive);
        ReflectionCollectionExpander.UnsavedScope = SettingsCommitScope.ForDwmKeys(
            _hive,
            [ReflectionRegistryKey, ReflectionIntensityKey]);
        MaterialCollectionExpander.UnsavedScope = SettingsCommitScope.ForDwmKeys(
            _hive,
            [MaterialRegistryKey, MaterialOpacityKey]);
        AtlasCollectionExpander.UnsavedScope = DwmThemeScopeCatalog.AtlasScope(_hive);
    }

    private void RefreshParallelBranchWarnings()
    {
        ApplySectionParallelWarning(
            (visible, tooltip, marker) => ParallaxRow.SetParallelBranchWarning(visible, tooltip, marker),
            [ParallaxKey]);
        ApplySectionParallelWarning(
            (visible, tooltip, marker) => _reflectionRow.SetParallelBranchWarning(visible, tooltip, marker),
            DwmThemeScopeCatalog.ReflectionOpacityKeys);
        ApplySectionParallelWarning(
            (visible, tooltip, marker) => _reflectionMaxRow.SetParallelBranchWarning(visible, tooltip, marker),
            DwmThemeScopeCatalog.ReflectionMaxOpacityKeys);
        ApplySectionParallelWarning(
            (visible, tooltip, marker) => ReflectionCollectionExpander.SetParallelBranchWarning(visible, tooltip, marker),
            [ReflectionRegistryKey, ReflectionIntensityKey]);
        ApplySectionParallelWarning(
            (visible, tooltip, marker) => MaterialCollectionExpander.SetParallelBranchWarning(visible, tooltip, marker),
            [MaterialRegistryKey, MaterialOpacityKey]);
        ApplySectionParallelWarning(
            (visible, tooltip, marker) => AtlasCollectionExpander.SetParallelBranchWarning(visible, tooltip, marker),
            [AtlasRegistryKey, LegacyAtlasRegistryKey]);
        SyncCollectionHeaderSliderLayout();
    }

    private void SyncCollectionHeaderSliderLayout()
    {
        SpoilerHeaderControlsTable.SyncLabelColumnWidthsAcross([
            ReflectionCollectionExpander.HeaderControlsTable,
            MaterialCollectionExpander.HeaderControlsTable,
            ParallaxRow.HeaderControlsTable]);
        ReflectionCollectionExpander.RefreshHeaderActionsLayout(resetStackState: true);
        MaterialCollectionExpander.RefreshHeaderActionsLayout(resetStackState: true);
        ParallaxRow.RefreshHeaderLayout();
    }

    private void RefreshPolicyUnsavedIndicator()
    {
        var pending = SettingsSession.Current.HasUnsavedInScope(DwmThemeScopeCatalog.PolicyScope(_hive));
        PolicyUnsavedDot.Visibility = pending ? Visibility.Visible : Visibility.Collapsed;
    }

    private static DwmOpacityGroupRow CreateOpacityRow(string title, string regBase)
    {
        var row = new DwmOpacityGroupRow
        {
            Title = title,
            Description = GlassSettingsResources.Get("Dwm_ReflectionOpacityDesc"),
            IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.Opacity),
            RegistryValueName = regBase,
            UseOpenGlassOpacitySemantics = true,
            UseSliderValueSuffix = true,
            UseParentGate = false
        };
        row.BuildControls();
        return row;
    }

    private void LoadReflectionImages() =>
        PopulateCollection(
            _reflectionItems,
            BuildReflectionItemsWithDefault(),
            EmptyStateHost,
            ReflectionItems,
            UpdateReflectionTilesLayout,
            model => _ = TryDeleteReflectionImageAsync(model));

    private void LoadMaterialImages() =>
        PopulateCollection(
            _materialItems,
            BuildMaterialItemsWithDefault(),
            MaterialEmptyStateHost,
            MaterialItems,
            UpdateMaterialTilesLayout,
            model => _ = TryDeleteMaterialImageAsync(model));

    private void LoadAtlasImages()
    {
        var items = ThemeImageCatalog.LoadAtlasImages();
        AtlasPreviewTrace.PageLoad(items.Count);
        PopulateAtlasCollection(items);

        if (items.Count > 0)
            _ = GenerateAtlasPreviewsAsync(items);
    }

    private void PopulateAtlasCollection(IReadOnlyList<ThemeImageItem> items)
    {
        _atlasItems.Clear();

        if (items.Count == 0)
        {
            AtlasEmptyStateHost.Visibility = Visibility.Visible;
            AtlasItems.Visibility = Visibility.Collapsed;
            return;
        }

        foreach (var item in items)
        {
            var model = new AtlasImageTileModel(item);
            model.DeleteRequested += (_, _) => _ = TryDeleteAtlasImageAsync(model);
            _atlasItems.Add(model);
        }

        AtlasEmptyStateHost.Visibility = Visibility.Collapsed;
        AtlasItems.Visibility = Visibility.Visible;
        AtlasPreviewTrace.PagePopulate(_atlasItems.Count);
        UpdateAtlasTilesLayout();
    }

    private async System.Threading.Tasks.Task GenerateAtlasPreviewsAsync(IReadOnlyList<ThemeImageItem> items)
    {
        foreach (var item in items)
        {
            try
            {
                var pngBytes = await System.Threading.Tasks.Task.Run(() =>
                    ThemeImageCatalog.TryGenerateAtlasPreviewBytes(item.FilePath));

                Microsoft.UI.Xaml.Media.ImageSource? image = null;
                if (pngBytes is not null)
                    image = await ThemeImageCatalog.DecodeAtlasPreviewAsync(pngBytes);

                var finalImage = image;
                DispatcherQueue.TryEnqueue(() =>
                {
                    var model = _atlasItems.FirstOrDefault(m => m.FilePath == item.FilePath);
                    if (model == null)
                    {
                        AtlasPreviewTrace.UiUpdate(item.Title, "skip", "model not found in collection");
                        return;
                    }

                    if (finalImage != null)
                    {
                        model.UpdateImage(finalImage);
                        AtlasPreviewTrace.UiUpdate(item.Title, "Ready");
                    }
                    else
                    {
                        model.SetPreviewFailed();
                        AtlasPreviewTrace.UiUpdate(item.Title, "Failed");
                    }
                });
            }
            catch (Exception ex)
            {
                AtlasPreviewTrace.Exception($"GenerateAtlasPreviewsAsync:{item.Title}", ex);
                DispatcherQueue.TryEnqueue(() =>
                {
                    var model = _atlasItems.FirstOrDefault(m => m.FilePath == item.FilePath);
                    model?.SetPreviewFailed();
                    AtlasPreviewTrace.UiUpdate(item.Title, "Failed", ex.Message);
                });
            }
        }
    }

    private static void PopulateCollection(
        ObservableCollection<ReflectionImageTileModel> target,
        IReadOnlyList<ThemeImageItem> items,
        Border emptyHost,
        ItemsView itemsView,
        Action updateLayout,
        Action<ReflectionImageTileModel> onDeleteRequested)
    {
        target.Clear();

        if (items.Count == 0)
        {
            emptyHost.Visibility = Visibility.Visible;
            itemsView.Visibility = Visibility.Collapsed;
            return;
        }

        foreach (var item in items)
        {
            var model = new ReflectionImageTileModel(item);
            model.DeleteRequested += (_, _) => onDeleteRequested(model);
            target.Add(model);
        }

        emptyHost.Visibility = Visibility.Collapsed;
        itemsView.Visibility = Visibility.Visible;
        updateLayout();
    }

    private static IReadOnlyList<ThemeImageItem> BuildReflectionItemsWithDefault()
    {
        var items = ThemeImageCatalog.LoadReflectionImages().ToList();
        var defaultImage = ThemeImageCatalog.TryGetDefaultReflectionImage();
        if (defaultImage is null)
            return items;

        items.Insert(0, new ThemeImageItem
        {
            Title = GlassSettingsResources.Get("Dwm_DefaultReflectionTile"),
            FilePath = DefaultReflectionTilePath,
            Image = defaultImage
        });
        return items;
    }

    private static IReadOnlyList<ThemeImageItem> BuildMaterialItemsWithDefault()
    {
        var items = ThemeImageCatalog.LoadMaterialImages().ToList();
        var defaultImage = ThemeImageCatalog.TryGetDefaultMaterialImage();
        if (defaultImage is null)
            return items;

        items.Insert(0, new ThemeImageItem
        {
            Title = GlassSettingsResources.Get("Dwm_DefaultMaterialTile"),
            FilePath = DefaultMaterialTilePath,
            Image = defaultImage
        });
        return items;
    }

    private void Reload()
    {
        _loading = true;
        _currentReflectionPath = NormalizePath(DwmRegistryStore.ReadStringForPage(_hive, ReflectionRegistryKey));
        _currentMaterialPath = NormalizePath(DwmRegistryStore.ReadStringForPage(_hive, MaterialRegistryKey));
        _currentAtlasPath = NormalizePath(DwmRegistryStore.ReadStringForPage(_hive, AtlasRegistryKey));
        if (string.IsNullOrWhiteSpace(_currentAtlasPath))
            _currentAtlasPath = NormalizePath(DwmRegistryStore.ReadStringForPage(_hive, LegacyAtlasRegistryKey));
        ApplyOpacityRegistryNames();

        ReflectionCollectionExpander.IsGateOn = IsReflectionCollectionGateOn();
        MaterialCollectionExpander.IsGateOn = IsMaterialCollectionGateOn();
        ReloadIntensityRow();
        ReloadParallaxRow();
        ReloadMaterialOpacityRow();
        ReloadPolicy();
        _reflectionRow.Reload();
        _reflectionMaxRow.Reload();

        AtlasCollectionExpander.IsGateOn = _atlasItems.Count > 0
            || DwmRegistryStore.HasAnyPresentForPage(
                _hive,
                [AtlasRegistryKey, LegacyAtlasRegistryKey]);
        ApplyPolicyGateVisual(PolicyGateSwitch.IsOn);

        UpdateValueLabels();
        UpdateReflectionTileSelection();
        UpdateMaterialTileSelection();
        UpdateAtlasTileSelection();
        RefreshParallelBranchWarnings();
        _ = RefreshHkcuConflictNoticeAsync();
        LayeredPreview.RefreshLayers();
        _loading = false;
    }

    private bool IsReflectionCollectionGateOn() =>
        DwmRegistryStore.HasAnyPresentForPage(_hive, [ReflectionRegistryKey, ReflectionIntensityKey]);

    private bool IsMaterialCollectionGateOn() =>
        DwmRegistryStore.HasAnyPresentForPage(_hive, [MaterialRegistryKey, MaterialOpacityKey]);

    private void ReloadIntensityRow()
    {
        var opt = DwmRegistryStore.ReadDwordForPage(_hive, ReflectionIntensityKey);
        _intensitySlider.Value = Math.Clamp(opt ?? DwmDefaults.DefaultWhenKeyAbsent(ReflectionIntensityKey), 0u, 100u);
        _intensitySlider.IsEnabled = ReflectionCollectionExpander.IsGateOn;
        SettingsSliderCommit.SyncCommittedValue(_intensitySlider, _intensitySlider.Value);
    }

    private void ReloadParallaxRow()
    {
        var opt = DwmRegistryStore.ReadDwordForPage(_hive, ParallaxKey);
        ParallaxRow.IsGateOn = DwmRegistryStore.ValueExistsForPage(_hive, ParallaxKey);
        _parallaxSlider.Value = Math.Clamp(opt ?? DwmDefaults.DefaultWhenKeyAbsent(ParallaxKey), 0u, 100u);
        _parallaxSlider.IsEnabled = ParallaxRow.IsGateOn;
        SettingsSliderCommit.SyncCommittedValue(_parallaxSlider, _parallaxSlider.Value);
    }

    private void ReloadMaterialOpacityRow()
    {
        var opt = DwmRegistryStore.ReadDwordForPage(_hive, MaterialOpacityKey);
        _materialOpacitySlider.Value = Math.Clamp(opt ?? DwmDefaults.DefaultWhenKeyAbsent(MaterialOpacityKey), 0u, 100u);
        _materialOpacitySlider.IsEnabled = MaterialCollectionExpander.IsGateOn;
        SettingsSliderCommit.SyncCommittedValue(_materialOpacitySlider, _materialOpacitySlider.Value);
    }

    private void UpdateReflectionTileSelection() =>
        UpdateDefaultAwareTileSelection(_reflectionItems, _currentReflectionPath, DefaultReflectionTilePath);

    private void UpdateMaterialTileSelection() =>
        UpdateDefaultAwareTileSelection(_materialItems, _currentMaterialPath, DefaultMaterialTilePath);

    private void UpdateAtlasTileSelection() =>
        UpdateAtlasTileSelection(_atlasItems, _currentAtlasPath);

    private static void UpdateDefaultAwareTileSelection(
        ObservableCollection<ReflectionImageTileModel> items,
        string? currentPath,
        string defaultTilePath)
    {
        var hasCustomPath = !string.IsNullOrWhiteSpace(currentPath);
        for (var i = 0; i < items.Count; i++)
        {
            var item = items[i];
            item.IsSelected = string.Equals(item.FilePath, defaultTilePath, StringComparison.Ordinal)
                ? !hasCustomPath
                : hasCustomPath && SamePath(item.FilePath, currentPath);
        }
    }

    private static void UpdateAtlasTileSelection(ObservableCollection<AtlasImageTileModel> items, string? currentPath)
    {
        for (var i = 0; i < items.Count; i++)
        {
            var item = items[i];
            item.IsSelected = !string.IsNullOrWhiteSpace(currentPath)
                && SamePath(item.FilePath, currentPath);
        }
    }

    private async void ActivePivot_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading) return;

        var newIndex = ActivePivot.SelectedIndex;
        var previousIndex = _lastPivotIndex;
        if (previousIndex == newIndex) return;

        await PivotTabContentTransition.AnimateTabChangeAsync(
            TabContentHost,
            previousIndex,
            newIndex,
            () =>
            {
                _inactive = newIndex == 1;
                ApplyOpacityRegistryNames();
                _reflectionRow.Reload();
                _reflectionMaxRow.Reload();
            }).ConfigureAwait(true);

        _lastPivotIndex = newIndex;
        RefreshParallelBranchWarnings();
    }

    private void ApplyOpacityRegistryNames()
    {
        _inactive = ActivePivot.SelectedIndex == 1;
        _reflectionRow.RegistryValueName = _inactive
            ? "ColorizationGlassReflectionOpacityInactive"
            : "ColorizationGlassReflectionOpacity";
        _reflectionMaxRow.RegistryValueName = _inactive
            ? "ColorizationGlassReflectionOpacityInactiveMaximized"
            : "ColorizationGlassReflectionOpacityMaximized";
    }

    private void UpdateValueLabels()
    {
        _intensityValueText.Text = GlassSettingsResources.Format("Dwm_ReflectionPercentFormat", (uint)_intensitySlider.Value);
        _parallaxValueText.Text = GlassSettingsResources.Format("Dwm_ReflectionPercentFormat", (uint)_parallaxSlider.Value);
        _materialOpacityValueText.Text = GlassSettingsResources.Format("Dwm_ReflectionPercentFormat", (uint)_materialOpacitySlider.Value);
    }

    private void ReloadPolicy()
    {
        var policy = DwmRegistryStore.ReadDwordForPage(_hive, PolicyRegistryKey);
        var gateOn = DwmRegistryStore.ValueExistsForPage(_hive, PolicyRegistryKey);
        if (PolicyGateSwitch.IsOn != gateOn)
            PolicyGateSwitch.IsOn = gateOn;

        var titlebar = policy is null || policy == DwmDefaults.PriorityAuto || (policy.Value & 0x1) != 0;
        var aeroPeek = policy is null || policy == DwmDefaults.PriorityAuto || (policy.Value & 0x4) != 0;
        var aeroSnap = policy is null || policy == DwmDefaults.PriorityAuto || (policy.Value & 0x8) != 0;

        if (PolicyTitlebarCheck.IsChecked != titlebar)
            PolicyTitlebarCheck.IsChecked = titlebar;
        if (PolicyAeroPeekCheck.IsChecked != aeroPeek)
            PolicyAeroPeekCheck.IsChecked = aeroPeek;
        if (PolicyAeroSnapCheck.IsChecked != aeroSnap)
            PolicyAeroSnapCheck.IsChecked = aeroSnap;
    }

    private static Brush GetThemeBrush(string key) =>
        Application.Current.Resources[key] is Brush brush
            ? brush
            : new SolidColorBrush(Microsoft.UI.Colors.Gray);

    /// <summary>
    /// Gate выкл.: заголовок/описание/иконка — tertiary; весь блок чекбоксов приглушается единым
    /// Opacity и снимается hit-test. Единый вид «заблокировано» НЕ зависит от checked/unchecked
    /// (нативный IsEnabled-disabled рисует пустой и заполненный чекбоксы по-разному → один кажется
    /// тусклее). Метод вызывается только при init/toggle (не в layout) — мерцания при resize/scroll нет.
    /// Toggle снаружи gated-контента, всегда активен.
    /// </summary>
    private void ApplyPolicyGateVisual(bool gateOn)
    {
        PolicyTitleText.Foreground = GetThemeBrush(gateOn ? "TextFillColorPrimaryBrush" : "TextFillColorTertiaryBrush");
        PolicyDescText.Foreground = GetThemeBrush(gateOn ? "TextFillColorSecondaryBrush" : "TextFillColorTertiaryBrush");
        PolicyIcon.Foreground = GetThemeBrush(gateOn ? "TextFillColorSecondaryBrush" : "TextFillColorTertiaryBrush");

        PolicyTitlebarCheck.IsEnabled = true;
        PolicyAeroPeekCheck.IsEnabled = true;
        PolicyAeroSnapCheck.IsEnabled = true;
        PolicyCheckboxesGrid.Opacity = gateOn ? 1.0 : 0.5;
        PolicyCheckboxesGrid.IsHitTestVisible = gateOn;
        PolicyGateSwitch.IsEnabled = true;
    }

    private uint ComputePolicyMask()
    {
        uint mask = 0;
        if (PolicyTitlebarCheck.IsChecked == true) mask |= 0x1;
        if (PolicyAeroPeekCheck.IsChecked == true) mask |= 0x4;
        if (PolicyAeroSnapCheck.IsChecked == true) mask |= 0x8;
        return mask;
    }

    private void PolicyCheckbox_OnChanged(object sender, RoutedEventArgs e)
    {
        if (_loading || !PolicyGateSwitch.IsOn) return;
        CommitPolicyMask(ComputePolicyMask());
    }

    private void CommitPolicyMask(uint mask)
    {
        SettingsSession.Current.WriteDwmDword(
            _hive,
            PolicyRegistryKey,
            mask,
            liveDwm: true,
            dwmNotifyImmediate: true);

        PageSessionNoticeState.InvalidateDismissOnMutation();
        HomeSessionNoticeState.InvalidateDismissOnMutation();
    }

    private void AdvancedExpander_OnExpanded(object sender, RoutedEventArgs e)
    {
        if (!AdvancedExpander.IsExpanded)
            return;

        RefreshOpacityRowsHeaderLayout();
    }

    private void RefreshOpacityRowsHeaderLayout()
    {
        _reflectionRow.RefreshHeaderLayout();
        _reflectionMaxRow.RefreshHeaderLayout();
    }

    private void ApplyCollectionIntensityDefault(string intensityKey, Slider slider)
    {
        var def = DwmDefaults.DefaultWhenKeyAbsent(intensityKey);
        slider.Value = def;
        SettingsSession.Current.WriteDwmDwordLivePreview(_hive, intensityKey, def);
        SettingsSliderCommit.SyncCommittedValue(slider, slider.Value);
        UpdateValueLabels();
        SettingsSession.Current.MarkDirty();
        PageSessionNoticeState.InvalidateDismissOnMutation();
        HomeSessionNoticeState.InvalidateDismissOnMutation();
    }

    private void ReflectionCollectionExpander_OnGateToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;

        if (ReflectionCollectionExpander.IsGateOn)
        {
            _intensitySlider.IsEnabled = true;
            ApplyCollectionIntensityDefault(ReflectionIntensityKey, _intensitySlider);
            Reload();
            return;
        }

        SettingsSession.Current.DeleteDwmValue(_hive, ReflectionRegistryKey, liveDwm: true);
        SettingsSession.Current.DeleteDwmValue(_hive, ReflectionIntensityKey, liveDwm: true);
        Reload();
    }

    private void MaterialCollectionExpander_OnGateToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;

        if (MaterialCollectionExpander.IsGateOn)
        {
            _materialOpacitySlider.IsEnabled = true;
            ApplyCollectionIntensityDefault(MaterialOpacityKey, _materialOpacitySlider);
            Reload();
            return;
        }

        SettingsSession.Current.DeleteDwmValue(_hive, MaterialRegistryKey, liveDwm: true);
        SettingsSession.Current.DeleteDwmValue(_hive, MaterialOpacityKey, liveDwm: true);
        Reload();
    }

    private void ParallaxRow_OnGateToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;

        _parallaxSlider.IsEnabled = ParallaxRow.IsGateOn;
        if (ParallaxRow.IsGateOn)
        {
            SettingsSession.Current.WriteDwmDwordLivePreview(_hive, ParallaxKey, (uint)_parallaxSlider.Value);
            SettingsSession.Current.MarkDirty();
            PageSessionNoticeState.InvalidateDismissOnMutation();
            HomeSessionNoticeState.InvalidateDismissOnMutation();
            return;
        }

        SettingsSession.Current.DeleteDwmValue(_hive, ParallaxKey, liveDwm: true);
        ReloadParallaxRow();
        PageSessionNoticeState.InvalidateDismissOnMutation();
        HomeSessionNoticeState.InvalidateDismissOnMutation();
    }

    private void PolicyGateSwitch_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;

        ApplyPolicyGateVisual(PolicyGateSwitch.IsOn);

        if (_appliedLayoutState is { } layoutState)
        {
            _policyCardHeightLocked = null;
            ApplyPolicyCardHeight(layoutState);
        }

        if (!PolicyGateSwitch.IsOn)
        {
            SettingsSession.Current.DeleteDwmValue(_hive, PolicyRegistryKey, liveDwm: true);
            _loading = true;
            ReloadPolicy();
            _loading = false;
            PageSessionNoticeState.InvalidateDismissOnMutation();
            HomeSessionNoticeState.InvalidateDismissOnMutation();
            return;
        }

        CommitPolicyMask(ComputePolicyMask());
    }

    private void AtlasCollectionExpander_OnGateToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        if (AtlasCollectionExpander.IsGateOn)
            return;

        SettingsSession.Current.DeleteDwmValue(_hive, AtlasRegistryKey, liveDwm: true);
        if (DwmRegistryStore.ValueExists(_hive, LegacyAtlasRegistryKey))
            SettingsSession.Current.DeleteDwmValue(_hive, LegacyAtlasRegistryKey, liveDwm: false);
        Reload();
    }

    private void AtlasItems_OnItemInvoked(ItemsView sender, ItemsViewItemInvokedEventArgs e)
    {
        if (e.InvokedItem is not AtlasImageTileModel model) return;
        TrySelectCustomImage(AtlasRegistryKey, model.FilePath);
    }

    private void ReflectionItems_OnItemInvoked(ItemsView sender, ItemsViewItemInvokedEventArgs e)
    {
        if (e.InvokedItem is not ReflectionImageTileModel model) return;
        TrySelectCustomImage(ReflectionRegistryKey, model.FilePath);
    }

    private void MaterialItems_OnItemInvoked(ItemsView sender, ItemsViewItemInvokedEventArgs e)
    {
        if (e.InvokedItem is not ReflectionImageTileModel model) return;
        TrySelectCustomImage(MaterialRegistryKey, model.FilePath);
    }

    private async Task TryDeleteReflectionImageAsync(ReflectionImageTileModel model) =>
        await TryDeleteThemeImageAsync(
            model.Title,
            model.FilePath,
            _currentReflectionPath,
            ThemeImageCatalog.TryDeleteReflectionImage,
            LoadReflectionImages,
            skipDelete: string.Equals(model.FilePath, DefaultReflectionTilePath, StringComparison.Ordinal)).ConfigureAwait(true);

    private async Task TryDeleteMaterialImageAsync(ReflectionImageTileModel model) =>
        await TryDeleteThemeImageAsync(
            model.Title,
            model.FilePath,
            _currentMaterialPath,
            ThemeImageCatalog.TryDeleteMaterialImage,
            LoadMaterialImages,
            skipDelete: string.Equals(model.FilePath, DefaultMaterialTilePath, StringComparison.Ordinal)).ConfigureAwait(true);

    private async Task TryDeleteAtlasImageAsync(AtlasImageTileModel model) =>
        await TryDeleteThemeImageAsync(
            model.Title,
            model.FilePath,
            _currentAtlasPath,
            ThemeImageCatalog.TryDeleteAtlasImage,
            LoadAtlasImages).ConfigureAwait(true);

    private static async Task TryDeleteThemeImageAsync(
        string title,
        string filePath,
        string? currentSelectedPath,
        Func<string, bool> deleteFile,
        Action reloadCollection,
        bool skipDelete = false)
    {
        if (skipDelete)
            return;

        if (SamePath(NormalizePath(currentSelectedPath), filePath))
        {
            await ThemeImageDeleteDialogService.ShowInUseAsync(title).ConfigureAwait(true);
            return;
        }

        if (!await ThemeImageDeleteDialogService.ConfirmDeleteAsync(title).ConfigureAwait(true))
            return;

        if (!deleteFile(filePath))
            return;

        reloadCollection();
    }

    private void TrySelectCustomImage(string registryKey, string filePath)
    {
        if (IsDefaultSelection(registryKey, filePath))
        {
            SettingsSession.Current.DeleteDwmValue(_hive, registryKey, liveDwm: true);
            PageSessionNoticeState.InvalidateDismissOnMutation();
            HomeSessionNoticeState.InvalidateDismissOnMutation();
            Reload();
            PageShell.RefreshSessionNoticeProgrammatic();
            return;
        }

        SettingsSession.Current.WriteDwmString(_hive, registryKey, filePath, liveDwm: true);
        if (!SamePath(NormalizePath(DwmRegistryStore.ReadString(_hive, registryKey)), filePath))
            return;

        if (string.Equals(registryKey, AtlasRegistryKey, StringComparison.Ordinal)
            && DwmRegistryStore.ValueExists(_hive, LegacyAtlasRegistryKey))
        {
            SettingsSession.Current.DeleteDwmValue(_hive, LegacyAtlasRegistryKey, liveDwm: false);
        }

        PageSessionNoticeState.InvalidateDismissOnMutation();
        HomeSessionNoticeState.InvalidateDismissOnMutation();
        Reload();
        PageShell.RefreshSessionNoticeProgrammatic();
    }

    private static bool IsDefaultSelection(string registryKey, string filePath)
    {
        if (string.Equals(registryKey, ReflectionRegistryKey, StringComparison.Ordinal))
            return string.Equals(filePath, DefaultReflectionTilePath, StringComparison.Ordinal);

        if (string.Equals(registryKey, MaterialRegistryKey, StringComparison.Ordinal))
            return string.Equals(filePath, DefaultMaterialTilePath, StringComparison.Ordinal);

        return false;
    }

    private void ReflectionItemsHost_OnSizeChanged(object sender, SizeChangedEventArgs e) => UpdateReflectionTilesLayout();

    private void MaterialItemsHost_OnSizeChanged(object sender, SizeChangedEventArgs e) => UpdateMaterialTilesLayout();

    private void AtlasItemsHost_OnSizeChanged(object sender, SizeChangedEventArgs e) => UpdateAtlasTilesLayout();

    private void UpdateHeroLayout()
    {
        var width = HeroLayoutHost.ActualWidth;
        if (width <= 0) return;

        var minHeroBeside = HeroPreviewLayoutMetrics.FootprintWidth + HeroColumnGap + PolicyCardBesidePreviewMinWidth;

        bool sideBySide;
        if (_heroSideBySide)
        {
            // side-by-side → stacked: переход при width < minHeroBeside
            if (width < minHeroBeside)
            {
                sideBySide = false;
            }
            else
            {
                sideBySide = true;
            }
        }
        else
        {
            // stacked → side-by-side: возврат при width >= minHeroBeside (симметрично переносу)
            sideBySide = width >= minHeroBeside;
        }

        if (sideBySide != _heroSideBySide)
        {
            _heroSideBySide = sideBySide;
            _appliedLayoutState = null;
            _policyCardHeightLocked = null;

            if (sideBySide)
            {
                PreviewColumn.Width = GridLength.Auto;
                PolicyColumn.Width = new GridLength(2, GridUnitType.Star);

                Grid.SetColumn(PreviewCard, 0);
                Grid.SetRow(PreviewCard, 0);
                Grid.SetColumnSpan(PreviewCard, 1);

                Grid.SetColumn(PolicyCard, 1);
                Grid.SetRow(PolicyCard, 0);
                Grid.SetColumnSpan(PolicyCard, 1);
            }
            else
            {
                PreviewColumn.Width = GridLength.Auto;
                PolicyColumn.Width = new GridLength(1, GridUnitType.Star);

                Grid.SetColumn(PreviewCard, 0);
                Grid.SetRow(PreviewCard, 0);
                Grid.SetColumnSpan(PreviewCard, 2);

                Grid.SetColumn(PolicyCard, 0);
                Grid.SetRow(PolicyCard, 1);
                Grid.SetColumnSpan(PolicyCard, 2);
            }
        }

        UpdatePolicyLayout();
    }

    private enum PolicyLayoutMode
    {
        Inline,
        Stacked
    }

    private void UpdatePolicyLayout()
    {
        if (_policyLayoutUpdating)
            return;

        var cardWidth = PolicyCard.ActualWidth;
        if (cardWidth <= 0)
            return;

        _policyLayoutUpdating = true;
        try
        {
            var mode = ResolvePolicyLayoutMode(cardWidth);
            var next = new PolicyChromeLayoutState(_heroSideBySide, mode);

            if (_appliedLayoutState is { } applied && applied.Equals(next))
                return;

            if (_appliedLayoutState is not { } previous || previous.Mode != next.Mode)
            {
                ApplyPolicyCheckboxLayout(next.Mode);
                ApplyPolicyContentPlacement(next.Mode);
                _policyLayoutMode = next.Mode;
            }

            ApplyPolicyCardHeight(next);
            _appliedLayoutState = next;
        }
        finally
        {
            _policyLayoutUpdating = false;
        }
    }

    private PolicyLayoutMode ResolvePolicyLayoutMode(double cardWidth)
    {
        if (_policyLayoutMode == PolicyLayoutMode.Stacked)
        {
            return cardWidth >= PolicyCardStackedThreshold + PolicyLayoutHysteresis
                ? PolicyLayoutMode.Inline
                : PolicyLayoutMode.Stacked;
        }

        return cardWidth < PolicyCardStackedThreshold
            ? PolicyLayoutMode.Stacked
            : PolicyLayoutMode.Inline;
    }

    private void ApplyPolicyContentPlacement(PolicyLayoutMode mode)
    {
        if (mode == PolicyLayoutMode.Inline)
        {
            PolicyContentMainRow.Height = new GridLength(1, GridUnitType.Star);
            PolicyContentStackRow.Height = new GridLength(0);

            Grid.SetRow(PolicyTextHost, 0);
            Grid.SetColumn(PolicyTextHost, 0);
            Grid.SetColumnSpan(PolicyTextHost, 1);

            Grid.SetRow(PolicyChecksHost, 0);
            Grid.SetColumn(PolicyChecksHost, 1);
            Grid.SetColumnSpan(PolicyChecksHost, 1);

            PolicyTextColumn.Width = new GridLength(1, GridUnitType.Star);
            PolicyChecksColumn.Width = new GridLength(1, GridUnitType.Star);

            return;
        }

        PolicyContentMainRow.Height = GridLength.Auto;
        PolicyContentStackRow.Height = GridLength.Auto;

        Grid.SetRow(PolicyTextHost, 0);
        Grid.SetColumn(PolicyTextHost, 0);
        Grid.SetColumnSpan(PolicyTextHost, 2);

        Grid.SetRow(PolicyChecksHost, 1);
        Grid.SetColumn(PolicyChecksHost, 0);
        Grid.SetColumnSpan(PolicyChecksHost, 2);

        PolicyTextColumn.Width = new GridLength(1, GridUnitType.Star);
        PolicyChecksColumn.Width = new GridLength(0);
    }

    private void ApplyPolicyCheckboxLayout(PolicyLayoutMode mode)
    {
        if (mode == PolicyLayoutMode.Inline)
        {
            PolicyCheckCol0.Width = new GridLength(1, GridUnitType.Star);
            PolicyCheckCol1.Width = new GridLength(1, GridUnitType.Star);
            PolicyCheckCol2.Width = new GridLength(0);
            PolicyCheckboxesSecondRow.Height = GridLength.Auto;

            Grid.SetRow(PolicyTitlebarCheck, 0);
            Grid.SetColumn(PolicyTitlebarCheck, 0);
            Grid.SetRow(PolicyAeroPeekCheck, 0);
            Grid.SetColumn(PolicyAeroPeekCheck, 1);
            Grid.SetRow(PolicyAeroSnapCheck, 1);
            Grid.SetColumn(PolicyAeroSnapCheck, 0);
            Grid.SetColumnSpan(PolicyAeroSnapCheck, 2);
            return;
        }

        PolicyCheckCol0.Width = new GridLength(1, GridUnitType.Star);
        PolicyCheckCol1.Width = new GridLength(1, GridUnitType.Star);
        PolicyCheckCol2.Width = new GridLength(1, GridUnitType.Star);
        PolicyCheckboxesSecondRow.Height = new GridLength(0);

        Grid.SetRow(PolicyTitlebarCheck, 0);
        Grid.SetColumn(PolicyTitlebarCheck, 0);
        Grid.SetRow(PolicyAeroPeekCheck, 0);
        Grid.SetColumn(PolicyAeroPeekCheck, 1);
        Grid.SetRow(PolicyAeroSnapCheck, 0);
        Grid.SetColumn(PolicyAeroSnapCheck, 2);
        Grid.SetColumnSpan(PolicyAeroSnapCheck, 1);
    }

    private void ApplyPolicyCardHeight(PolicyChromeLayoutState state)
    {
        var lockHeight = state.HeroSideBySide
            && PolicyGateSwitch.IsOn
            && state.Mode == PolicyLayoutMode.Inline;

        if (_policyCardHeightLocked == lockHeight)
            return;

        _policyCardHeightLocked = lockHeight;
        var heroHeight = HeroPreviewLayoutMetrics.OuterHeight;
        PolicyCard.MinHeight = heroHeight;

        if (!lockHeight)
        {
            PolicyCard.ClearValue(FrameworkElement.MaxHeightProperty);
            return;
        }

        PolicyCard.MaxHeight = heroHeight;
    }

    private void UpdateReflectionTilesLayout() =>
        UpdateTilesLayout(_reflectionItems.Count, ReflectionItemsHost, ReflectionItems);

    private void UpdateMaterialTilesLayout() =>
        UpdateTilesLayout(_materialItems.Count, MaterialItemsHost, MaterialItems);

    private void UpdateAtlasTilesLayout() =>
        UpdateTilesLayout(_atlasItems.Count, AtlasItemsHost, AtlasItems);

    private static void UpdateTilesLayout(
        int itemCount,
        Grid host,
        ItemsView itemsView)
    {
        if (itemCount == 0) return;

        var hostWidth = host.ActualWidth;
        if (hostWidth <= 0) return;

        var tileUnit = GridTileWidth + GridTileGap;
        var maxColumns = Math.Clamp((int)((hostWidth + GridTileGap) / tileUnit), 1, MaxTileColumns);
        // Ширина ряда по числу «условных» слотов (maxColumns), чтобы коллекции с 1–4 плитками
        // выравнивались по левому краю с полными рядами (отражение ↔ материал).
        var gridWidth = maxColumns * GridTileWidth + Math.Max(0, maxColumns - 1) * GridTileGap;

        if (itemsView.Layout is UniformGridLayout layout)
            layout.MaximumRowsOrColumns = maxColumns;

        itemsView.Width = gridWidth;
        itemsView.HorizontalAlignment = HorizontalAlignment.Center;
    }

    private static string? NormalizePath(string? path)
    {
        if (string.IsNullOrWhiteSpace(path)) return null;
        try { return Path.GetFullPath(path); }
        catch { return path; }
    }

    private static bool SamePath(string? left, string? right) =>
        !string.IsNullOrWhiteSpace(left) &&
        !string.IsNullOrWhiteSpace(right) &&
        string.Equals(NormalizePath(left), NormalizePath(right), StringComparison.OrdinalIgnoreCase);

    private void ApplySectionParallelWarning(Action<bool, string?, string?> setWarning, IEnumerable<string> keys) =>
        DwmParallelBranchUi.ApplySectionWarning(_hive, keys, setWarning);

    private async Task RefreshHkcuConflictNoticeAsync()
    {
        var stack = PageShell.PageNoticeStack;
        var strip = stack.ActionNotice;
        var showConflict = _hive == DwmRegistryHive.Hkcu
                           && DwmRegistryStore.HasAnyPresent(
                               DwmRegistryHive.Hklm,
                               DwmSnapshotCatalog.GetKeys(DwmSnapshotPartition.Theme));
        if (showConflict)
        {
            strip.AllowDismiss = false;
            var host = SettingsNoticeCoordinator.CreateShellHost(stack, PageShell.PageScrollViewer);
            await SettingsNoticeCoordinator.PresentAsync(host, new SettingsNoticeCoordinator.NoticePresentRequest
            {
                Strip = strip,
                Severity = SettingsNoticeSeverity.Error,
                Message = GlassSettingsResources.Get("Dwm_HkcuConflictNotice"),
                MirrorSuppressKey = "dwm.hkcu-conflict.theme",
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
        SettingsCommitScope.ForDwmHiveArea(CustomizationAreaId.DwmTheme, _hive);

    public void ReloadFromExternalRegistrySync() => Reload();
}

public sealed class ReflectionImageTileModel : INotifyPropertyChanged, IThemeImageTileModel
{
    private bool _isSelected;
    private ImageSource? _image;

    public ReflectionImageTileModel(ThemeImageItem item)
    {
        Title = item.Title;
        FilePath = item.FilePath;
        _image = item.Image;
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    public event EventHandler? DeleteRequested;
    public string Title { get; }
    public string FilePath { get; }

    public void RequestDelete() => DeleteRequested?.Invoke(this, EventArgs.Empty);

    public ImageSource? Image
    {
        get => _image;
        private set
        {
            if (_image == value) return;
            _image = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Image)));
        }
    }

    public void UpdateImage(ImageSource? image)
    {
        Image = image;
    }

    public bool IsSelected
    {
        get => _isSelected;
        set
        {
            if (_isSelected == value) return;
            _isSelected = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsSelected)));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(SelectedVisibility)));
        }
    }

    public Visibility SelectedVisibility => IsSelected ? Visibility.Visible : Visibility.Collapsed;
}

public enum AtlasPreviewState
{
    Loading,
    Ready,
    Failed
}

public sealed class AtlasImageTileModel : INotifyPropertyChanged, IThemeImageTileModel
{
    private bool _isSelected;
    private ImageSource? _image;
    private AtlasPreviewState _previewState = AtlasPreviewState.Loading;

    public AtlasImageTileModel(ThemeImageItem item)
    {
        Title = item.Title;
        FilePath = item.FilePath;
        _image = item.Image;
        _previewState = item.Image is null ? AtlasPreviewState.Loading : AtlasPreviewState.Ready;
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    public event EventHandler? DeleteRequested;
    public string Title { get; }
    public string FilePath { get; }

    public void RequestDelete() => DeleteRequested?.Invoke(this, EventArgs.Empty);

    public ImageSource? Image
    {
        get => _image;
        private set
        {
            if (_image == value) return;
            _image = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Image)));
        }
    }

    public AtlasPreviewState PreviewState
    {
        get => _previewState;
        private set
        {
            if (_previewState == value) return;
            _previewState = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(PreviewState)));
        }
    }

    public void UpdateImage(ImageSource? image)
    {
        Image = image;
        PreviewState = image != null ? AtlasPreviewState.Ready : AtlasPreviewState.Loading;
    }

    public void SetPreviewFailed()
    {
        Image = null;
        PreviewState = AtlasPreviewState.Failed;
    }

    public bool IsSelected
    {
        get => _isSelected;
        set
        {
            if (_isSelected == value) return;
            _isSelected = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsSelected)));
        }
    }
}
