using System.IO;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;

namespace GlassSettings.Controls.Themes;

/// <summary>Body hosted-диалога: группы картинок альбома + checkbox include.</summary>
public sealed partial class SlideshowAlbumPickerPanel : UserControl, IHostedDialogAdaptiveBody
{
    private const int ThumbEdgePx = 180;
    private const int ApplyDebounceMs = 350;
    /// <summary>Как VisualStylesPage / плитки «Стилизация».</summary>
    private const double GridTileWidth = 168;
    private const double GridTileGap = 12;
    private const int MaxTileColumns = 6;
    private const double TileMinHeight = 148;

    private SlideshowAlbumKind _albumKind;
    private string _albumRoot;
    private string _albumDisplayName;
    private readonly List<SlideshowAlbumImageItem> _allItems = new();
    private readonly List<SettingsDividerExpander> _groupExpanders = new();
    private readonly List<ItemsRepeater> _groupRepeaters = new();
    private readonly DispatcherQueueTimer _applyTimer;
    private int _thumbGeneration;
    private bool _applyBusy;
    private bool _applyPending;
    private bool _bulkSelectionBusy;
    private bool _suppressFolderCombo;
    private bool _rebuildBusy;
    private HostedDialogHostContext? _hostContext;

    public SlideshowAlbumPickerPanel(
        SlideshowAlbumKind albumKind,
        string albumRoot,
        string albumDisplayName)
    {
        _albumKind = albumKind;
        _albumRoot = albumRoot;
        _albumDisplayName = albumDisplayName;
        InitializeComponent();

        var pad = HostedDialogLayout.GetContentDialogPadding();
        // Body edge-to-edge; inset бара/списка как у footer (ContentDialogPadding).
        Padding = new Thickness(0);
        SelectionBar.Margin = new Thickness(pad.Left, 0, pad.Right, 0);
        // Зазор сверху от бара и снизу от footer-разделителя.
        ListHost.Margin = new Thickness(pad.Left, 16, pad.Right, 16);

        _applyTimer = DispatcherQueue.CreateTimer();
        _applyTimer.Interval = TimeSpan.FromMilliseconds(ApplyDebounceMs);
        _applyTimer.IsRepeating = false;
        _applyTimer.Tick += ApplyTimer_OnTick;

        // SelectAll / ClearSelection / Switch (Segoe Fluent).
        SelectAllButton.Content = CreateToolbarButtonContent(
            "\uE8B3",
            GlassSettingsResources.Get("Themes_Bg_AlbumSelectAll"));
        DeselectAllButton.Content = CreateToolbarButtonContent(
            "\uE8E6",
            GlassSettingsResources.Get("Themes_Bg_AlbumDeselectAll"));
        InvertSelectionButton.Content = CreateToolbarButtonContent(
            "\uE8AB",
            GlassSettingsResources.Get("Themes_Bg_AlbumInvertSelection"));
        EmptyText.Text = GlassSettingsResources.Get("Themes_Bg_AlbumPickerEmpty");

        FillAlbumFolderCombo();
        SyncAlbumFolderComboSelection();

        ListHost.SizeChanged += (_, _) => UpdateTilesLayout();
        Loaded += OnLoaded;
    }

    public void AttachHostContext(HostedDialogHostContext context)
    {
        _hostContext = context;
        RefreshPrimaryEnabled();
    }

    public void OnHostedDialogBoundsChanged(double width, double height)
    {
        UpdateTilesLayout();
    }

    private void RefreshPrimaryEnabled()
    {
        var hasSelection = _allItems.Any(static i => i.IsIncluded);
        _hostContext?.SetPrimaryEnabled(hasSelection);
    }

