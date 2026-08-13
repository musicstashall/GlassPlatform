using System.Diagnostics;
using GlassSettings.Controls;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Navigation;
using Windows.UI;
using Windows.UI.ViewManagement;

namespace GlassSettings.Pages;

/// <summary>Визуальные стили → Фон рабочего стола (Фото / Solid / Слайд-шоу / Spotlight).</summary>
public sealed partial class ThemesDesktopBackgroundPage : Page
{
    private const int RecentTileSize = 100;
    private const int RecentTileDecodeWidth = 100;
    private const double RecentTileStride = 103; // 100 + spacing 3
    /// <summary>Reveal: выплывание из первой плитки вправо (чуть дольше theme Entrance).</summary>
    private const int RecentRevealDurationMs = 420;
    private const int RecentRevealStaggerMs = 80;
    /// <summary>Ожидание после Children Reorder/Insert под RepositionThemeTransition.</summary>
    private const int RecentThemeTransitionSettleMs = 500;
    private const double PersonalizeActionMinWidth = 125;
    /// <summary>Смена Photo↔Slideshow: системный cross-fade (Storyboard Opacity), 500 мс.</summary>
    private const int ModeBodyTransitionMs = 500;
    /// <summary>Смена кнопки папки: улёт вправо (250 мс) → пауза 500 мс → проявление 500 мс.</summary>
    private const int FolderRowSlideOutMs = 250;
    private const int FolderRowPauseMs = 500;
    private const int FolderRowFadeInMs = 500;
    private const string RecentTileScrimTag = "RecentTileScrim";
    private const string RecentTileAccentTag = "RecentTileAccent";

    private bool _loading;
    private bool _applyBusy;
    private bool _suppressAlbumSourceCombo;
    private bool _tileAnimBusy;
    private bool _folderRowAnimBusy;
    /// <summary>Последний показанный identity кнопки папки (kind|path|name); null — ещё не синхронизировали.</summary>
    private string? _folderRowIdentity;
    private bool _folderRowPending;
    private bool _folderRowPendingVisible;
    private string? _folderRowPendingPath;
    private string _folderRowPendingDisplay = string.Empty;
    private SlideshowAlbumKind _folderRowPendingKind;
    private Storyboard? _folderRowStoryboard;
    /// <summary>От старта wait/reveal до конца L→R storyboard — блокирует Sync/Rebuild strip.</summary>
    private bool _recentRevealPending;
    /// <summary>Клик по recent → до конца sync/анимации (анти-даблклик + прогресс на плитке).</summary>
    private bool _recentInteractionBusy;
    private Border? _pendingRecentProgressTile;
    private DispatcherQueueTimer? _recentProgressFallbackTimer;
    /// <summary>
    /// Browse/mode/fit сами обновляют preview — не дублировать через WallpaperChanged.
    /// Recent apply: false → strip/hero обновляются штатно по реестру.
    /// </summary>
    private bool _suppressExternalWallpaperRefresh;
    private ComboBox? _modeCombo;
    private EventHandler<DesktopEnvironmentChangedEventArgs>? _environmentChangedHandler;
    private DispatcherQueueTimer? _wallpaperRefreshTimer;

    public ThemesDesktopBackgroundPage()
    {
        InitializeComponent();
        Loaded += ThemesDesktopBackgroundPage_OnLoaded;
        PersonalizeExpander.Loaded += (_, _) => SyncBodyActionsAlignment();
        PersonalizeExpander.SizeChanged += PersonalizeExpander_OnSizeChanged;
        PersonalizeExpander.WireExpandedLayoutRefresh(SyncBodyActionsAlignment);
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        HookEnvironmentWatcher();
        // Первый заход: IsLoaded=false — полный sync в OnLoaded (после Fill*Combo).
        // Повторный заход (page cached): строки уже локализованы — sync здесь.
        if (IsLoaded)
            RefreshAll(forceImage: true, revealRecentIfPicture: true);
    }

    protected override void OnNavigatedFrom(NavigationEventArgs e)
    {
        UnhookEnvironmentWatcher();
        base.OnNavigatedFrom(e);
    }

    private void ThemesDesktopBackgroundPage_OnLoaded(object sender, RoutedEventArgs e)
    {
        // Один _loading на Fill+RefreshAll — иначе SelectionChanged между ними
        // может вызвать ApplyModeChoice / apply альбома.
        _loading = true;
        try
        {
            EnsureModeCombo();
            // Fill*Combo сбрасывает SelectedIndex — следом обязателен RefreshAll/Sync*.
            ApplyLocalizedStrings();
            RefreshAll(forceImage: true, revealRecentIfPicture: true);
        }
        finally
        {
            _loading = false;
        }
    }

    private void EnsureModeCombo()
    {
        if (_modeCombo is not null)
            return;

        _modeCombo = new ComboBox
        {
            MinWidth = PersonalizeActionMinWidth,
            HorizontalAlignment = HorizontalAlignment.Right
        };
        _modeCombo.SelectionChanged += ModeCombo_OnSelectionChanged;
        PersonalizeExpander.HeaderActions = _modeCombo;
        FillModeCombo(_modeCombo);
    }

    private void ApplyLocalizedStrings()
    {
        PersonalizeExpander.Title = GlassSettingsResources.Get("Themes_Bg_PersonalizeTitle");
        PersonalizeExpander.Description = GlassSettingsResources.Get("Themes_Bg_PersonalizeDesc");
        PersonalizeExpander.IconGlyph = SettingsSectionGlyphs.Theme;

        RecentImagesLabel.Text = GlassSettingsResources.Get("Themes_Bg_RecentImages");
        ChoosePhotoLabel.Text = GlassSettingsResources.Get("Themes_Bg_ChoosePhoto");
        BrowsePhotosButton.Content = GlassSettingsResources.Get("Themes_Bg_BrowsePhotos");
        ChooseFitLabel.Text = GlassSettingsResources.Get("Themes_Bg_ChooseFit");

        SlideshowAlbumLabel.Text = GlassSettingsResources.Get("Themes_Bg_SlideshowAlbum");
        BrowseAlbumButton.Content = GlassSettingsResources.Get("Themes_Bg_Browse");
        SlideshowFolderIcon.Source = ShellFolderIconResolver.TryGetFolderIcon();
        SlideshowIntervalLabel.Text = GlassSettingsResources.Get("Themes_Bg_SlideshowInterval");
        SlideshowShuffleLabel.Text = GlassSettingsResources.Get("Themes_Bg_SlideshowShuffle");
        SlideshowBatteryLabel.Text = GlassSettingsResources.Get("Themes_Bg_SlideshowOnBattery");
        SlideshowFitLabel.Text = GlassSettingsResources.Get("Themes_Bg_SlideshowChooseFit");

        FillModeCombo(_modeCombo);
        FillFitCombo(PhotoFitCombo);
        FillFitCombo(SlideshowFitCombo);
        FillIntervalCombo(SlideshowIntervalCombo);
        FillSlideshowAlbumSourceCombo();
    }

    private static void FillModeCombo(ComboBox? combo)
    {
        if (combo is null)
            return;

        var items = GlassSettingsResources.Get("Themes_Bg_ModeItems")
            .Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        combo.Items.Clear();
        foreach (var item in items)
            combo.Items.Add(item);
    }

    private static void FillFitCombo(ComboBox combo)
    {
        var items = GlassSettingsResources.Get("Themes_Bg_FitItems")
            .Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        combo.Items.Clear();
        foreach (var item in items)
            combo.Items.Add(item);
    }

    private static void FillIntervalCombo(ComboBox combo)
    {
        var items = GlassSettingsResources.Get("Themes_Bg_SlideshowIntervalItems")
            .Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        combo.Items.Clear();
        foreach (var item in items)
            combo.Items.Add(item);
    }

    private void RefreshAll(bool forceImage, bool revealRecentIfPicture = false)
    {
        var revealPicture = false;
        var ownedLoading = !_loading;
        if (ownedLoading)
            _loading = true;
        try
        {
            EnsureModeCombo();
            RefreshThemeChrome();
            // Enter/RefreshAll — только чтение реестра → UI (без heal/remember/apply).
            SyncModeComboFromRegistry();
            SyncFitCombosFromRegistry();
            SyncSlideshowControlsFromRegistry();
            ApplyModeBodyVisibility();
            revealPicture = revealRecentIfPicture
                && _modeCombo?.SelectedIndex == (int)DesktopBackgroundService.ModeChoice.Picture;
            if (!revealPicture)
                RebuildRecentTiles();
        }
        finally
        {
            if (ownedLoading)
                _loading = false;
        }

        if (revealPicture)
            _ = PrepareAndRevealRecentTilesAsync();

        if (forceImage)
            _ = RefreshWallpaperPreviewAsync();
        else
            HeroPreview.RefreshPreview();
    }

    /// <summary>
    /// Сразу резервирует высоту полосы (скрытые плитки из кэша), затем XAML fade+slide без паузы.
    /// </summary>
    private async Task PrepareAndRevealRecentTilesAsync()
    {
        if (_recentRevealPending || _tileAnimBusy)
        {
            return;
        }

        _recentRevealPending = true;
        try
        {
            var tiles = await PrepareRecentTilesHiddenAsync().ConfigureAwait(true);
            if (!IsLoaded)
                return;

            await AnimatePreparedRecentTilesRevealAsync(tiles).ConfigureAwait(true);
        }
        finally
        {
            _recentRevealPending = false;
        }
    }

    private void RefreshThemeChrome()
    {
        HeroPreview.ApplyThemeChrome(ResolveCurrentThemeEntry());
    }

    /// <summary>
    /// Общая цепочка: Invalidate+BeginLoad кэша → await → ForceRefreshPreview (как DWM/MainWindow).
    /// </summary>
    private async Task RefreshWallpaperPreviewAsync()
    {
        if (!IsLoaded)
        {
            return;
        }

        DesktopWallpaperImageCache.NotifyWallpaperChanged(DispatcherQueue);
        var load = DesktopWallpaperImageCache.LoadTask;
        if (load is not null)
            await load.ConfigureAwait(true);

        if (!IsLoaded)
            return;

        HeroPreview.ForceRefreshPreview();
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

    private void SyncModeComboFromRegistry()
    {
        if (_modeCombo is null)
            return;

        var index = (int)DesktopBackgroundService.GetModeChoice();
        if (_modeCombo.SelectedIndex != index && index < _modeCombo.Items.Count)
            _modeCombo.SelectedIndex = index;
    }

    private void SyncFitCombosFromRegistry()
    {
        var fit = (int)DesktopBackgroundService.GetWallpaperFit();
        SetComboIndex(PhotoFitCombo, fit);
        SetComboIndex(SlideshowFitCombo, fit);
    }

    private void SyncSlideshowControlsFromRegistry()
    {
        var hasAlbum = DesktopBackgroundService.TryGetSlideshowAlbum(out var path, out var displayName, out var kind)
            && !string.IsNullOrWhiteSpace(displayName);

        SyncSlideshowAlbumSourceComboSelection(
            hasAlbum ? path : null,
            hasAlbum ? kind : SlideshowAlbumKind.None);
        RequestSlideshowFolderRowUpdate(hasAlbum, path, displayName ?? string.Empty, kind);

        if (DesktopBackgroundService.TryGetSlideshowIntervalMs(out var intervalMs))
        {
            var index = DesktopBackgroundService.FindSlideshowIntervalIndex(intervalMs);
            SetComboIndex(SlideshowIntervalCombo, index);
        }

        if (DesktopBackgroundService.TryGetSlideshowShuffle(out var shuffle))
            SlideshowShuffleToggle.IsOn = shuffle;

        SlideshowBatteryToggle.IsOn = DesktopBackgroundService.GetAllowSlideshowOnBattery();
    }

    private static string BuildSlideshowFolderIdentity(
        bool visible,
        string? path,
        string displayName,
        SlideshowAlbumKind kind)
    {
        if (!visible)
            return string.Empty;

        return string.Concat(
            ((int)kind).ToString(),
            "|",
            path ?? string.Empty,
            "|",
            displayName);
    }

    private void RequestSlideshowFolderRowUpdate(
        bool visible,
        string? path,
        string displayName,
        SlideshowAlbumKind kind)
    {
        var identity = BuildSlideshowFolderIdentity(visible, path, displayName, kind);
        if (!_folderRowAnimBusy
            && _folderRowIdentity is not null
            && string.Equals(_folderRowIdentity, identity, StringComparison.Ordinal))
        {
            return;
        }

        if (_folderRowAnimBusy)
        {
            _folderRowPending = true;
            _folderRowPendingVisible = visible;
            _folderRowPendingPath = path;
            _folderRowPendingDisplay = displayName;
            _folderRowPendingKind = kind;
            return;
        }

        var animate = AreUiAnimationsEnabled()
            && _folderRowIdentity is not null
            && !string.Equals(_folderRowIdentity, identity, StringComparison.Ordinal)
            && (SlideshowFolderRow.Visibility == Visibility.Visible || visible);

        if (!animate)
        {
            ApplySlideshowFolderRow(visible, path, displayName, kind);
            return;
        }

        _ = AnimateSlideshowFolderRowAsync(visible, path, displayName, kind);
    }

    private void ApplySlideshowFolderRow(
        bool visible,
        string? path,
        string displayName,
        SlideshowAlbumKind kind)
    {
        StopFolderRowStoryboard();
        ResetFolderAnimLayer();
        SlideshowFolderAnimLayer.IsHitTestVisible = true;

        if (visible)
        {
            SlideshowFolderName.Text = displayName;
            SlideshowFolderRow.Visibility = Visibility.Visible;
        }
        else
        {
            SlideshowFolderName.Text = string.Empty;
            SlideshowFolderRow.Visibility = Visibility.Collapsed;
        }

        _folderRowIdentity = BuildSlideshowFolderIdentity(visible, path, displayName, kind);
    }

    private void ResetFolderAnimLayer()
    {
        EnsureTranslate(SlideshowFolderAnimLayer).X = 0;
        SlideshowFolderAnimLayer.Opacity = 1;
        SlideshowFolderRow.Opacity = 1;
        SlideshowFolderRow.ClearValue(UIElement.RenderTransformProperty);
    }

    private async Task AnimateSlideshowFolderRowAsync(
        bool visible,
        string? path,
        string displayName,
        SlideshowAlbumKind kind)
    {
        _folderRowAnimBusy = true;
        SlideshowFolderAnimLayer.IsHitTestVisible = false;
        try
        {
            // Снимаем Transform с Button — иначе WinUI двигает только ContentPresenter.
            SlideshowFolderRow.ClearValue(UIElement.RenderTransformProperty);
            SlideshowFolderRow.Opacity = 1;

            var slideOut = TimeSpan.FromMilliseconds(FolderRowSlideOutMs);
            var fadeIn = TimeSpan.FromMilliseconds(FolderRowFadeInMs);
            var wasVisible = SlideshowFolderRow.Visibility == Visibility.Visible;

            if (wasVisible)
            {
                // Улёт всей кнопки вправо: до двух её ширин, в пределах ширины строки (host).
                var buttonW = Math.Max(SlideshowFolderRow.ActualWidth, 1);
                var hostW = Math.Max(SlideshowFolderAnimHost.ActualWidth, buttonW);
                var slidePx = Math.Min(2.0 * buttonW, hostW);
                await RunFolderRowSlideFadeAsync(
                        fromX: 0,
                        toX: slidePx,
                        fromOpacity: SlideshowFolderAnimLayer.Opacity,
                        toOpacity: 0,
                        duration: slideOut,
                        ease: new CubicEase { EasingMode = EasingMode.EaseIn })
                    .ConfigureAwait(true);
            }

            if (_folderRowPending)
            {
                // Во время улёта пришла более новая цель — подхватить её на появлении.
                visible = _folderRowPendingVisible;
                path = _folderRowPendingPath;
                displayName = _folderRowPendingDisplay;
                kind = _folderRowPendingKind;
                _folderRowPending = false;
            }

            EnsureTranslate(SlideshowFolderAnimLayer).X = 0;
            SlideshowFolderRow.ClearValue(UIElement.RenderTransformProperty);
            SlideshowFolderRow.Opacity = 1;

            if (!visible)
            {
                SlideshowFolderName.Text = string.Empty;
                SlideshowFolderRow.Visibility = Visibility.Collapsed;
                SlideshowFolderAnimLayer.Opacity = 1;
                _folderRowIdentity = string.Empty;
                return;
            }

            SlideshowFolderName.Text = displayName;
            SlideshowFolderRow.Visibility = Visibility.Visible;
            SlideshowFolderAnimLayer.Opacity = 0;
            SlideshowFolderRow.UpdateLayout();
            UpdateSlideshowFolderAnimClip();

            // Пауза между улётом и проявлением — фазы не сливаются.
            if (wasVisible && FolderRowPauseMs > 0)
                await Task.Delay(FolderRowPauseMs).ConfigureAwait(true);

            await RunFolderRowSlideFadeAsync(
                    fromX: 0,
                    toX: 0,
                    fromOpacity: 0,
                    toOpacity: 1,
                    duration: fadeIn,
                    ease: new CubicEase { EasingMode = EasingMode.EaseOut })
                .ConfigureAwait(true);

            SlideshowFolderAnimLayer.Opacity = 1;
            _folderRowIdentity = BuildSlideshowFolderIdentity(true, path, displayName, kind);
        }
        finally
        {
            SlideshowFolderAnimLayer.IsHitTestVisible = true;
            _folderRowAnimBusy = false;

            if (_folderRowPending)
            {
                _folderRowPending = false;
                RequestSlideshowFolderRowUpdate(
                    _folderRowPendingVisible,
                    _folderRowPendingPath,
                    _folderRowPendingDisplay,
                    _folderRowPendingKind);
            }
        }
    }

    private Task RunFolderRowSlideFadeAsync(
        double fromX,
        double toX,
        double fromOpacity,
        double toOpacity,
        TimeSpan duration,
        EasingFunctionBase ease)
    {
        StopFolderRowStoryboard();

        var layer = SlideshowFolderAnimLayer;
        var tx = EnsureTranslate(layer);
        tx.X = fromX;
        layer.Opacity = fromOpacity;

        var tcs = new TaskCompletionSource();
        var sb = new Storyboard { FillBehavior = FillBehavior.HoldEnd };

        if (Math.Abs(fromX - toX) > 0.01)
        {
            var slide = new DoubleAnimation
            {
                From = fromX,
                To = toX,
                Duration = duration,
                EasingFunction = ease,
                EnableDependentAnimation = false,
                FillBehavior = FillBehavior.HoldEnd
            };
            Storyboard.SetTarget(slide, tx);
            Storyboard.SetTargetProperty(slide, "X");
            sb.Children.Add(slide);
        }

        if (Math.Abs(fromOpacity - toOpacity) > 0.01)
        {
            var fade = new DoubleAnimation
            {
                From = fromOpacity,
                To = toOpacity,
                Duration = duration,
                EasingFunction = ease,
                EnableDependentAnimation = false,
                FillBehavior = FillBehavior.HoldEnd
            };
            Storyboard.SetTarget(fade, layer);
            Storyboard.SetTargetProperty(fade, "Opacity");
            sb.Children.Add(fade);
        }

        if (sb.Children.Count == 0)
        {
            tcs.TrySetResult();
            return tcs.Task;
        }

        _folderRowStoryboard = sb;
        sb.Completed += (_, _) =>
        {
            tx.X = toX;
            layer.Opacity = toOpacity;
            if (ReferenceEquals(_folderRowStoryboard, sb))
                _folderRowStoryboard = null;
            tcs.TrySetResult();
        };
        sb.Begin();
        return tcs.Task;
    }

    private void StopFolderRowStoryboard()
    {
        if (_folderRowStoryboard is null)
            return;

        _folderRowStoryboard.Stop();
        _folderRowStoryboard = null;
    }

    private void SlideshowFolderAnimHost_OnSizeChanged(object sender, SizeChangedEventArgs e)
    {
        UpdateSlideshowFolderAnimClip();
    }

    private void UpdateSlideshowFolderAnimClip()
    {
        var w = SlideshowFolderAnimHost.ActualWidth;
        var h = SlideshowFolderAnimHost.ActualHeight;
        if (w <= 0 || h <= 0)
        {
            SlideshowFolderAnimHost.ClearValue(UIElement.ClipProperty);
            return;
        }

        SlideshowFolderAnimHost.Clip = new RectangleGeometry
        {
            Rect = new Windows.Foundation.Rect(0, 0, w, h)
        };
    }

    private void FillSlideshowAlbumSourceCombo()
    {
        SlideshowAlbumSourceCombo.Items.Clear();
        SlideshowAlbumSourceCombo.Items.Add(new SlideshowAlbumSourceItem(
            GlassSettingsResources.Get("Themes_Bg_WindowsDesktopBackgrounds"),
            folderPath: null,
            isWindowsAlbum: true));

        foreach (var (folderPath, name) in DesktopBackgroundService.EnumerateWindowsWallpaperSubfolders())
        {
            SlideshowAlbumSourceCombo.Items.Add(new SlideshowAlbumSourceItem(
                name,
                folderPath,
                isWindowsAlbum: false));
        }
    }

    private void SyncSlideshowAlbumSourceComboSelection(string? path, SlideshowAlbumKind kind)
    {
        _suppressAlbumSourceCombo = true;
        try
        {
            if (kind == SlideshowAlbumKind.WindowsDesktopBackgrounds)
            {
                for (var i = 0; i < SlideshowAlbumSourceCombo.Items.Count; i++)
                {
                    if (SlideshowAlbumSourceCombo.Items[i] is SlideshowAlbumSourceItem item
                        && item.IsWindowsAlbum)
                    {
                        SlideshowAlbumSourceCombo.SelectedIndex = i;
                        return;
                    }
                }
            }

            if (!string.IsNullOrWhiteSpace(path))
            {
                var full = Path.GetFullPath(path);
                for (var i = 0; i < SlideshowAlbumSourceCombo.Items.Count; i++)
                {
                    if (SlideshowAlbumSourceCombo.Items[i] is SlideshowAlbumSourceItem item
                        && !item.IsWindowsAlbum
                        && !string.IsNullOrWhiteSpace(item.FolderPath)
                        && Path.GetFullPath(item.FolderPath)
                            .Equals(full, StringComparison.OrdinalIgnoreCase))
                    {
                        SlideshowAlbumSourceCombo.SelectedIndex = i;
                        return;
                    }
                }
            }

            SlideshowAlbumSourceCombo.SelectedIndex = -1;
        }
        finally
        {
            _suppressAlbumSourceCombo = false;
        }
    }

    private sealed class SlideshowAlbumSourceItem
    {
        public SlideshowAlbumSourceItem(string displayName, string? folderPath, bool isWindowsAlbum)
        {
            DisplayName = displayName;
            FolderPath = folderPath;
            IsWindowsAlbum = isWindowsAlbum;
        }

        public string DisplayName { get; }
        public string? FolderPath { get; }
        public bool IsWindowsAlbum { get; }

        public override string ToString() => DisplayName;
    }

    private static void SetComboIndex(ComboBox combo, int index)
    {
        if (index < 0 || index >= combo.Items.Count)
            return;

        if (combo.SelectedIndex != index)
            combo.SelectedIndex = index;
    }

    private DesktopBackgroundService.ModeChoice GetSelectedMode()
    {
        if (_modeCombo is not null && _modeCombo.SelectedIndex >= 0)
            return (DesktopBackgroundService.ModeChoice)_modeCombo.SelectedIndex;

        return DesktopBackgroundService.GetModeChoice();
    }

    private void ApplyModeBodyVisibility()
    {
        var mode = GetSelectedMode();
        var isPhoto = mode == DesktopBackgroundService.ModeChoice.Picture;
        var isSlideshow = mode == DesktopBackgroundService.ModeChoice.Slideshow;

        PhotoBody.Opacity = 1;
        SlideshowBody.Opacity = 1;
        PhotoBody.Visibility = isPhoto ? Visibility.Visible : Visibility.Collapsed;
        SlideshowBody.Visibility = isSlideshow ? Visibility.Visible : Visibility.Collapsed;

        // Solid / Spotlight: тело скрыто, спойлер закрыт и не раскрывается; combo активен.
        var expand = isPhoto || isSlideshow;
        PersonalizeExpander.AllowBodyExpand = expand;
        // Не перезаписывать IsExpanded тем же значением — повторный expand даёт мерцание тела.
        if (PersonalizeExpander.IsExpanded != expand)
            PersonalizeExpander.IsExpanded = expand;
        if (expand)
            SyncBodyActionsAlignment();
    }

    private static bool IsPhotoOrSlideshowBody(DesktopBackgroundService.ModeChoice mode) =>
        mode is DesktopBackgroundService.ModeChoice.Picture
            or DesktopBackgroundService.ModeChoice.Slideshow;

    private static bool AreUiAnimationsEnabled()
    {
        try
        {
            return new UISettings().AnimationsEnabled;
        }
        catch
        {
            return true;
        }
    }

    /// <summary>
    /// Смена тела Photo↔Slideshow: системный cross-fade; →Фото затем L→R плиток.
    /// </summary>
    private async Task TransitionPhotoSlideshowBodiesAsync(
        DesktopBackgroundService.ModeChoice from,
        DesktopBackgroundService.ModeChoice to)
    {
        var outgoing = from == DesktopBackgroundService.ModeChoice.Picture ? PhotoBody : SlideshowBody;
        var incoming = to == DesktopBackgroundService.ModeChoice.Picture ? PhotoBody : SlideshowBody;

        PersonalizeExpander.AllowBodyExpand = true;
        if (!PersonalizeExpander.IsExpanded)
            PersonalizeExpander.IsExpanded = true;

        if (to == DesktopBackgroundService.ModeChoice.Picture)
        {
            _recentRevealPending = true;
            try
            {
                var warmSnap = RecentWallpaperThumbCache.TryGetSnapshot();

                // Плитки до fade на полном масштабе; LTR после cross-fade.
                incoming.Opacity = 0;
                incoming.IsHitTestVisible = false;
                incoming.Visibility = Visibility.Visible;
                incoming.ClearValue(UIElement.RenderTransformProperty);
                SyncBodyActionsAlignment();

                var preparedTiles = await PrepareRecentTilesHiddenAsync(warmSnap).ConfigureAwait(true);
                await TransitionBodiesCrossFadeAsync(outgoing, incoming).ConfigureAwait(true);
                incoming.IsHitTestVisible = true;

                if (preparedTiles.Count > 0)
                    await AnimatePreparedRecentTilesRevealAsync(preparedTiles).ConfigureAwait(true);
            }
            finally
            {
                PhotoBody.IsHitTestVisible = true;
                _recentRevealPending = false;
            }

            return;
        }

        await TransitionBodiesCrossFadeAsync(outgoing, incoming).ConfigureAwait(true);
    }

    /// <summary>
    /// Системный cross-fade через Storyboard Opacity (без ручного Clip/ScaleY — без дрожи).
    /// </summary>
    private async Task TransitionBodiesCrossFadeAsync(
        FrameworkElement outgoing,
        FrameworkElement incoming)
    {
        incoming.ClearValue(UIElement.RenderTransformProperty);
        outgoing.ClearValue(UIElement.RenderTransformProperty);
        incoming.ClearValue(UIElement.ClipProperty);
        outgoing.ClearValue(UIElement.ClipProperty);

        incoming.Opacity = 0;
        incoming.Visibility = Visibility.Visible;
        SyncBodyActionsAlignment();
        incoming.UpdateLayout();
        ModeBodiesHost.UpdateLayout();

        var outH = outgoing.Visibility == Visibility.Visible ? outgoing.ActualHeight : 0;
        var inH = incoming.ActualHeight;
        var reserveH = Math.Max(outH, inH);
        if (reserveH > 0)
            ModeBodiesHost.MinHeight = reserveH;

        if (!AreUiAnimationsEnabled())
        {
            FinishBodyCrossFade(outgoing, incoming);
            return;
        }

        var fadeOut = outgoing.Visibility == Visibility.Visible
            ? AnimateElementOpacityAsync(outgoing, from: outgoing.Opacity, to: 0)
            : Task.CompletedTask;
        var fadeIn = AnimateElementOpacityAsync(incoming, from: 0, to: 1);
        await Task.WhenAll(fadeOut, fadeIn).ConfigureAwait(true);

        FinishBodyCrossFade(outgoing, incoming);
    }

    private void FinishBodyCrossFade(FrameworkElement outgoing, FrameworkElement incoming)
    {
        outgoing.Visibility = Visibility.Collapsed;
        outgoing.Opacity = 1;
        incoming.Opacity = 1;
        ModeBodiesHost.ClearValue(FrameworkElement.MinHeightProperty);
    }

    private Task AnimateElementOpacityAsync(FrameworkElement target, double from, double to)
    {
        var tcs = new TaskCompletionSource();
        target.Opacity = from;

        var anim = new DoubleAnimation
        {
            From = from,
            To = to,
            Duration = TimeSpan.FromMilliseconds(ModeBodyTransitionMs),
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseInOut },
            EnableDependentAnimation = false,
            FillBehavior = FillBehavior.HoldEnd
        };
        Storyboard.SetTarget(anim, target);
        Storyboard.SetTargetProperty(anim, "Opacity");

        var sb = new Storyboard { FillBehavior = FillBehavior.HoldEnd };
        sb.Children.Add(anim);
        sb.Completed += (_, _) =>
        {
            target.Opacity = to;
            tcs.TrySetResult();
        };
        sb.Begin();
        return tcs.Task;
    }