    private static UIElement CreateToolbarButtonContent(string glyph, string label)
    {
        var row = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 8,
            VerticalAlignment = VerticalAlignment.Center
        };
        row.Children.Add(new FontIcon
        {
            Glyph = glyph,
            FontSize = 16,
            VerticalAlignment = VerticalAlignment.Center
        });
        row.Children.Add(new TextBlock
        {
            Text = label,
            VerticalAlignment = VerticalAlignment.Center,
            Style = (Style)Application.Current.Resources["BodyTextBlockStyle"]
        });
        return row;
    }

    private void FillAlbumFolderCombo()
    {
        _suppressFolderCombo = true;
        try
        {
            AlbumFolderCombo.Items.Clear();
            AlbumFolderCombo.Items.Add(new AlbumFolderComboItem(
                GlassSettingsResources.Get("Themes_Bg_WindowsDesktopBackgrounds"),
                folderPath: null,
                isWindowsAlbum: true));

            foreach (var (folderPath, name) in DesktopBackgroundService.EnumerateWindowsWallpaperSubfolders())
            {
                AlbumFolderCombo.Items.Add(new AlbumFolderComboItem(
                    name,
                    folderPath,
                    isWindowsAlbum: false));
            }
        }
        finally
        {
            _suppressFolderCombo = false;
        }
    }

    private void SyncAlbumFolderComboSelection()
    {
        _suppressFolderCombo = true;
        try
        {
            if (_albumKind == SlideshowAlbumKind.WindowsDesktopBackgrounds)
            {
                for (var i = 0; i < AlbumFolderCombo.Items.Count; i++)
                {
                    if (AlbumFolderCombo.Items[i] is AlbumFolderComboItem { IsWindowsAlbum: true })
                    {
                        AlbumFolderCombo.SelectedIndex = i;
                        return;
                    }
                }
            }

            if (!string.IsNullOrWhiteSpace(_albumRoot))
            {
                var full = Path.GetFullPath(_albumRoot);
                for (var i = 0; i < AlbumFolderCombo.Items.Count; i++)
                {
                    if (AlbumFolderCombo.Items[i] is AlbumFolderComboItem item
                        && !item.IsWindowsAlbum
                        && !string.IsNullOrWhiteSpace(item.FolderPath)
                        && Path.GetFullPath(item.FolderPath)
                            .Equals(full, StringComparison.OrdinalIgnoreCase))
                    {
                        AlbumFolderCombo.SelectedIndex = i;
                        return;
                    }
                }
            }

            AlbumFolderCombo.SelectedIndex = -1;
        }
        finally
        {
            _suppressFolderCombo = false;
        }
    }

    private async void AlbumFolderCombo_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_suppressFolderCombo || _rebuildBusy)
            return;

        if (AlbumFolderCombo.SelectedItem is not AlbumFolderComboItem item)
            return;

        _rebuildBusy = true;
        SelectionBar.IsHitTestVisible = false;
        try
        {
            if (item.IsWindowsAlbum)
            {
                var ok = await StaThreadRunner.RunAsync(DesktopBackgroundService.TryApplyWindowsDesktopBackgroundsAlbum)
                    .ConfigureAwait(true);
                if (ok != true)
                {
                    SyncAlbumFolderComboSelection();
                    return;
                }

                _albumKind = SlideshowAlbumKind.WindowsDesktopBackgrounds;
                _albumRoot = DesktopWallpaperCom.GetWindowsDesktopBackgroundsRoot();
                _albumDisplayName = DesktopBackgroundService.ResolveWindowsDesktopBackgroundsDisplayName();
                SyncAlbumFolderComboSelection();
            }
            else
            {
                if (string.IsNullOrWhiteSpace(item.FolderPath))
                    return;

                var folder = item.FolderPath;
                var ok = await StaThreadRunner.RunAsync(() => DesktopBackgroundService.TryApplySlideshowFolder(folder))
                    .ConfigureAwait(true);
                if (ok != true)
                {
                    SyncAlbumFolderComboSelection();
                    return;
                }

                _albumKind = SlideshowAlbumKind.Folder;
                _albumRoot = Path.GetFullPath(folder);
                _albumDisplayName = item.DisplayName;
                SyncAlbumFolderComboSelection();
            }

            await BuildAsync().ConfigureAwait(true);
        }
        finally
        {
            SelectionBar.IsHitTestVisible = true;
            _rebuildBusy = false;
        }
    }

    /// <summary>
    /// Эталон VisualStylesPage.UpdateThemeTilesLayout: columns по hostWidth,
    /// gridWidth = N×tile+(N−1)×gap, MaximumRowsOrColumns, Width, Center.
    /// </summary>
    private void UpdateTilesLayout()
    {
        var hostWidth = ListHost.ActualWidth;
        if (hostWidth <= 0)
            return;

        var tileUnit = GridTileWidth + GridTileGap;
        var columns = Math.Clamp((int)((hostWidth + GridTileGap) / tileUnit), 1, MaxTileColumns);
        var gridWidth = columns * GridTileWidth + Math.Max(0, columns - 1) * GridTileGap;

        GroupsHost.Width = gridWidth;
        GroupsHost.HorizontalAlignment = HorizontalAlignment.Center;

        for (var i = 0; i < _groupRepeaters.Count; i++)
        {
            var repeater = _groupRepeaters[i];
            if (repeater.Layout is UniformGridLayout layout)
                layout.MaximumRowsOrColumns = columns;

            repeater.Width = gridWidth;

            if (i < _groupExpanders.Count)
            {
                _groupExpanders[i].Width = gridWidth;
                _groupExpanders[i].HorizontalAlignment = HorizontalAlignment.Center;
            }
        }
    }

    private async void OnLoaded(object sender, RoutedEventArgs e)
    {
        Loaded -= OnLoaded;
        await BuildAsync().ConfigureAwait(true);
    }

    private async Task BuildAsync()
    {
        _thumbGeneration++;
        var isWindowsAlbum = _albumKind == SlideshowAlbumKind.WindowsDesktopBackgrounds;
        // Фоны… → все подпапки как в combo; иначе — только выбранная папка.
        var candidates = isWindowsAlbum
            ? DesktopBackgroundService.EnumerateWindowsDesktopBackgroundsAlbumImages()
            : DesktopBackgroundService.EnumerateSlideshowAlbumImages(_albumRoot, recursive: true);
        var included = DesktopBackgroundService.TryGetSlideshowIncludedImagePaths(_albumKind, candidates)
                       ?? new HashSet<string>(candidates.Select(Path.GetFullPath), StringComparer.OrdinalIgnoreCase);

        // Windows-альбом: пустой included (COM отдал чужой subset) → все картинки.
        if (isWindowsAlbum && included.Count == 0 && candidates.Count > 0)
        {
            included = new HashSet<string>(
                candidates.Select(Path.GetFullPath),
                StringComparer.OrdinalIgnoreCase);
        }

        foreach (var old in _allItems)
            old.InclusionChanged -= Item_OnInclusionChanged;

        _allItems.Clear();
        GroupsHost.Children.Clear();
        _groupExpanders.Clear();
        _groupRepeaters.Clear();

        if (candidates.Count == 0)
        {
            EmptyText.Visibility = Visibility.Visible;
            GroupsScroll.Visibility = Visibility.Collapsed;
            RefreshPrimaryEnabled();
            return;
        }

        EmptyText.Visibility = Visibility.Collapsed;
        GroupsScroll.Visibility = Visibility.Visible;

        foreach (var path in candidates)
        {
            var full = Path.GetFullPath(path);
            var item = new SlideshowAlbumImageItem(full, included.Contains(full));
            item.InclusionChanged += Item_OnInclusionChanged;
            _allItems.Add(item);
        }

        var groups = new List<SlideshowAlbumGroup>();
        if (isWindowsAlbum)
        {
            // Группы = все папки combo (даже пустые не попадут — нет файлов).
            var grouped = _allItems
                .GroupBy(i =>
                {
                    if (DesktopWallpaperCom.TryGetShellFolderDisplayName(i.FolderKey, out var shellName)
                        && !string.IsNullOrWhiteSpace(shellName))
                    {
                        return shellName;
                    }

                    var name = Path.GetFileName(
                        i.FolderKey.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
                    return string.IsNullOrWhiteSpace(name) ? _albumDisplayName : name;
                })
                .OrderBy(static g => g.Key, StringComparer.CurrentCultureIgnoreCase);

            foreach (var g in grouped)
                groups.Add(new SlideshowAlbumGroup(g.Key, g.ToList()));
        }
        else
        {
            var key = string.IsNullOrWhiteSpace(_albumDisplayName)
                ? Path.GetFileName(
                    _albumRoot.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar))
                : _albumDisplayName;
            if (DesktopWallpaperCom.TryGetShellFolderDisplayName(_albumRoot, out var shellName)
                && !string.IsNullOrWhiteSpace(shellName))
            {
                key = shellName;
            }

            groups.Add(new SlideshowAlbumGroup(key, _allItems.ToList()));
        }

        var tileTemplate = (DataTemplate)Resources["AlbumImageTileTemplate"];
        foreach (var group in groups)
        {
            var expander = new SettingsDividerExpander
            {
                Title = group.FolderName,
                IsExpanded = true,
                HorizontalAlignment = HorizontalAlignment.Center
            };
            expander.SetChromeSpacing(headerTopMargin: 8, bodyTopMargin: 8);

            var layout = new UniformGridLayout
            {
                MinItemWidth = GridTileWidth,
                MinItemHeight = TileMinHeight,
                MinRowSpacing = GridTileGap,
                MinColumnSpacing = GridTileGap,
                MaximumRowsOrColumns = MaxTileColumns,
                ItemsStretch = UniformGridLayoutItemsStretch.None,
                ItemsJustification = UniformGridLayoutItemsJustification.Start
            };

            var repeater = new ItemsRepeater
            {
                ItemsSource = group.Images,
                ItemTemplate = tileTemplate,
                Layout = layout,
                HorizontalAlignment = HorizontalAlignment.Left
            };

            expander.BodyContent = repeater;
            GroupsHost.Children.Add(expander);
            _groupExpanders.Add(expander);
            _groupRepeaters.Add(repeater);
        }

        UpdateTilesLayout();
        RefreshPrimaryEnabled();

        _ = DispatcherQueue.TryEnqueue(() =>
        {
            UpdateTilesLayout();
            foreach (var exp in _groupExpanders)
                exp.RefreshDividerChrome();
        });

        await LoadThumbnailsAsync().ConfigureAwait(true);
    }

    private async Task LoadThumbnailsAsync()
    {
        var gen = ++_thumbGeneration;
        try
        {
            foreach (var item in _allItems)
            {
                if (gen != _thumbGeneration)
                    return;

                var thumb = await ThemeImageUriHelper.TryCreateThumbnailAsync(item.FullPath, ThumbEdgePx)
                    .ConfigureAwait(true);
                if (gen != _thumbGeneration)
                    return;

                if (thumb is not null)
                    item.Thumbnail = thumb;
            }
        }
        finally
        {
            // no-op: бублик на плитке гаснет через Thumbnail / LoadingRingVisibility
        }
    }

    private void SelectAllButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (_allItems.Count == 0)
            return;

        _bulkSelectionBusy = true;
        try
        {
            foreach (var item in _allItems)
                item.SetIncludedSilent(true);
        }
        finally
        {
            _bulkSelectionBusy = false;
        }

        RefreshPrimaryEnabled();
        ScheduleApply();
    }

    private void DeselectAllButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (_allItems.Count == 0)
            return;

        _bulkSelectionBusy = true;
        try
        {
            foreach (var item in _allItems)
                item.SetIncludedSilent(false);
        }
        finally
        {
            _bulkSelectionBusy = false;
        }

        // Пустой выбор — «Сохранить» недоступна; COM не трогаем до Save / Dismiss→restore.
        RefreshPrimaryEnabled();
        _applyTimer.Stop();
    }

    private void InvertSelectionButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (_allItems.Count == 0)
            return;

        _bulkSelectionBusy = true;
        try
        {
            foreach (var item in _allItems)
                item.SetIncludedSilent(!item.IsIncluded);
        }
        finally
        {
            _bulkSelectionBusy = false;
        }

        RefreshPrimaryEnabled();
        if (_allItems.Any(static i => i.IsIncluded))
            ScheduleApply();
        else
            _applyTimer.Stop();
    }

    private void Item_OnInclusionChanged(SlideshowAlbumImageItem item, bool isIncluded)
    {
        if (_bulkSelectionBusy)
            return;

        RefreshPrimaryEnabled();
        if (_allItems.Any(static i => i.IsIncluded))
            ScheduleApply();
        else
            _applyTimer.Stop();
    }

    private void TileBorder_OnPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        if (sender is not FrameworkElement { Tag: SlideshowAlbumImageItem item })
            return;

        // Клик по всей плитке (как карточки в Text Visual Tuning Wizard).
        item.IsIncluded = !item.IsIncluded;
        e.Handled = true;
    }

    private void ScheduleApply()
    {
        _applyTimer.Stop();
        _applyTimer.Start();
    }

    private void ApplyTimer_OnTick(DispatcherQueueTimer sender, object args)
    {
        _ = ApplyIncludedAsync();
    }

    private async Task ApplyIncludedAsync()
    {
        if (_applyBusy)
        {
            _applyPending = true;
            return;
        }

        _applyBusy = true;
        try
        {
            do
            {
                _applyPending = false;
                var selected = _allItems.Where(static i => i.IsIncluded).Select(static i => i.FullPath).ToList();
                // Пустой список не применяем — иначе COM/реестр дают чёрный стол.
                if (selected.Count == 0)
                {
                    SyncInclusionFromCom();
                    continue;
                }

                var ok = await StaThreadRunner.RunAsync(() =>
                        DesktopBackgroundService.TryApplySlideshowImageSubset(
                            selected,
                            _albumKind,
                            _albumDisplayName))
                    .ConfigureAwait(true);

                if (!ok)
                {
                    // Откат UI к реальному COM, если apply не прошёл.
                    SyncInclusionFromCom();
                }
            }
            while (_applyPending);
        }
        finally
        {
            _applyBusy = false;
        }
    }

    private void SyncInclusionFromCom()
    {
        var candidates = _allItems.Select(static i => i.FullPath).ToList();
        var included = DesktopBackgroundService.TryGetSlideshowIncludedImagePaths(_albumKind, candidates);
        if (included is null)
            return;

        foreach (var item in _allItems)
        {
            var should = included.Contains(item.FullPath);
            if (item.IsIncluded != should)
                item.SetIncludedSilent(should);
        }

        RefreshPrimaryEnabled();
    }

    private sealed class AlbumFolderComboItem
    {
        public AlbumFolderComboItem(string displayName, string? folderPath, bool isWindowsAlbum)
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
}