    /// <summary>i&gt;0 сложены в слот первой (TranslateX = −i·stride) для L→R reveal.</summary>
    private void SeedRecentRevealTransforms(IReadOnlyList<Border> tiles)
    {
        if (tiles.Count == 0)
            return;

        EnsureTranslate(tiles[0]).X = 0;
        if (tiles.Count == 1 || !AreUiAnimationsEnabled())
        {
            for (var i = 1; i < tiles.Count; i++)
                EnsureTranslate(tiles[i]).X = 0;
            return;
        }

        for (var i = 1; i < tiles.Count; i++)
            EnsureTranslate(tiles[i]).X = -i * RecentTileStride;
    }

    /// <summary>
    /// Правый край кнопок/combo/тумблеров в теле = правый край combo в шапке
    /// (inset под spacing+шеврон при ShowGateSwitch=false).
    /// </summary>
    private void PersonalizeExpander_OnSizeChanged(object sender, SizeChangedEventArgs e)
    {
        SyncBodyActionsAlignment();
    }

    private void SyncBodyActionsAlignment()
    {
        var inset = PersonalizeExpander.MeasureHeaderActionsBodyAlignInset();
        var margin = new Thickness(0, 0, inset, 0);
        ApplyRightInset(BrowsePhotosButton, margin);
        ApplyRightInset(PhotoFitCombo, margin);
        // Combo слева от «Обзор» — inset только у крайней правой кнопки.
        ApplyRightInset(BrowseAlbumButton, margin);
        ApplyRightInset(SlideshowIntervalCombo, margin);
        ApplyRightInset(SlideshowShuffleToggle, margin);
        ApplyRightInset(SlideshowBatteryToggle, margin);
        ApplyRightInset(SlideshowFitCombo, margin);
    }

    private static void ApplyRightInset(FrameworkElement element, Thickness margin)
    {
        if (element.Margin != margin)
            element.Margin = margin;
    }

    private void RebuildRecentTiles()
    {
        if (_recentRevealPending)
        {
            return;
        }

        EndRecentTileInteraction();
        RunWithoutRecentTileTransitions(() =>
        {
            RecentTilesHost.Children.Clear();
            var paths = DesktopBackgroundService.GetRecentHistoryPaths();
            if (paths.Count == 0)
            {
                RecentStrip.Visibility = Visibility.Collapsed;
                return;
            }

            RecentStrip.Visibility = Visibility.Visible;
            foreach (var path in paths)
                RecentTilesHost.Children.Add(CreateRecentTile(path));
        });
    }

    /// <summary>
    /// Плитки только из <see cref="RecentWallpaperThumbCache"/> (прогрев GS / внешний sync).
    /// Свежие shell + ImageSource из кэша; без await LoadTask на показе.
    /// При анимации 1..n сложены под первой (TranslateX = −i·stride).
    /// </summary>
    private Task<IReadOnlyList<Border>> PrepareRecentTilesHiddenAsync(
        IReadOnlyList<RecentWallpaperThumbCache.CachedThumb>? preferredSources = null)
    {
        EndRecentTileInteraction();
        RecentTilesHost.MinHeight = RecentTileSize;

        var paths = DesktopBackgroundService.GetRecentHistoryPaths();
        if (paths.Count == 0)
        {
            RunWithoutRecentTileTransitions(() => RecentTilesHost.Children.Clear());
            RecentStrip.Visibility = Visibility.Collapsed;
            RecentTilesHost.MinHeight = 0;
            return Task.FromResult<IReadOnlyList<Border>>(Array.Empty<Border>());
        }

        // Не ждать LoadTask на показе: ApplyModeChoice→NotifyHistoryChanged→Invalidate
        // даёт пустую строку на сотни мс. Только готовый snapshot / TryGet; фон догрузит сам.

        RecentStrip.Visibility = Visibility.Visible;

        // Не reuse Border: HoldEnd Storyboard / старый Translate на mode-switch дают запинание.
        // ImageSource только из кэша — без decode.
        // preferredSources: снимок до Invalidate (mode→Photo), те же объекты что у nav.
        var snapshot = preferredSources ?? RecentWallpaperThumbCache.TryGetSnapshot();
        var byPath = new Dictionary<string, ImageSource>(StringComparer.OrdinalIgnoreCase);
        if (snapshot is not null)
        {
            foreach (var item in snapshot)
                byPath[item.Path] = item.Source;
        }

        var tiles = new List<Border>(paths.Count);

        RunWithoutRecentTileTransitions(() =>
        {
            RecentTilesHost.Children.Clear();
            foreach (var path in paths)
            {
                var tile = CreateRecentTileShell(path);
                if (byPath.TryGetValue(path, out var source))
                    ApplyRecentTileImage(tile, source);
                else if (RecentWallpaperThumbCache.TryGet(path, out var cached) && cached is not null)
                    ApplyRecentTileImage(tile, cached);

                tile.Opacity = 1;
                RecentTilesHost.Children.Add(tile);
                tiles.Add(tile);
            }
        });

        SeedRecentRevealTransforms(tiles);
        return Task.FromResult<IReadOnlyList<Border>>(tiles);
    }

    /// <summary>
    /// Выплывание 1..n из слота первой плитки вправо (stagger). Без Opacity-анимации — меньше дрожи.
    /// </summary>
    private async Task AnimatePreparedRecentTilesRevealAsync(IReadOnlyList<Border> tiles)
    {
        if (tiles.Count == 0 || _tileAnimBusy)
            return;

        if (tiles.Count == 1 || !AreUiAnimationsEnabled())
        {
            foreach (var tile in tiles)
                EnsureTranslate(tile).X = 0;

            RecentTilesHost.ClearValue(FrameworkElement.MinHeightProperty);
            return;
        }

        _tileAnimBusy = true;
        try
        {
            EnsureTranslate(tiles[0]).X = 0;

            var duration = TimeSpan.FromMilliseconds(RecentRevealDurationMs);
            var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
            var sb = new Storyboard { FillBehavior = FillBehavior.HoldEnd };

            for (var i = 1; i < tiles.Count; i++)
            {
                var tile = tiles[i];
                var fromX = -i * RecentTileStride;
                EnsureTranslate(tile).X = fromX;

                var slide = new DoubleAnimation
                {
                    From = fromX,
                    To = 0,
                    Duration = duration,
                    BeginTime = TimeSpan.FromMilliseconds((i - 1) * RecentRevealStaggerMs),
                    EasingFunction = ease,
                    FillBehavior = FillBehavior.HoldEnd
                };
                Storyboard.SetTarget(slide, EnsureTranslate(tile));
                Storyboard.SetTargetProperty(slide, "X");
                sb.Children.Add(slide);
            }

            var tcs = new TaskCompletionSource();
            sb.Completed += (_, _) => tcs.TrySetResult();
            sb.Begin();
            await tcs.Task.ConfigureAwait(true);

            foreach (var tile in tiles)
                EnsureTranslate(tile).X = 0;

            RecentTilesHost.ClearValue(FrameworkElement.MinHeightProperty);
        }
        finally
        {
            _tileAnimBusy = false;
        }
    }

    /// <summary>
    /// Штатный sync strip по реестру: move-to-front / insert-front через Reposition/Entrance.
    /// </summary>
    private async Task SyncRecentTilesFromRegistryAsync(bool ignoreSuppress = false)
    {
        if (_tileAnimBusy
            || _recentRevealPending
            || (!ignoreSuppress && _suppressExternalWallpaperRefresh))
        {
            return;
        }

        var paths = DesktopBackgroundService.GetRecentHistoryPaths();
        if (paths.Count == 0)
        {
            RunWithoutRecentTileTransitions(() => RecentTilesHost.Children.Clear());
            RecentStrip.Visibility = Visibility.Collapsed;
            EndRecentTileInteraction();
            return;
        }

        RecentStrip.Visibility = Visibility.Visible;
        var tiles = RecentTilesHost.Children.OfType<Border>().ToList();
        if (RecentTilesMatchHistory(paths, tiles))
        {
            EndRecentTileInteraction();
            return;
        }

        if (TryGetMoveToFrontIndex(paths, tiles, out var fromIndex))
        {
            // Внешний sync (не клик recent): Reposition без бублика.
            ClearRecentTileProgressVisual();
            _tileAnimBusy = true;
            try
            {
                if (AreUiAnimationsEnabled())
                {
                    CommitRecentReorder(tiles, fromIndex);
                    await Task.Delay(RecentThemeTransitionSettleMs).ConfigureAwait(true);
                }
                else
                {
                    RunWithoutRecentTileTransitions(() => CommitRecentReorder(tiles, fromIndex));
                }
            }
            finally
            {
                _tileAnimBusy = false;
                EndRecentTileInteraction();
            }

            EnsureRecentTilesMatchHistory();
            return;
        }

        if (TryGetInsertFront(paths, tiles, out var newPath))
        {
            EndRecentTileInteraction();
            _tileAnimBusy = true;
            try
            {
                await AnimateInsertFrontAsync(newPath, tiles, paths.Count).ConfigureAwait(true);
            }
            finally
            {
                _tileAnimBusy = false;
            }

            EnsureRecentTilesMatchHistory();
            return;
        }

        RebuildRecentTiles();
    }

    private void EnsureRecentTilesMatchHistory()
    {
        var after = DesktopBackgroundService.GetRecentHistoryPaths();
        var afterTiles = RecentTilesHost.Children.OfType<Border>().ToList();
        if (!RecentTilesMatchHistory(after, afterTiles))
            RebuildRecentTiles();
    }

    private void BeginRecentTileInteraction(Border tile)
    {
        _recentInteractionBusy = true;
        _pendingRecentProgressTile = tile;
        SetRecentTileAccent(tile, show: true);
        SetRecentTileProgress(tile, show: true);
        ArmRecentProgressFallback();
    }

    private void EndRecentTileInteraction()
    {
        DisarmRecentProgressFallback();
        if (_pendingRecentProgressTile is not null)
        {
            SetRecentTileProgress(_pendingRecentProgressTile, show: false);
            SetRecentTileAccent(_pendingRecentProgressTile, show: false);
        }

        _pendingRecentProgressTile = null;
        _recentInteractionBusy = false;
    }

    /// <summary>Снять только бублик (рамочка акцента остаётся до конца interaction).</summary>
    private void ClearRecentTileProgressVisual()
    {
        if (_pendingRecentProgressTile is not null)
            SetRecentTileProgress(_pendingRecentProgressTile, show: false);
    }

    private static Border? FindRecentTileChrome(Border tile, string tag)
    {
        if (tile.Child is not Grid grid)
            return null;

        return grid.Children.OfType<Border>().FirstOrDefault(b => b.Tag as string == tag);
    }

    private static void SetRecentTileProgress(Border tile, bool show)
    {
        var scrim = FindRecentTileChrome(tile, RecentTileScrimTag);
        if (scrim?.Child is not ProgressRing ring)
            return;

        if (show)
        {
            scrim.Visibility = Visibility.Visible;
            ring.Visibility = Visibility.Visible;
            ring.IsActive = true;
        }
        else
        {
            ring.IsActive = false;
            ring.Visibility = Visibility.Collapsed;
            scrim.Visibility = Visibility.Collapsed;
        }
    }

    private static void SetRecentTileAccent(Border tile, bool show)
    {
        var frame = FindRecentTileChrome(tile, RecentTileAccentTag);
        if (frame is null)
            return;

        if (show)
        {
            frame.BorderBrush = AppThemeVisualResolver.ResolveThemeBrush("AccentFillColorDefaultBrush");
            frame.Visibility = Visibility.Visible;
        }
        else
        {
            frame.Visibility = Visibility.Collapsed;
        }
    }

    private void ArmRecentProgressFallback()
    {
        _recentProgressFallbackTimer ??= DispatcherQueue.CreateTimer();
        _recentProgressFallbackTimer.Interval = TimeSpan.FromMilliseconds(2500);
        _recentProgressFallbackTimer.IsRepeating = false;
        _recentProgressFallbackTimer.Tick -= OnRecentProgressFallbackTick;
        _recentProgressFallbackTimer.Tick += OnRecentProgressFallbackTick;
        _recentProgressFallbackTimer.Stop();
        _recentProgressFallbackTimer.Start();
    }

    private void DisarmRecentProgressFallback()
    {
        if (_recentProgressFallbackTimer is null)
            return;

        _recentProgressFallbackTimer.Tick -= OnRecentProgressFallbackTick;
        _recentProgressFallbackTimer.Stop();
    }

    private void OnRecentProgressFallbackTick(DispatcherQueueTimer sender, object args)
    {
        sender.Tick -= OnRecentProgressFallbackTick;
        if (!_recentInteractionBusy)
            return;

        EndRecentTileInteraction();
    }

    private static bool RecentTilesMatchHistory(
        IReadOnlyList<string> paths,
        IReadOnlyList<Border> tiles)
    {
        if (paths.Count != tiles.Count)
            return false;

        for (var i = 0; i < paths.Count; i++)
        {
            if (tiles[i].Tag is not string tag
                || !string.Equals(tag, paths[i], StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }
        }

        return true;
    }

    /// <summary>
    /// paths = перестановка tiles с элементом i вынесенным вперёд (типичный PushHistory).
    /// </summary>
    private static bool TryGetMoveToFrontIndex(
        IReadOnlyList<string> paths,
        IReadOnlyList<Border> tiles,
        out int fromIndex)
    {
        fromIndex = -1;
        if (paths.Count == 0 || paths.Count != tiles.Count)
            return false;

        var current = new string[tiles.Count];
        for (var i = 0; i < tiles.Count; i++)
        {
            if (tiles[i].Tag is not string tag)
                return false;
            current[i] = tag;
        }

        fromIndex = -1;
        for (var i = 0; i < current.Length; i++)
        {
            if (string.Equals(current[i], paths[0], StringComparison.OrdinalIgnoreCase))
            {
                fromIndex = i;
                break;
            }
        }

        if (fromIndex <= 0)
            return false;

        // Ожидаемый порядок после move-to-front.
        var expected = new List<string>(current.Length)
        {
            current[fromIndex]
        };
        for (var i = 0; i < fromIndex; i++)
            expected.Add(current[i]);
        for (var i = fromIndex + 1; i < current.Length; i++)
            expected.Add(current[i]);

        for (var i = 0; i < paths.Count; i++)
        {
            if (!string.Equals(expected[i], paths[i], StringComparison.OrdinalIgnoreCase))
                return false;
        }

        return true;
    }

    /// <summary>
    /// Новая обоина в history[0], которой не было в strip; suffix = прежние плитки.
    /// </summary>
    private static bool TryGetInsertFront(
        IReadOnlyList<string> paths,
        IReadOnlyList<Border> tiles,
        out string newPath)
    {
        newPath = string.Empty;
        if (paths.Count == 0)
            return false;

        newPath = paths[0];
        for (var i = 0; i < tiles.Count; i++)
        {
            if (tiles[i].Tag is string tag
                && string.Equals(tag, newPath, StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }
        }

        // paths[1..] совпадает с префиксом текущих плиток (хвост history мог отвалиться).
        var suffixLen = paths.Count - 1;
        if (tiles.Count < suffixLen)
            return false;

        for (var i = 0; i < suffixLen; i++)
        {
            if (tiles[i].Tag is not string tag
                || !string.Equals(tag, paths[i + 1], StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }
        }

        return true;
    }

    /// <summary>
    /// Новая карточка слева + Reposition существующих; лишние справа снимаются.
    /// </summary>
    private async Task AnimateInsertFrontAsync(
        string newPath,
        IReadOnlyList<Border> existingTiles,
        int targetCount)
    {
        if (existingTiles.Count == 0)
        {
            RunWithoutRecentTileTransitions(() =>
            {
                RecentTilesHost.Children.Clear();
                RecentTilesHost.Children.Add(CreateRecentTile(newPath));
            });
            return;
        }

        var newTile = (Border)CreateRecentTile(newPath);
        Border? dropTile = existingTiles.Count >= targetCount ? existingTiles[^1] : null;

        void ApplyInsert()
        {
            RecentTilesHost.Children.Insert(0, newTile);
            if (dropTile is not null)
                RecentTilesHost.Children.Remove(dropTile);

            while (RecentTilesHost.Children.Count > targetCount)
                RecentTilesHost.Children.RemoveAt(RecentTilesHost.Children.Count - 1);
        }

        if (!AreUiAnimationsEnabled())
        {
            RunWithoutRecentTileTransitions(ApplyInsert);
            return;
        }

        ApplyInsert();
        await Task.Delay(RecentThemeTransitionSettleMs).ConfigureAwait(true);
    }

    private void CommitRecentReorder(IReadOnlyList<Border> tiles, int selectedIndex)
    {
        if (selectedIndex <= 0
            || selectedIndex >= tiles.Count
            || !RecentTilesHost.Children.Contains(tiles[selectedIndex]))
        {
            return;
        }

        var selected = tiles[selectedIndex];
        RecentTilesHost.Children.Remove(selected);
        RecentTilesHost.Children.Insert(0, selected);
    }

    /// <summary>Мгновенный Clear/Add без Reposition (rebuild / анимации выкл.).</summary>
    private void RunWithoutRecentTileTransitions(Action action)
    {
        var saved = RecentTilesHost.ChildrenTransitions;
        RecentTilesHost.ChildrenTransitions = null;
        try
        {
            action();
        }
        finally
        {
            RecentTilesHost.ChildrenTransitions = saved;
        }
    }

    private static TranslateTransform EnsureTranslate(FrameworkElement element)
    {
        if (element.RenderTransform is TranslateTransform existing)
            return existing;

        var tx = new TranslateTransform();
        element.RenderTransform = tx;
        return tx;
    }

    private Border CreateRecentTileShell(string path)
    {
        // ImageBrush на Border уважает CornerRadius (дочерний Image — нет).
        var ring = new ProgressRing
        {
            Width = 32,
            Height = 32,
            MinWidth = 32,
            MinHeight = 32,
            IsActive = false,
            Visibility = Visibility.Collapsed,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center
        };

        var scrim = new Border
        {
            Tag = RecentTileScrimTag,
            Background = new SolidColorBrush(Color.FromArgb(0x66, 0x00, 0x00, 0x00)),
            CornerRadius = new CornerRadius(4),
            Visibility = Visibility.Collapsed,
            IsHitTestVisible = false,
            Child = ring
        };

        var accentFrame = new Border
        {
            Tag = RecentTileAccentTag,
            BorderThickness = new Thickness(2),
            CornerRadius = new CornerRadius(4),
            Background = new SolidColorBrush(Color.FromArgb(0, 0, 0, 0)),
            Visibility = Visibility.Collapsed,
            IsHitTestVisible = false
        };

        var root = new Border
        {
            Width = RecentTileSize,
            Height = RecentTileSize,
            CornerRadius = new CornerRadius(4),
            Tag = path,
            RenderTransform = new TranslateTransform(),
            Child = new Grid { Children = { scrim, accentFrame } }
        };

        root.Tapped += RecentTile_OnTapped;
        return root;
    }

    private static void ApplyRecentTileImage(Border root, ImageSource? source)
    {
        if (source is null)
            return;

        root.Background = new ImageBrush
        {
            ImageSource = source,
            Stretch = Stretch.UniformToFill,
            AlignmentX = AlignmentX.Center,
            AlignmentY = AlignmentY.Center
        };
    }

    private UIElement CreateRecentTile(string path)
    {
        var root = CreateRecentTileShell(path);
        if (RecentWallpaperThumbCache.TryGet(path, out var cached) && cached is not null)
            ApplyRecentTileImage(root, cached);
        else
            ApplyRecentTileImage(root, ThemeImageUriHelper.TryCreateBitmap(path, RecentTileDecodeWidth));
        return root;
    }

    private void RecentTile_OnTapped(object sender, TappedRoutedEventArgs e)
    {
        if (_loading
            || _applyBusy
            || _tileAnimBusy
            || _recentInteractionBusy
            || sender is not Border { Tag: string path } tile)
        {
            return;
        }

        _ = ApplyPictureFromRecentAsync(path, tile);
    }

    /// <summary>
    /// Клик по recent: сразу move-to-front (UI), apply картинки параллельно на STA.
    /// Без ProgressRing — не ждём COM/реестр перед анимацией.
    /// </summary>
    private async Task ApplyPictureFromRecentAsync(string path, Border tile)
    {
        if (_applyBusy || _recentInteractionBusy || _tileAnimBusy)
            return;

        var tiles = RecentTilesHost.Children.OfType<Border>().ToList();
        var fromIndex = -1;
        for (var i = 0; i < tiles.Count; i++)
        {
            if (ReferenceEquals(tiles[i], tile))
            {
                fromIndex = i;
                break;
            }
        }

        if (fromIndex < 0)
            return;

        _applyBusy = true;
        _recentInteractionBusy = true;
        // Пока идёт apply+анимация — не дублировать strip через WallpaperChanged.
        _suppressExternalWallpaperRefresh = true;
        try
        {
            // STA apply сразу, не блокируя старт анимации.
            var applyTask = StaThreadRunner.RunAsync(() => DesktopBackgroundService.TryApplyPicture(path));

            if (fromIndex > 0)
            {
                _tileAnimBusy = true;
                try
                {
                    if (AreUiAnimationsEnabled())
                    {
                        CommitRecentReorder(tiles, fromIndex);
                        await Task.Delay(RecentThemeTransitionSettleMs).ConfigureAwait(true);
                    }
                    else
                    {
                        RunWithoutRecentTileTransitions(() => CommitRecentReorder(tiles, fromIndex));
                    }
                }
                finally
                {
                    _tileAnimBusy = false;
                }
            }

            var ok = await applyTask.ConfigureAwait(true);
            if (ok == true)
                await RefreshWallpaperPreviewFromCacheAsync().ConfigureAwait(true);

            // Apply fail / расхождение path — вернуть strip к реестру.
            EnsureRecentTilesMatchHistory();
        }
        finally
        {
            _suppressExternalWallpaperRefresh = false;
            _recentInteractionBusy = false;
            _applyBusy = false;
        }
    }

    private async void BrowsePhotosButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (_loading || _applyBusy)
            return;

        var path = await ExecutableFilePickerDialogService.PickImageAsync(DispatcherQueue)
            .ConfigureAwait(true);
        if (string.IsNullOrWhiteSpace(path))
            return;

        await ApplyPictureAsync(path).ConfigureAwait(true);
    }

    private async void BrowseAlbumButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (_loading || _applyBusy)
            return;

        var initialDirectory = ResolveSlideshowBrowseInitialDirectory();
        var folder = await ExecutableFilePickerDialogService
            .PickFolderAsync(DispatcherQueue, initialDirectory)
            .ConfigureAwait(true);
        if (string.IsNullOrWhiteSpace(folder))
            return;

        await ApplySlideshowFolderAsync(folder).ConfigureAwait(true);
    }

    /// <summary>
    /// Стартовый путь Folder-Picker: текущий альбом слайд-шоу, иначе %SystemRoot%\Web\Wallpaper.
    /// </summary>
    private static string ResolveSlideshowBrowseInitialDirectory()
    {
        if (DesktopBackgroundService.TryGetSlideshowAlbum(out var path, out _, out _)
            && ExecutableFilePickerCatalog.IsValidDirectory(path))
            return ExecutableFilePickerCatalog.NormalizeBrowsablePath(path);

        return DesktopWallpaperCom.GetWindowsDesktopBackgroundsRoot();
    }

    private void SlideshowAlbumSourceCombo_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || _suppressAlbumSourceCombo || _applyBusy)
            return;

        if (SlideshowAlbumSourceCombo.SelectedItem is not SlideshowAlbumSourceItem item)
            return;

        if (item.IsWindowsAlbum)
        {
            _ = ApplyWindowsDesktopBackgroundsAsync();
            return;
        }

        if (!string.IsNullOrWhiteSpace(item.FolderPath))
            _ = ApplySlideshowFolderAsync(item.FolderPath);
    }

    private async void SlideshowFolderRow_OnClick(object sender, RoutedEventArgs e)
    {
        if (_loading || _applyBusy || App.ShellWindow is null)
            return;

        if (!DesktopBackgroundService.TryGetSlideshowAlbum(out var path, out var displayName, out var kind)
            || kind == SlideshowAlbumKind.None
            || string.IsNullOrWhiteSpace(path))
        {
            return;
        }

        await SlideshowAlbumPickerDialogService.ShowAsync(
                DispatcherQueue,
                kind,
                path,
                displayName)
            .ConfigureAwait(true);

        _loading = true;
        try
        {
            SyncSlideshowControlsFromRegistry();
        }
        finally
        {
            _loading = false;
        }

        await RefreshWallpaperPreviewAsync().ConfigureAwait(true);
    }

    private async Task ApplyWindowsDesktopBackgroundsAsync()
    {
        if (_applyBusy)
            return;

        _applyBusy = true;
        try
        {
            // COM IDesktopWallpaper — только STA (Task.Run = MTA ломал стол).
            var ok = await StaThreadRunner.RunAsync(DesktopBackgroundService.TryApplyWindowsDesktopBackgroundsAlbum)
                .ConfigureAwait(true);
            if (ok != true)
                return;

            _loading = true;
            try
            {
                if (_modeCombo is not null
                    && _modeCombo.SelectedIndex != (int)DesktopBackgroundService.ModeChoice.Slideshow)
                {
                    _modeCombo.SelectedIndex = (int)DesktopBackgroundService.ModeChoice.Slideshow;
                }

                SyncSlideshowControlsFromRegistry();
                ApplyModeBodyVisibility();
            }
            finally
            {
                _loading = false;
            }

            // Превью — только RegWatcher (LastTick/Transcoded), без Schedule здесь.
        }
        finally
        {
            _applyBusy = false;
        }
    }

    private async Task ApplySlideshowFolderAsync(string folder)
    {
        if (_applyBusy)
            return;

        _applyBusy = true;
        try
        {
            var ok = await StaThreadRunner.RunAsync(() => DesktopBackgroundService.TryApplySlideshowFolder(folder))
                .ConfigureAwait(true);
            if (ok != true)
                return;

            _loading = true;
            try
            {
                if (_modeCombo is not null
                    && _modeCombo.SelectedIndex != (int)DesktopBackgroundService.ModeChoice.Slideshow)
                {
                    _modeCombo.SelectedIndex = (int)DesktopBackgroundService.ModeChoice.Slideshow;
                }

                SyncSlideshowControlsFromRegistry();
                ApplyModeBodyVisibility();
            }
            finally
            {
                _loading = false;
            }
        }
        finally
        {
            _applyBusy = false;
        }
    }

    private async Task ApplyPictureAsync(string path)
    {
        if (_applyBusy || _tileAnimBusy)
            return;

        _applyBusy = true;
        _suppressExternalWallpaperRefresh = true;
        try
        {
            var ok = await StaThreadRunner.RunAsync(() => DesktopBackgroundService.TryApplyPicture(path))
                .ConfigureAwait(true);
            if (ok != true)
                return;

            _loading = true;
            try
            {
                if (_modeCombo is not null
                    && _modeCombo.SelectedIndex != (int)DesktopBackgroundService.ModeChoice.Picture)
                {
                    _modeCombo.SelectedIndex = (int)DesktopBackgroundService.ModeChoice.Picture;
                }

                ApplyModeBodyVisibility();
            }
            finally
            {
                _loading = false;
            }

            // Insert-front анимация (не мгновенный Rebuild).
            await SyncRecentTilesFromRegistryAsync(ignoreSuppress: true).ConfigureAwait(true);
            await RefreshWallpaperPreviewAsync().ConfigureAwait(true);
        }
        finally
        {
            _suppressExternalWallpaperRefresh = false;
            _applyBusy = false;
        }
    }

    private async void ModeCombo_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || _applyBusy || _modeCombo is null)
            return;

        var index = _modeCombo.SelectedIndex;
        if (index < 0 || index > (int)DesktopBackgroundService.ModeChoice.Spotlight)
            return;

        var mode = (DesktopBackgroundService.ModeChoice)index;
        var previous = InferPreviousBodyMode(mode);

        _applyBusy = true;
        // SPI/COM из ApplyModeChoice шлёт WallpaperChanged — без suppress тик
        // мгновенно ApplyModeBodyVisibility и съедает fade Photo↔Slideshow.
        _suppressExternalWallpaperRefresh = true;
        // До Apply: иначе NotifyHistoryChanged→BeginLoad декодирует thumbs на UI во время LTR.
        RecentWallpaperThumbCache.SuspendReload();
        DesktopWallpaperImageCache.SuspendReload();

        try
        {
            var animateBody = PersonalizeExpander.IsExpanded
                && IsPhotoOrSlideshowBody(previous)
                && IsPhotoOrSlideshowBody(mode)
                && previous != mode
                && AreUiAnimationsEnabled();

            // →Фото + LTR: Apply после reveal (SPI/DWM не во время Translate).
            // →Слайд-шоу: Apply+Sync контролов до fade — иначе combo/кнопки
            // появляются только в CompleteModeApply после анимации.
            Task applyTask;
            if (animateBody && mode == DesktopBackgroundService.ModeChoice.Picture)
            {
                await TransitionPhotoSlideshowBodiesAsync(previous, mode)
                    .ConfigureAwait(true);
                applyTask = StaThreadRunner.RunAsync(() =>
                {
                    DesktopBackgroundService.ApplyModeChoice(mode);
                    return true;
                });
            }
            else
            {
                applyTask = StaThreadRunner.RunAsync(() =>
                {
                    DesktopBackgroundService.ApplyModeChoice(mode);
                    return true;
                });

                if (animateBody)
                {
                    if (mode == DesktopBackgroundService.ModeChoice.Slideshow)
                    {
                        await applyTask.ConfigureAwait(true);
                        SyncSlideshowControlsFromRegistry();
                    }

                    await TransitionPhotoSlideshowBodiesAsync(previous, mode)
                        .ConfigureAwait(true);
                }
                else
                {
                    ApplyModeBodyVisibility();
                    if (mode == DesktopBackgroundService.ModeChoice.Picture)
                        await PrepareAndRevealRecentTilesAsync().ConfigureAwait(true);
                }
            }

            _ = CompleteModeApplyAsync(applyTask, mode);
        }
        catch
        {
            RecentWallpaperThumbCache.ResumeReload(DispatcherQueue);
            DesktopWallpaperImageCache.ResumeReload(DispatcherQueue);
            _suppressExternalWallpaperRefresh = false;
            _applyBusy = false;
            throw;
        }
    }

    private async Task CompleteModeApplyAsync(
        Task applyTask,
        DesktopBackgroundService.ModeChoice mode)
    {
        try
        {
            await applyTask.ConfigureAwait(true);

            // Без анимации Sync ещё не вызывался; с анимацией — идемпотентно.
            if (mode == DesktopBackgroundService.ModeChoice.Slideshow)
                SyncSlideshowControlsFromRegistry();
        }
        finally
        {
            RecentWallpaperThumbCache.ResumeReload(DispatcherQueue);
            DesktopWallpaperImageCache.ResumeReload(DispatcherQueue);
            _suppressExternalWallpaperRefresh = false;
            _applyBusy = false;
        }
    }

    private DesktopBackgroundService.ModeChoice InferPreviousBodyMode(
        DesktopBackgroundService.ModeChoice next)
    {
        if (PhotoBody.Visibility == Visibility.Visible
            && next != DesktopBackgroundService.ModeChoice.Picture)
        {
            return DesktopBackgroundService.ModeChoice.Picture;
        }

        if (SlideshowBody.Visibility == Visibility.Visible
            && next != DesktopBackgroundService.ModeChoice.Slideshow)
        {
            return DesktopBackgroundService.ModeChoice.Slideshow;
        }

        return next;
    }

    private async void FitCombo_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || _applyBusy || sender is not ComboBox combo)
            return;

        var index = combo.SelectedIndex;
        if (index < 0 || index > (int)DesktopBackgroundService.WallpaperFit.Span)
            return;

        _applyBusy = true;
        _suppressExternalWallpaperRefresh = true;
        try
        {
            DesktopBackgroundService.SetWallpaperFit((DesktopBackgroundService.WallpaperFit)index);
            SetComboIndex(PhotoFitCombo, index);
            SetComboIndex(SlideshowFitCombo, index);
            await RefreshWallpaperPreviewAsync().ConfigureAwait(true);
        }
        finally
        {
            _suppressExternalWallpaperRefresh = false;
            _applyBusy = false;
        }
    }

    private void SlideshowIntervalCombo_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || _applyBusy)
            return;

        var index = SlideshowIntervalCombo.SelectedIndex;
        if (index < 0 || index >= DesktopBackgroundService.SlideshowIntervalMs.Length)
            return;

        _applyBusy = true;
        try
        {
            DesktopBackgroundService.SetSlideshowIntervalMs(
                DesktopBackgroundService.SlideshowIntervalMs[index]);
        }
        finally
        {
            _applyBusy = false;
        }
    }

    private void SlideshowShuffleToggle_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading || _applyBusy)
            return;

        _applyBusy = true;
        try
        {
            DesktopBackgroundService.SetSlideshowShuffle(SlideshowShuffleToggle.IsOn);
        }
        finally
        {
            _applyBusy = false;
        }
    }

    private void SlideshowBatteryToggle_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading || _applyBusy)
            return;

        _applyBusy = true;
        try
        {
            DesktopBackgroundService.SetAllowSlideshowOnBattery(SlideshowBatteryToggle.IsOn);
        }
        finally
        {
            _applyBusy = false;
        }
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

        _environmentChangedHandler = (_, args) =>
            DispatcherQueue.TryEnqueue(() => OnDesktopEnvironmentChanged(args));
        watcher.Changed += _environmentChangedHandler;
    }

    private void UnhookEnvironmentWatcher()
    {
        if (_environmentChangedHandler is null
            || DesktopEnvironmentWatcher.Current is not { } watcher)
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
                RefreshThemeChrome();

            // Recent apply не suppress — strip/hero как при смене фона вне приложения.
            if (e.WallpaperChanged && !_suppressExternalWallpaperRefresh)
            {
                ScheduleWallpaperPreviewRefresh();
            }
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"GlassSettings ThemesDesktopBackgroundPage.OnDesktopEnvironmentChanged: {ex}");
        }
    }

    private void ScheduleWallpaperPreviewRefresh()
    {
        _wallpaperRefreshTimer ??= DispatcherQueue.CreateTimer();
        // Чуть дольше — схлопнуть пачку TranscodedImageCache / history / Wallpaper.
        _wallpaperRefreshTimer.Interval = TimeSpan.FromMilliseconds(350);
        _wallpaperRefreshTimer.IsRepeating = false;
        _wallpaperRefreshTimer.Tick -= OnWallpaperPreviewRefreshTick;
        _wallpaperRefreshTimer.Tick += OnWallpaperPreviewRefreshTick;
        _wallpaperRefreshTimer.Stop();
        _wallpaperRefreshTimer.Start();
    }

    private void OnWallpaperPreviewRefreshTick(DispatcherQueueTimer sender, object args)
    {
        sender.Tick -= OnWallpaperPreviewRefreshTick;
        if (!IsLoaded || _suppressExternalWallpaperRefresh || _applyBusy)
            return;

        // Во время page-reveal не трогаем expander/strip — только hero.
        if (_recentRevealPending || _tileAnimBusy)
        {
            _ = RefreshWallpaperPreviewFromCacheAsync();
            return;
        }

        _ = RefreshDesktopBackgroundUiAfterExternalChangeAsync();
    }

    /// <summary>
    /// Внешняя смена обоев/темы: только sync UI. Heal — после apply .theme (VisualStylesPage),
    /// не здесь: LastTick слайдшоу при открытой странице иначе писал бы реестр.
    /// </summary>
    private async Task RefreshDesktopBackgroundUiAfterExternalChangeAsync()
    {
        if (!IsLoaded || _suppressExternalWallpaperRefresh || _applyBusy)
            return;

        if (_recentRevealPending || _tileAnimBusy)
        {
            await RefreshWallpaperPreviewFromCacheAsync().ConfigureAwait(true);
            return;
        }

        _loading = true;
        try
        {
            SyncModeComboFromRegistry();
            SyncFitCombosFromRegistry();
            SyncSlideshowControlsFromRegistry();
            ApplyModeBodyVisibility();
            RefreshThemeChrome();
        }
        finally
        {
            _loading = false;
        }

        await SyncRecentTilesFromRegistryAsync().ConfigureAwait(true);
        await RefreshWallpaperPreviewFromCacheAsync().ConfigureAwait(true);
    }

    /// <summary>После внешнего WallpaperChanged: кэш уже инвалидирован MainWindow.</summary>
    private async Task RefreshWallpaperPreviewFromCacheAsync()
    {
        if (!IsLoaded)
            return;

        var load = DesktopWallpaperImageCache.LoadTask;
        if (load is not null)
            await load.ConfigureAwait(true);

        if (!IsLoaded)
            return;

        HeroPreview.ForceRefreshPreview();
    }
}
