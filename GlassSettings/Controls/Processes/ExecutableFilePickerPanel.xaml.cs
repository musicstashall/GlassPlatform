using System.Collections.ObjectModel;
using System.Threading;
using GlassSettings.Core.Registry;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;
using VirtualKey = Windows.System.VirtualKey;

namespace GlassSettings.Controls.Processes;

public sealed partial class ExecutableFilePickerPanel : UserControl, IHostedDialogAdaptiveBody
{
    private readonly DispatcherQueue _dispatcherQueue;
    private readonly List<ExecutableFilePickerItem> _allItems = new();
    private readonly List<ExecutableFilePickerItem> _searchResults = new();
    private readonly List<ExecutableFilePickerItem> _rawSearchResults = new();
    private readonly ObservableCollection<ExecutableFilePickerItem> _filtered = new();
    private readonly List<ExecutableFilePickerBreadcrumbItem> _breadcrumbTrail = new();
    private readonly Stack<ExecutableFilePickerNavigationEntry> _backHistory = new();
    private readonly Stack<ExecutableFilePickerNavigationEntry> _forwardHistory = new();
    private readonly ExecutableFilePickerSearchOptions _searchOptions = new();

    private HostedDialogHostContext? _dialogContext;
    private string _currentPath;
    private readonly bool _foldersOnly;
    private readonly IReadOnlyList<ExecutableFilePickerExtensionFilterItem> _extensionFilters;
    private IReadOnlyList<string>? _catalogExtensionFilter;
    private ExecutableFilePickerItem? _selected;
    private bool _suppressTreeSync;
    private bool _suppressHistoryRecord;
    private bool _initialized;
    private bool _sortDescending;
    private FilePickerSortColumn _sortColumn = FilePickerSortColumn.Name;
    private bool _isInlineTreeVisible = true;
    private FilePickerViewMode _viewMode = FilePickerViewMode.List;
    private bool _isAddressEditMode;
    private bool _barHostHoverRegistered;
    private ExecutableFilePickerTreeItem? _pendingTreeNavigateItem;
    private TreeView? _pendingTreeNavigateView;
    private Brush? _barHostNormalBrush;
    private Brush? _barHostHoverBrush;
    private bool _suppressAddressLostFocus;
    private int _listIconGeneration;
    private bool _isSearchMode;
    private string _activeSearchQuery = string.Empty;
    private bool _suppressSearchTextChanged;
    private int _searchGeneration;
    private int _navigateGeneration;
    private CancellationTokenSource? _searchCancellation;
    private bool _isSearchInProgress;
    private readonly DispatcherQueueTimer _searchDebounceTimer;
    private string _pendingSearchQuery = string.Empty;
    private string _lastDiskSearchQuery = string.Empty;
    private ExecutableFilePickerSearchScope _lastDiskSearchScope;
    private IReadOnlyList<string>? _lastDiskSearchCatalogFilter;
    private string? _lastDiskSearchTypeExtension;
    private ExecutableFilePickerSearchSizeFilter? _lastDiskSearchSizeFilter;
    private bool _pendingTreeNavigateUserInitiated;
    private RadioMenuFlyoutItem? _scopeAllNestedItem;
    private RadioMenuFlyoutItem? _scopeCurrentFolderItem;
    private RadioMenuFlyoutItem? _sizeAnyItem;

    public ExecutableFilePickerPanel(DispatcherQueue dispatcherQueue)
        : this(dispatcherQueue, ExecutableFilePickerCatalog.GetExtensionFilters())
    {
    }

    public ExecutableFilePickerPanel(
        DispatcherQueue dispatcherQueue,
        IReadOnlyList<ExecutableFilePickerExtensionFilterItem> extensionFilters)
        : this(dispatcherQueue, extensionFilters, foldersOnly: false, initialDirectory: null)
    {
    }

    /// <summary>Folder-Picker: только папки, без combo фильтров расширений.</summary>
    public ExecutableFilePickerPanel(
        DispatcherQueue dispatcherQueue,
        bool foldersOnly,
        string? initialDirectory)
        : this(
            dispatcherQueue,
            Array.Empty<ExecutableFilePickerExtensionFilterItem>(),
            foldersOnly,
            initialDirectory)
    {
    }

    private ExecutableFilePickerPanel(
        DispatcherQueue dispatcherQueue,
        IReadOnlyList<ExecutableFilePickerExtensionFilterItem> extensionFilters,
        bool foldersOnly,
        string? initialDirectory)
    {
        _dispatcherQueue = dispatcherQueue;
        _foldersOnly = foldersOnly;
        if (foldersOnly)
        {
            _extensionFilters = Array.Empty<ExecutableFilePickerExtensionFilterItem>();
            _catalogExtensionFilter = null;
        }
        else
        {
            _extensionFilters = extensionFilters.Count > 0
                ? extensionFilters
                : ExecutableFilePickerCatalog.GetExtensionFilters();
            _catalogExtensionFilter = _extensionFilters[0].ResolveExtensions();
        }

        TreeRoots = ExecutableFilePickerTreeHelper.BuildRootItems();
        _currentPath = ResolveInitialDirectory(initialDirectory);
        SearchPlaceholderText = GlassSettingsResources.Get("Attach_FilePickerSearch");
        AddressPathPlaceholderText = GlassSettingsResources.Get("Attach_FilePickerPath");
        EmptyListText = GlassSettingsResources.Get("Attach_FilePickerEmpty");
        SearchNoResultsText = GlassSettingsResources.Get("Attach_FilePickerSearchNoResults");
        SearchInProgressText = GlassSettingsResources.Get("Attach_FilePickerSearchInProgress");
        SearchParametersButtonText = GlassSettingsResources.Get("Attach_FilePickerSearchParameters");
        CloseSearchButtonText = GlassSettingsResources.Get("Attach_FilePickerCloseSearch");
        SortButtonText = GlassSettingsResources.Get("Attach_FilePickerSort");
        ViewButtonText = GlassSettingsResources.Get("Attach_FilePickerView");
        DetailsColumnNameText = GlassSettingsResources.Get("Attach_FilePickerColumnName");
        DetailsColumnTypeText = GlassSettingsResources.Get("Attach_FilePickerColumnType");
        InitializeComponent();
        ToolTipService.SetToolTip(BackButton, GlassSettingsResources.Get("Attach_FilePickerGoBack"));
        ToolTipService.SetToolTip(ForwardButton, GlassSettingsResources.Get("Attach_FilePickerGoForward"));
        ToolTipService.SetToolTip(RefreshButton, GlassSettingsResources.Get("Attach_FilePickerRefresh"));
        ToolTipService.SetToolTip(HomeButton, GlassSettingsResources.Get("Attach_FilePickerHome"));
        ToolTipService.SetToolTip(TreePaneToggleButton, GlassSettingsResources.Get("Attach_FilePickerFolders"));
        WireCommandBarFlyouts();
        WireSearchParametersFlyout();
        _searchDebounceTimer = _dispatcherQueue.CreateTimer();
        _searchDebounceTimer.Interval = TimeSpan.FromMilliseconds(300);
        _searchDebounceTimer.IsRepeating = false;
        _searchDebounceTimer.Tick += SearchDebounceTimer_OnTick;
        ExecutableFilePickerTreeRow.ExpandCompletedHandler = OnTreeBranchExpandedAsync;
        ExecutableFilePickerTreeRow.NavigableFolderPointerReleasedHandler = OnTreeNavigableFolderPointerReleased;
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    private static string ResolveInitialDirectory(string? initialDirectory)
    {
        if (!string.IsNullOrWhiteSpace(initialDirectory)
            && ExecutableFilePickerCatalog.IsValidDirectory(initialDirectory))
            return ExecutableFilePickerCatalog.NormalizeBrowsablePath(initialDirectory);

        return ExecutableFilePickerCatalog.GetInitialDirectory();
    }

    private void OnUnloaded(object sender, RoutedEventArgs e) =>
        PersistCurrentDirectory();

    public ObservableCollection<ExecutableFilePickerTreeItem> TreeRoots { get; }

    public string SearchPlaceholderText { get; }

    public string AddressPathPlaceholderText { get; }

    public string EmptyListText { get; }

    public string SearchNoResultsText { get; }

    public string SearchInProgressText { get; }

    public string SearchParametersButtonText { get; }

    public string CloseSearchButtonText { get; }

    public string SortButtonText { get; }

    public string ViewButtonText { get; }

    public string DetailsColumnNameText { get; }

    public string DetailsColumnTypeText { get; }

    public void AttachDialogContext(HostedDialogHostContext context)
    {
        _dialogContext = context;
        context.PrimaryButton.Click += PrimaryButton_OnClick;
        if (!_foldersOnly)
            AttachExtensionFilterCombo(context);
        UpdatePrimaryEnabled();
    }

    public string? GetSelectedFilePath()
    {
        if (_selected is null || _selected.IsFolder)
            return null;

        return _selected.FullPath;
    }

    /// <summary>Folder-Picker: выделенная папка или текущий каталог навигации.</summary>
    public string? GetSelectedFolderPath()
    {
        if (!_foldersOnly)
            return null;

        if (_selected is { IsFolder: true })
            return ExecutableFilePickerCatalog.NormalizeBrowsablePath(_selected.FullPath);

        if (!ExecutableFilePickerCatalog.IsValidDirectory(_currentPath))
            return null;

        return ExecutableFilePickerCatalog.NormalizeBrowsablePath(_currentPath);
    }

    public string? GetSelectedExeName()
    {
        if (_selected is null || _selected.IsFolder)
            return null;

        return AttachListRegistry.NormalizeExeName(Path.GetFileName(_selected.FullPath));
    }

    private void WireCommandBarFlyouts()
    {
        var sortFlyout = new MenuFlyout();
        sortFlyout.Items.Add(CreateSortMenuItem("Attach_SortNameAsc", descending: false));
        sortFlyout.Items.Add(CreateSortMenuItem("Attach_SortNameDesc", descending: true));
        SortButton.Flyout = sortFlyout;

        var viewFlyout = new MenuFlyout();
        viewFlyout.Items.Add(CreateViewMenuItem("Attach_ViewList", FilePickerViewMode.List));
        viewFlyout.Items.Add(CreateViewMenuItem("Attach_ViewDetails", FilePickerViewMode.Details));
        viewFlyout.Items.Add(CreateViewMenuItem("Attach_ViewLargeIcons", FilePickerViewMode.LargeIcons));
        ViewButton.Flyout = viewFlyout;
    }

    private void WireSearchParametersFlyout()
    {
        var flyout = new MenuFlyout();

        _scopeAllNestedItem = CreateScopeMenuItem(
            "Attach_FilePickerSearchScopeAllNested",
            "\uE8B7",
            ExecutableFilePickerSearchScope.AllNestedFolders,
            isDefault: true);
        _scopeCurrentFolderItem = CreateScopeMenuItem(
            "Attach_FilePickerSearchScopeCurrent",
            "\uE8B8",
            ExecutableFilePickerSearchScope.CurrentFolder,
            isDefault: false);
        flyout.Items.Add(_scopeAllNestedItem);
        flyout.Items.Add(_scopeCurrentFolderItem);

        // Folder-Picker: без фильтров типа/размера (только папки).
        if (!_foldersOnly)
        {
            flyout.Items.Add(new MenuFlyoutSeparator());

            var typeSubMenu = new MenuFlyoutSubItem
            {
                Text = GlassSettingsResources.Get("Attach_FilePickerSearchType"),
                Icon = new FontIcon { Glyph = "\uE7C3" }
            };
            for (var i = 0; i < _extensionFilters.Count; i++)
            {
                var filter = _extensionFilters[i];
                var resolved = filter.ResolveExtensions();
                var typeExtension = resolved is { Count: 1 } ? resolved[0] : null;
                typeSubMenu.Items.Add(CreateTypeMenuItem(filter.DisplayName, typeExtension, isDefault: i == 0));
            }

            flyout.Items.Add(typeSubMenu);

            var sizeSubMenu = new MenuFlyoutSubItem
            {
                Text = GlassSettingsResources.Get("Attach_FilePickerSearchSize"),
                Icon = new FontIcon { Glyph = "\uE74C" }
            };
            _sizeAnyItem = CreateSizeMenuItem(GlassSettingsResources.Get("Attach_FilePickerSearchAnySize"), null, isDefault: true);
            sizeSubMenu.Items.Add(_sizeAnyItem);
            sizeSubMenu.Items.Add(CreateSizeMenuItem(
                GlassSettingsResources.Get("Attach_FilePickerSearchSizeSmall"),
                ExecutableFilePickerSearchSizeFilter.Small,
                isDefault: false));
            sizeSubMenu.Items.Add(CreateSizeMenuItem(
                GlassSettingsResources.Get("Attach_FilePickerSearchSizeMedium"),
                ExecutableFilePickerSearchSizeFilter.Medium,
                isDefault: false));
            sizeSubMenu.Items.Add(CreateSizeMenuItem(
                GlassSettingsResources.Get("Attach_FilePickerSearchSizeLarge"),
                ExecutableFilePickerSearchSizeFilter.Large,
                isDefault: false));

            flyout.Items.Add(sizeSubMenu);
        }

        SearchParametersButton.Flyout = flyout;
    }

    private RadioMenuFlyoutItem CreateScopeMenuItem(
        string resourceKey,
        string glyph,
        ExecutableFilePickerSearchScope scope,
        bool isDefault)
    {
        var item = new RadioMenuFlyoutItem
        {
            Text = GlassSettingsResources.Get(resourceKey),
            GroupName = "search_scope",
            IsChecked = isDefault,
            Icon = new FontIcon { Glyph = glyph }
        };
        item.Click += (_, _) =>
        {
            _searchOptions.Scope = scope;
            _ = RerunSearchIfActiveAsync(SearchOptionsChangeKind.Scope);
        };
        return item;
    }

    private RadioMenuFlyoutItem CreateTypeMenuItem(string text, string? extension, bool isDefault)
    {
        var item = new RadioMenuFlyoutItem
        {
            Text = text,
            GroupName = "search_type",
            IsChecked = isDefault
        };
        item.Click += (_, _) =>
        {
            _searchOptions.TypeExtension = extension;
            _ = RerunSearchIfActiveAsync(SearchOptionsChangeKind.Type);
        };
        return item;
    }

    private RadioMenuFlyoutItem CreateSizeMenuItem(
        string text,
        ExecutableFilePickerSearchSizeFilter? sizeFilter,
        bool isDefault)
    {
        var item = new RadioMenuFlyoutItem
        {
            Text = text,
            GroupName = "search_size",
            IsChecked = isDefault
        };
        item.Click += (_, _) =>
        {
            _searchOptions.SizeFilter = sizeFilter;
            _ = RerunSearchIfActiveAsync(SearchOptionsChangeKind.Size);
        };
        return item;
    }

    private enum SearchOptionsChangeKind
    {
        Scope,
        Type,
        Size,
        Catalog
    }

    private Task RerunSearchIfActiveAsync(SearchOptionsChangeKind changeKind)
    {
        if (!_isSearchMode || string.IsNullOrWhiteSpace(_activeSearchQuery))
            return Task.CompletedTask;

        if (changeKind is SearchOptionsChangeKind.Scope or SearchOptionsChangeKind.Catalog
            || !CanRefineSearchResultsInMemory(changeKind))
            return RestartSearchWithProgressAsync();

        ApplySearchResultsFromRaw();
        return Task.CompletedTask;
    }

    private bool HasValidRawSearchCache() =>
        !string.IsNullOrWhiteSpace(_lastDiskSearchQuery)
        && string.Equals(_activeSearchQuery, _lastDiskSearchQuery, StringComparison.Ordinal)
        && _searchOptions.Scope == _lastDiskSearchScope
        && ExecutableFilePickerCatalog.ExtensionFiltersEqual(
            _catalogExtensionFilter,
            _lastDiskSearchCatalogFilter)
        && _rawSearchResults.Count > 0;

    private bool CanRefineSearchResultsInMemory(SearchOptionsChangeKind changeKind)
    {
        if (!HasValidRawSearchCache())
            return false;

        return changeKind switch
        {
            SearchOptionsChangeKind.Type => _lastDiskSearchTypeExtension is null,
            SearchOptionsChangeKind.Size => _lastDiskSearchSizeFilter is null,
            _ => false
        };
    }

    private void ApplySearchResultsFromRaw()
    {
        _searchResults.Clear();
        _searchResults.AddRange(
            ExecutableFilePickerSearchService.ApplyClientFilters(_rawSearchResults, _searchOptions));
        ApplySearchResults();
        FileListView.SelectedItem = null;
        UpdatePrimaryEnabled();
    }

    private Task RestartSearchWithProgressAsync()
    {
        if (!_isSearchMode || string.IsNullOrWhiteSpace(_activeSearchQuery))
            return Task.CompletedTask;

        _searchResults.Clear();
        _rawSearchResults.Clear();
        _isSearchInProgress = true;
        ReplaceFilteredItems(new List<ExecutableFilePickerItem>(), syncLayout: false);
        return RunSearchAsync(_activeSearchQuery);
    }

    private void OnTreeNavigableFolderPointerReleased(ExecutableFilePickerTreeItem item, TreeView tree)
    {
        if (!item.IsNavigableFolder || item.FullPath is null)
            return;

        if (ExecutableFilePickerCatalog.PathsEqual(_currentPath, item.FullPath))
            return;

        ScheduleTreeNavigation(item, tree, userInitiated: true);
    }

    private void ScheduleTreeNavigation(ExecutableFilePickerTreeItem item, TreeView tree, bool userInitiated)
    {
        _pendingTreeNavigateItem = item;
        _pendingTreeNavigateView = tree;
        _pendingTreeNavigateUserInitiated = userInitiated;
        _dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Normal, ProcessPendingTreeNavigationAsync);
    }

    private MenuFlyoutItem CreateSortMenuItem(string resourceKey, bool descending)
    {
        var item = new MenuFlyoutItem { Text = GlassSettingsResources.Get(resourceKey) };
        item.Click += (_, _) =>
        {
            _sortColumn = FilePickerSortColumn.Name;
            _sortDescending = descending;
            ApplyFilter();
        };
        return item;
    }

    private MenuFlyoutItem CreateViewMenuItem(string resourceKey, FilePickerViewMode mode)
    {
        var item = new MenuFlyoutItem { Text = GlassSettingsResources.Get(resourceKey) };
        item.Click += (_, _) => SetViewMode(mode);
        return item;
    }

    private void SetViewMode(FilePickerViewMode mode, bool refreshIcons = true)
    {
        if (_viewMode == mode)
            return;

        _viewMode = mode;
        var listTemplate = Application.Current.Resources["ExecutableFilePickerItemTemplate"] as DataTemplate;
        var detailsTemplate = Application.Current.Resources["ExecutableFilePickerDetailsItemTemplate"] as DataTemplate;
        var largeTemplate = Application.Current.Resources["ExecutableFilePickerLargeIconItemTemplate"] as DataTemplate;

        FileListView.ItemTemplate = mode switch
        {
            FilePickerViewMode.Details => detailsTemplate,
            FilePickerViewMode.LargeIcons => largeTemplate,
            _ => listTemplate
        };

        FileListView.ItemsPanel = mode == FilePickerViewMode.LargeIcons
            ? (ItemsPanelTemplate)Resources["FilePickerLargeIconsItemsPanel"]
            : (ItemsPanelTemplate)Resources["FilePickerListItemsPanel"];

        FileListView.ItemContainerStyle = mode == FilePickerViewMode.LargeIcons
            ? (Style)Resources["FilePickerLargeIconItemContainerStyle"]
            : null;

        DetailsHeaderRow.Visibility = mode == FilePickerViewMode.Details
            ? Visibility.Visible
            : Visibility.Collapsed;
        UpdateDetailsHeaderSortIcons();
        ApplyContentScrollInsets();
        SyncTreeListLayout("view_mode");

        if (refreshIcons && _allItems.Count > 0 && !_isSearchMode)
            ScheduleListIconsRefresh();
    }

    private void ScheduleListIconsRefresh()
    {
        var iconGeneration = ++_listIconGeneration;
        ExecutableFilePickerListIconResolver.ApplyFolderIcons(_allItems);
        _ = ExecutableFilePickerListIconResolver.ApplyIconsAsync(
            _allItems,
            _dispatcherQueue,
            iconGeneration,
            gen => gen == _listIconGeneration,
            preferImageThumbnails: _viewMode == FilePickerViewMode.LargeIcons);
    }

    private void UpdateSearchGlyphVisibility() =>
        SearchGlyph.Visibility = string.IsNullOrEmpty(SearchBox.Text)
            ? Visibility.Visible
            : Visibility.Collapsed;

    private void SortByColumn(FilePickerSortColumn column)
    {
        if (_sortColumn == column)
            _sortDescending = !_sortDescending;
        else
        {
            _sortColumn = column;
            _sortDescending = false;
        }

        ApplyFilter();
    }

    private void UpdateDetailsHeaderSortIcons()
    {
        SetSortIcon(DetailsHeaderNameSortIcon, FilePickerSortColumn.Name);
        SetSortIcon(DetailsHeaderTypeSortIcon, FilePickerSortColumn.Type);
    }

    private void SetSortIcon(FontIcon icon, FilePickerSortColumn column)
    {
        if (_sortColumn != column)
        {
            icon.Visibility = Visibility.Collapsed;
            return;
        }

        icon.Glyph = _sortDescending ? "\uE70D" : "\uE70E";
        icon.Visibility = Visibility.Visible;
    }

    private void DetailsHeaderName_OnClick(object sender, RoutedEventArgs e) =>
        SortByColumn(FilePickerSortColumn.Name);

    private void DetailsHeaderType_OnClick(object sender, RoutedEventArgs e) =>
        SortByColumn(FilePickerSortColumn.Type);

    private void AttachExtensionFilterCombo(HostedDialogHostContext context)
    {
        if (context.FooterRow is null)
            return;

        var combo = new ComboBox
        {
            Width = 220,
            MinHeight = 32,
            MaxHeight = 32,
            VerticalAlignment = VerticalAlignment.Center,
            VerticalContentAlignment = VerticalAlignment.Center,
            HorizontalAlignment = HorizontalAlignment.Left,
            Margin = new Thickness(0, 0, 12, 0),
            ItemsSource = _extensionFilters,
            DisplayMemberPath = nameof(ExecutableFilePickerExtensionFilterItem.DisplayName),
            SelectedIndex = 0
        };
        combo.SelectionChanged += ExtensionFilterCombo_OnSelectionChanged;

        context.FooterRow.Children.Insert(0, combo);
        Grid.SetColumn(combo, 0);
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        if (_initialized)
            return;

        _initialized = true;
        FileListView.ItemsSource = _filtered;
        ApplyContentScrollInsets();
        ExecutableFilePickerTreeScrollHelper.ConfigureTreeView(FolderTreeView);
        ExecutableFilePickerTreeScrollHelper.ConfigureTreeView(FolderTreePaneView);
        ExecutableFilePickerTreeScrollHelper.ScheduleScrollExtentRefresh(FolderTreeView, _dispatcherQueue);        EnsureBarHostBrushes();
        EnsureBarHostHoverTracking();
        UpdateSearchGlyphVisibility();
        foreach (var root in TreeRoots)
            ShellFolderIconResolver.ApplyIconsRecursive(root);

        WindowBackdropService.ApplyAcrylicInAppChromeBackground(TreePanePanel);
        ExecutableFilePickerTreeRow.PrewarmHoverTooltip(XamlRoot);
        UpdateLayoutMode(ActualWidth);
        SyncTreeListLayout("loaded");
        _ = NavigateListAsync(_currentPath, syncTree: true, recordHistory: false);
    }

    /// <summary>Сохраняет текущую папку в реестр (вызывается при закрытии диалога).</summary>
    public void PersistLastDirectory() => PersistCurrentDirectory();

    private void PersistCurrentDirectory()
    {
        // Folder-Picker не пишет ExecutableFilePickerLastPath — стартовый путь задаёт call site.
        if (_foldersOnly || string.IsNullOrWhiteSpace(_currentPath))
            return;

        ExecutableFilePickerCatalog.RememberLastDirectory(_currentPath);
    }

    public double? GetHostedDialogMinWidth() =>
        _isInlineTreeVisible
            ? ExecutableFilePickerLayoutMetrics.InlineDialogMinWidth
            : ExecutableFilePickerLayoutMetrics.CompactDialogMinWidth;

    public void OnHostedDialogBoundsChanged(double width, double height)
    {
        UpdateLayoutMode(width > 0 ? width : ActualWidth);
        ApplyDialogHeightConstraint(height);
    }

    private void ApplyDialogHeightConstraint(double dialogHeight)
    {
        if (dialogHeight <= 0)
        {
            ClearValue(MaxHeightProperty);
            ContentHostGrid.ClearValue(FrameworkElement.MinHeightProperty);
            return;
        }

        var panelBudget = ExecutableFilePickerLayoutMetrics.ComputeHostedPanelBudget(dialogHeight);
        if (panelBudget > 0)
            MaxHeight = panelBudget;

        var treeBudget = ExecutableFilePickerLayoutMetrics.ComputeTreePaneBudget(dialogHeight);
        ContentHostGrid.MinHeight = treeBudget;
        SyncTreeListLayout("dialog_height");
        ExecutableFilePickerTreeScrollHelper.ScheduleScrollExtentRefresh(FolderTreeView, _dispatcherQueue);
        if (TreePaneOverlay.Visibility == Visibility.Visible)
            ExecutableFilePickerTreeScrollHelper.ScheduleScrollExtentRefresh(FolderTreePaneView, _dispatcherQueue);
    }

    private void Root_OnSizeChanged(object sender, SizeChangedEventArgs e) =>
        UpdateLayoutMode(e.NewSize.Width);

    private void UpdateLayoutMode(double width)
    {
        if (width <= 0)
            return;

        var expandInline = width >= ExecutableFilePickerLayoutMetrics.CompactBreakpoint
            + ExecutableFilePickerLayoutMetrics.Hysteresis;
        var collapseCompact = width <= ExecutableFilePickerLayoutMetrics.CompactBreakpoint
            - ExecutableFilePickerLayoutMetrics.Hysteresis;

        if (expandInline)
        {
            if (_isInlineTreeVisible && TreePaneOverlay.Visibility == Visibility.Collapsed)
                return;

            ApplyInlineTreeLayout();
            return;
        }

        if (collapseCompact)
        {
            if (!_isInlineTreeVisible && TreePaneOverlay.Visibility == Visibility.Collapsed)
                return;

            ApplyCompactTreeLayout();
        }
    }

    private void ApplyInlineTreeLayout()
    {
        var minWidthChanged = !_isInlineTreeVisible;
        _isInlineTreeVisible = true;
        TreeColumn.Width = new GridLength(ExecutableFilePickerLayoutMetrics.TreePaneWidth);
        TreeListGutterColumn.Width = new GridLength(ExecutableFilePickerLayoutMetrics.TreeListGutter);
        TreeListDividerColumn.Width = new GridLength(ExecutableFilePickerLayoutMetrics.TreeListDividerWidth);
        TreeListDivider.Visibility = Visibility.Visible;
        FolderTreeView.Visibility = Visibility.Visible;
        TreePaneToggleButton.Visibility = Visibility.Collapsed;
        CloseTreePaneOverlay();
        if (minWidthChanged)
            _dialogContext?.RequestBoundsRefresh();
    }

    private void ApplyCompactTreeLayout()
    {
        var minWidthChanged = _isInlineTreeVisible;
        _isInlineTreeVisible = false;
        TreeColumn.Width = new GridLength(0);
        TreeListGutterColumn.Width = new GridLength(0);
        TreeListDividerColumn.Width = new GridLength(0);
        TreeListDivider.Visibility = Visibility.Collapsed;
        FolderTreeView.Visibility = Visibility.Collapsed;
        TreePaneToggleButton.Visibility = Visibility.Visible;
        if (minWidthChanged)
            _dialogContext?.RequestBoundsRefresh();
    }

    private void TreePaneToggleButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (TreePaneOverlay.Visibility == Visibility.Visible)
            CloseTreePaneOverlay();
        else
            OpenTreePaneOverlay();
    }

    private void OpenTreePaneOverlay()
    {
        WindowBackdropService.ApplyAcrylicInAppChromeBackground(TreePanePanel);
        TreePaneOverlay.Visibility = Visibility.Visible;
        ExecutableFilePickerTreeScrollHelper.ConfigureTreeView(FolderTreePaneView);        SyncTreeSelection(_currentPath);
    }

    private void CloseTreePaneOverlay() =>
        TreePaneOverlay.Visibility = Visibility.Collapsed;

    private void TreePaneDismissLayer_OnPointerPressed(object sender, PointerRoutedEventArgs e) =>
        CloseTreePaneOverlay();

    private void EnsureBarHostHoverTracking()
    {
        if (_barHostHoverRegistered)
            return;

        _barHostHoverRegistered = true;
        RegisterBarHostHoverTracking(FileExplorerAddressBarGrid);
        RegisterBarHostHoverTracking(FileExplorerSearchBox);
        NavigationBarInnerGrid.PointerExited += NavigationBarInnerGrid_OnPointerExited;
    }

    private void RegisterBarHostHoverTracking(Border border)
    {
        border.AddHandler(UIElement.PointerEnteredEvent, new PointerEventHandler((_, e) => BarHost_OnPointerEnteredRouted(border, e)), true);
        border.AddHandler(UIElement.PointerExitedEvent, new PointerEventHandler((_, e) => BarHost_OnPointerExitedRouted(border, e)), true);
    }

    private void BarHost_OnPointerEnteredRouted(Border border, PointerRoutedEventArgs e)
    {
        if (_barHostHoverBrush is null)
            return;

        if (!IsDescendantOf(e.OriginalSource as DependencyObject, border))
            return;

        if (ShouldSkipBarHostHover(border))
            return;

        ApplyBarHostHover(border);
    }

    private void BarHost_OnPointerExitedRouted(Border border, PointerRoutedEventArgs e)
    {
        if (_barHostNormalBrush is null)
            return;

        if (!IsDescendantOf(e.OriginalSource as DependencyObject, border))
            return;

        var stillOver = IsPointerOverBarHostSubtree(border, e);

        if (stillOver)
            return;

        ResetBarHostBackground(border);
    }

    private void NavigationBarInnerGrid_OnPointerExited(object sender, PointerRoutedEventArgs e)
    {
        if (sender is not FrameworkElement grid)
            return;

        var point = e.GetCurrentPoint(grid).Position;

        if (!IsPointerOverBarHostSubtreeAtPoint(FileExplorerAddressBarGrid, point, grid))
            ResetBarHostBackground(FileExplorerAddressBarGrid);

        if (!IsPointerOverBarHostSubtreeAtPoint(FileExplorerSearchBox, point, grid))
            ResetBarHostBackground(FileExplorerSearchBox);
    }

    private bool ShouldSkipBarHostHover(Border border)
    {
        if (ReferenceEquals(border, FileExplorerSearchBox) && SearchBox.FocusState != FocusState.Unfocused)
            return true;

        return ReferenceEquals(border, FileExplorerAddressBarGrid) &&
               (_isAddressEditMode || AddressPathBox.FocusState != FocusState.Unfocused);
    }

    private void ApplyBarHostHover(Border border)
    {
        if (_barHostHoverBrush is null)
            return;

        if (ReferenceEquals(border, FileExplorerAddressBarGrid))
            ResetBarHostBackground(FileExplorerSearchBox);
        else if (ReferenceEquals(border, FileExplorerSearchBox))
            ResetBarHostBackground(FileExplorerAddressBarGrid);

        border.Background = _barHostHoverBrush;
    }

    private void ResetBarHostBackground(Border border)
    {
        if (_barHostNormalBrush is null)
            return;

        border.Background = _barHostNormalBrush;
    }

    private static bool IsPointerOverBarHostSubtree(Border border, PointerRoutedEventArgs e) =>
        IsPointerOverBarHostSubtreeAtPoint(border, e.GetCurrentPoint(border).Position, border);

    private static bool IsPointerOverBarHostSubtreeAtPoint(Border border, Point point, UIElement coordinateRoot)
    {
        var localPoint = coordinateRoot.TransformToVisual(border).TransformPoint(point);
        var hits = VisualTreeHelper.FindElementsInHostCoordinates(localPoint, border);
        foreach (var _ in hits)
            return true;

        return false;
    }

    private static bool IsDescendantOf(DependencyObject? node, DependencyObject ancestor)
    {
        while (node is not null)
        {
            if (ReferenceEquals(node, ancestor))
                return true;

            node = VisualTreeHelper.GetParent(node);
        }

        return false;
    }

    private void EnsureBarHostBrushes()
    {
        if (_barHostNormalBrush is not null)
            return;

        _barHostNormalBrush = Application.Current.Resources["LayerFillColorDefaultBrush"] as Brush;
        // Как у спойлеров (SpoilerExpanderChrome.AttachHeaderHover): заметный hover поверх layer-фона.
        _barHostHoverBrush = Application.Current.Resources["SubtleFillColorSecondaryBrush"] as Brush;
    }

    private void FilePickerBarHost_OnPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        if (!ReferenceEquals(sender, FileExplorerAddressBarGrid))
            return;

        TryEnterAddressEditModeFromInput(e.OriginalSource);
    }

    private void FileExplorerAddressBarGrid_OnTapped(object sender, TappedRoutedEventArgs e) =>
        TryEnterAddressEditModeFromInput(e.OriginalSource);

    private void TryEnterAddressEditModeFromInput(object? originalSource)
    {
        if (IsHomeButtonHit(originalSource))
            return;

        if (IsBreadcrumbInteractiveHit(originalSource))
            return;

        EnterAddressEditMode();
    }

    private bool IsHomeButtonHit(object? source)
    {
        if (source is not DependencyObject node)
            return false;

        while (node is not null)
        {
            if (ReferenceEquals(node, HomeButton))
                return true;

            if (ReferenceEquals(node, FileExplorerAddressBarGrid))
                return false;

            node = VisualTreeHelper.GetParent(node);
        }

        return false;
    }

    private void AddressBarContentHost_OnTapped(object sender, TappedRoutedEventArgs e) =>
        TryEnterAddressEditModeFromInput(e.OriginalSource);

    private bool IsBreadcrumbInteractiveHit(object? source)
    {
        if (source is not DependencyObject node)
            return false;

        while (node is not null)
        {
            if (node is BreadcrumbBarItem)
                return true;

            if (ReferenceEquals(node, FileExplorerAddressBarGrid) || ReferenceEquals(node, AddressBarContentHost))
                return false;

            node = VisualTreeHelper.GetParent(node);
        }

        return false;
    }

    private void EnterAddressEditMode()
    {
        if (_isAddressEditMode)
            return;

        _isAddressEditMode = true;
        PathBreadcrumb.Visibility = Visibility.Collapsed;
        AddressPathBox.Visibility = Visibility.Visible;
        AddressPathBox.Text = _currentPath;
        AddressPathBox.Focus(FocusState.Programmatic);
        SelectAddressPathAll();
    }

    private void SelectAddressPathAll()
    {
        AddressPathBox.SelectAll();
        _dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, () =>
        {
            if (!_isAddressEditMode || AddressPathBox.FocusState == FocusState.Unfocused)
                return;

            AddressPathBox.SelectAll();
        });
    }

    private void ExitAddressEditMode()
    {
        if (!_isAddressEditMode)
            return;

        _isAddressEditMode = false;
        AddressPathBox.Visibility = Visibility.Collapsed;
        PathBreadcrumb.Visibility = Visibility.Visible;

        if (_barHostNormalBrush is not null)
            ResetBarHostBackground(FileExplorerAddressBarGrid);
    }

    private async void AddressPathBox_OnKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key == VirtualKey.Enter)
        {
            e.Handled = true;
            await TryCommitAddressAsync().ConfigureAwait(true);
            return;
        }

        if (e.Key == VirtualKey.Escape)
        {
            e.Handled = true;
            ExitAddressEditMode();
        }
    }

    private void AddressPathBox_OnLostFocus(object sender, RoutedEventArgs e)
    {
        if (_suppressAddressLostFocus)
            return;

        ExitAddressEditMode();
    }

    private async Task TryCommitAddressAsync()
    {
        if (!ExecutableFilePickerPathHelper.TryResolveDirectoryInput(AddressPathBox.Text, out var resolved))
            return;

        _suppressAddressLostFocus = true;
        try
        {
            ExitAddressEditMode();
            await NavigateListAsync(resolved, syncTree: true).ConfigureAwait(true);
            RestoreAddressBarFocusDeferred();
        }
        finally
        {
            _suppressAddressLostFocus = false;
        }
    }

    private void RestoreAddressBarFocusDeferred()
    {
        _dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, () =>
        {
            if (ReferenceEquals(FocusManager.GetFocusedElement(XamlRoot), SearchBox))
                AddressBarContentHost.Focus(FocusState.Programmatic);
        });
    }

    private void UpdateNavButtons()
    {
        BackButton.IsEnabled = _isSearchMode || _backHistory.Count > 0;
        ForwardButton.IsEnabled = _forwardHistory.Count > 0;
    }

    private void UpdateSearchChrome()
    {
        SearchCommandBarPanel.Visibility = _isSearchMode
            ? Visibility.Visible
            : Visibility.Collapsed;
    }

    private ExecutableFilePickerNavigationEntry CreateCurrentNavigationEntry() =>
        new()
        {
            FolderPath = _currentPath,
            IsSearchPage = _isSearchMode,
            SearchQuery = _isSearchMode ? _activeSearchQuery : null
        };

    private void UpdateBreadcrumb()
    {
        _breadcrumbTrail.Clear();
        _breadcrumbTrail.AddRange(ExecutableFilePickerPathHelper.BuildTrail(_currentPath));
        PathBreadcrumb.ItemsSource = _breadcrumbTrail.ToList();
    }

    private void ApplyFilter()
    {
        if (_isSearchMode)
            ApplySearchResults();
        else
            ApplyBrowseList();
    }

    private void ApplyBrowseList()
    {
        var items = new List<ExecutableFilePickerItem>(_allItems);
        items.Sort(CompareFilteredItems);
        ReplaceFilteredItems(items);
    }

    private void ApplySearchResults()
    {
        var items = new List<ExecutableFilePickerItem>(_searchResults);
        items.Sort(CompareFilteredItems);
        ReplaceFilteredItems(items, syncLayout: false);
    }

    private void ReplaceFilteredItems(List<ExecutableFilePickerItem> items, bool syncLayout = true)
    {
        _filtered.Clear();
        foreach (var item in items)
            _filtered.Add(item);

        UpdateListEmptyState();
        UpdateDetailsHeaderSortIcons();
        ApplyContentScrollInsets();
        if (syncLayout)
            SyncTreeListLayout("filter");
    }

    private void UpdateListEmptyState()
    {
        var hasItems = _filtered.Count > 0;
        if (_isSearchMode && _isSearchInProgress && !hasItems)
        {
            SearchProgressPanel.Visibility = Visibility.Visible;
            SearchProgressRing.IsActive = true;
            SearchEmptyText.Visibility = Visibility.Collapsed;
            EmptyText.Visibility = Visibility.Collapsed;
            FileListView.Visibility = Visibility.Collapsed;
            return;
        }

        SearchProgressPanel.Visibility = Visibility.Collapsed;
        SearchProgressRing.IsActive = false;

        if (_isSearchMode)
        {
            SearchEmptyText.Visibility = hasItems ? Visibility.Collapsed : Visibility.Visible;
            EmptyText.Visibility = Visibility.Collapsed;
        }
        else
        {
            SearchEmptyText.Visibility = Visibility.Collapsed;
            EmptyText.Visibility = hasItems ? Visibility.Collapsed : Visibility.Visible;
        }

        FileListView.Visibility = hasItems ? Visibility.Visible : Visibility.Collapsed;
    }

    private void EnsureSearchModeEntered(bool recordHistory = true)
    {
        if (_isSearchMode)
            return;

        if (recordHistory)
        {
            _backHistory.Push(CreateCurrentNavigationEntry());
            _forwardHistory.Clear();
        }

        _isSearchMode = true;
        UpdateSearchChrome();
        UpdateNavButtons();
        _searchResults.Clear();
        _isSearchInProgress = true;
        ReplaceFilteredItems(new List<ExecutableFilePickerItem>(), syncLayout: false);
    }

    private async Task EnterSearchPageAsync(string query, bool recordHistory = true)
    {
        var normalized = query.Trim();
        if (string.IsNullOrEmpty(normalized))
            return;

        EnsureSearchModeEntered(recordHistory);
        _activeSearchQuery = normalized;
        await RunSearchAsync(normalized).ConfigureAwait(true);
    }

    private async Task CloseSearchPageAsync(bool recordForward = true)
    {
        if (!_isSearchMode)
            return;

        if (recordForward)
            _forwardHistory.Push(CreateCurrentNavigationEntry());

        if (_backHistory.Count > 0 && _backHistory.Peek() is { IsSearchPage: false } entry
            && ExecutableFilePickerCatalog.PathsEqual(entry.FolderPath, _currentPath))
            _backHistory.Pop();

        _searchCancellation?.Cancel();
        _searchCancellation = null;
        _searchDebounceTimer.Stop();
        _pendingSearchQuery = string.Empty;
        _isSearchMode = false;
        _isSearchInProgress = false;
        _activeSearchQuery = string.Empty;
        _searchResults.Clear();
        _rawSearchResults.Clear();
        _lastDiskSearchQuery = string.Empty;

        _suppressSearchTextChanged = true;
        try
        {
            SearchBox.Text = string.Empty;
        }
        finally
        {
            _suppressSearchTextChanged = false;
        }

        UpdateSearchChrome();
        ApplyBrowseList();
        FileListView.SelectedItem = null;
        UpdatePrimaryEnabled();
        UpdateNavButtons();
        await Task.CompletedTask.ConfigureAwait(true);
    }

    private async Task RunSearchAsync(string query)
    {
        var generation = ++_searchGeneration;
        _searchCancellation?.Cancel();
        _searchCancellation = new CancellationTokenSource();
        var token = _searchCancellation.Token;

        try
        {
            var results = await ExecutableFilePickerSearchService
                .SearchAsync(
                    _currentPath,
                    query,
                    _searchOptions,
                    _catalogExtensionFilter,
                    token,
                    foldersOnly: _foldersOnly)
                .ConfigureAwait(true);

            if (generation != _searchGeneration || !_isSearchMode)
                return;

            _lastDiskSearchQuery = query;
            _lastDiskSearchScope = _searchOptions.Scope;
            _lastDiskSearchCatalogFilter = _catalogExtensionFilter;
            _lastDiskSearchTypeExtension = _searchOptions.TypeExtension;
            _lastDiskSearchSizeFilter = _searchOptions.SizeFilter;

            _rawSearchResults.Clear();
            _rawSearchResults.AddRange(results);

            _searchResults.Clear();
            _searchResults.AddRange(results);
            ApplySearchResults();
            FileListView.SelectedItem = null;
            UpdatePrimaryEnabled();

            var iconGeneration = ++_listIconGeneration;
            ExecutableFilePickerListIconResolver.ApplyFolderIcons(_searchResults);
            _ = ExecutableFilePickerListIconResolver.ApplyIconsAsync(
                _searchResults,
                _dispatcherQueue,
                iconGeneration,
                gen => gen == _listIconGeneration,
                preferImageThumbnails: _viewMode == FilePickerViewMode.LargeIcons);
        }
        catch (OperationCanceledException)
        {
            return;
        }
        catch (Exception)
        {
            if (generation != _searchGeneration || !_isSearchMode)
                return;

            _searchResults.Clear();
            ApplySearchResults();
            FileListView.SelectedItem = null;
            UpdatePrimaryEnabled();
        }
        finally
        {
            if (generation == _searchGeneration)
                SetSearchInProgress(false);
        }
    }

    private void SetSearchInProgress(bool inProgress)
    {
        _isSearchInProgress = inProgress;
        UpdateListEmptyState();
    }

    private async Task RestoreNavigationEntryAsync(ExecutableFilePickerNavigationEntry entry)
    {
        if (entry.IsSearchPage)
        {
            if (!ExecutableFilePickerCatalog.PathsEqual(_currentPath, entry.FolderPath))
            {
                await NavigateListAsync(
                    entry.FolderPath,
                    syncTree: true,
                    recordHistory: false,
                    closeSearchPage: false).ConfigureAwait(true);
            }

            _suppressSearchTextChanged = true;
            try
            {
                SearchBox.Text = entry.SearchQuery ?? string.Empty;
            }
            finally
            {
                _suppressSearchTextChanged = false;
            }

            await EnterSearchPageAsync(entry.SearchQuery ?? string.Empty, recordHistory: false).ConfigureAwait(true);
            return;
        }

        if (_isSearchMode)
            await CloseSearchPageAsync(recordForward: false).ConfigureAwait(true);

        await NavigateListAsync(entry.FolderPath, syncTree: true, recordHistory: false).ConfigureAwait(true);
    }

    private void ContentHostGrid_OnSizeChanged(object sender, SizeChangedEventArgs e) =>
        SyncTreeListLayout("content_host_size");

    private void ApplyContentScrollInsets()
    {
        var bottom = ExecutableFilePickerLayoutMetrics.GetContentScrollBottomInset(
            _viewMode == FilePickerViewMode.Details);
        ExecutableFilePickerTreeScrollHelper.ApplyTreeScrollInsets(FolderTreeView, bottom);
        ExecutableFilePickerTreeScrollHelper.ApplyTreeScrollInsets(FolderTreePaneView, bottom);
        ExecutableFilePickerTreeScrollHelper.ApplyScrollInsets(FileListView, bottom);
    }

    private void SyncTreeListLayout(string trigger)
    {
        SyncTreeListColumnHeights();    }

    private void SyncTreeListColumnHeights()
    {
        FolderTreeView.ClearValue(FrameworkElement.MaxHeightProperty);
    }

    private int CompareFilteredItems(ExecutableFilePickerItem a, ExecutableFilePickerItem b)
    {
        if (a.IsFolder != b.IsFolder)
            return a.IsFolder ? -1 : 1;

        var comparison = _sortColumn switch
        {
            FilePickerSortColumn.Name => string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase),
            FilePickerSortColumn.Type => string.Compare(
                ExecutableFilePickerDisplayHelper.GetTypeSortKey(a),
                ExecutableFilePickerDisplayHelper.GetTypeSortKey(b),
                StringComparison.OrdinalIgnoreCase),
            _ => 0
        };

        if (comparison == 0 && _sortColumn != FilePickerSortColumn.Name)
            comparison = string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase);

        return _sortDescending ? -comparison : comparison;
    }

    private void UpdatePrimaryEnabled()
    {
        _selected = FileListView.SelectedItem as ExecutableFilePickerItem;
        if (_foldersOnly)
        {
            _dialogContext?.SetPrimaryEnabled(
                ExecutableFilePickerCatalog.IsValidDirectory(_currentPath));
            return;
        }

        var enabled = _selected is not null && !_selected.IsFolder;
        _dialogContext?.SetPrimaryEnabled(enabled);
    }

    private int _treeSyncSuppressGeneration;

    private void ReleaseTreeSyncSuppressDeferred()
    {
        var generation = ++_treeSyncSuppressGeneration;
        _dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, () =>
        {
            if (generation == _treeSyncSuppressGeneration)
                _suppressTreeSync = false;
        });
    }

    private void SyncTreeSelection(string path) =>
        _ = SyncTreeToPathAsync(path);

    private Task SyncTreeToPathAsync(string path)
    {
        _suppressTreeSync = true;
        try
        {
            var item = ExecutableFilePickerTreeHelper.FindPathTreeSelection(TreeRoots, path);
            if (item is null)
            {
                FolderTreeView.SelectedItem = null;
                FolderTreePaneView.SelectedItem = null;
                return Task.CompletedTask;
            }

            ExecutableFilePickerTreeHelper.SelectTreeItem(FolderTreeView, item);
            ExecutableFilePickerTreeHelper.SelectTreeItem(FolderTreePaneView, item);
            EnsureTreeSelectionVisible(item);
        }
        finally
        {
            ReleaseTreeSyncSuppressDeferred();
        }

        return Task.CompletedTask;
    }

    private void EnsureTreeSelectionVisible(ExecutableFilePickerTreeItem item)
    {
        _dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, async () =>
        {
            await Task.Delay(1).ConfigureAwait(true);
            ExecutableFilePickerTreeScrollHelper.BringItemIntoView(FolderTreeView, item);
            ExecutableFilePickerTreeScrollHelper.BringItemIntoView(FolderTreePaneView, item);
        });
    }

    private void RefineTreeSelectionIfRelated(ExecutableFilePickerTreeItem expandedItem)
    {
        if (!ExecutableFilePickerTreeHelper.IsExpandRelatedToPath(expandedItem, _currentPath))
            return;

        if (!expandedItem.IsExpanded)
        {
            SyncTreeSelection(_currentPath);
            return;
        }

        var selection = ExecutableFilePickerTreeHelper.FindDirectPathChild(expandedItem, _currentPath)
            ?? ExecutableFilePickerTreeHelper.FindPathTreeSelection(TreeRoots, _currentPath);
        if (selection is null)
            return;

        _suppressTreeSync = true;
        try
        {
            ExecutableFilePickerTreeHelper.SelectTreeItem(FolderTreeView, selection);
            ExecutableFilePickerTreeHelper.SelectTreeItem(FolderTreePaneView, selection);
            EnsureTreeSelectionVisible(selection);
        }
        finally
        {
            ReleaseTreeSyncSuppressDeferred();
        }
    }

    private async Task ScheduleExpandScrollDeferredAsync(TreeView treeView, ExecutableFilePickerTreeItem item)
    {
        await ExecutableFilePickerTreeScrollHelper
            .RunExpandBranchScrollAsync(treeView, TreeRoots, item, _dispatcherQueue, _currentPath)
            .ConfigureAwait(true);
    }

    private void ScheduleExpandScrollDeferred(TreeView treeView, ExecutableFilePickerTreeItem item) =>
        _dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, () => _ = ScheduleExpandScrollDeferredAsync(treeView, item));

    private Task OnTreeBranchExpandedAsync(ExecutableFilePickerTreeItem item)
    {
        // Сворачивание по double-click: OnExpanding не вызывается — только refine selection.
        // Разворот: load + scroll целиком в FolderTreeView_OnExpanding (без дубля от double-click).
        RefineTreeSelectionIfRelated(item);
        return Task.CompletedTask;
    }

    private async Task NavigateListAsync(
        string path,
        bool syncTree,
        bool recordHistory = true,
        bool forceReload = false,
        bool closeSearchPage = true)
    {
        var generation = ++_navigateGeneration;
        ExitAddressEditMode();

        path = ExecutableFilePickerCatalog.NormalizeBrowsablePath(path);

        if (closeSearchPage && _isSearchMode)
            await CloseSearchPageAsync(recordForward: false).ConfigureAwait(true);

        if (!ExecutableFilePickerCatalog.IsValidDirectory(path))
            return;

        if (!forceReload &&
            ExecutableFilePickerCatalog.PathsEqual(_currentPath, path))
        {
            if (_allItems.Count > 0)
            {
                if (syncTree)
                    await SyncTreeToPathAsync(_currentPath).ConfigureAwait(true);

                PersistCurrentDirectory();
                return;
            }
        }

        if (recordHistory &&
            !_suppressHistoryRecord &&
            !string.IsNullOrEmpty(_currentPath) &&
            !ExecutableFilePickerCatalog.PathsEqual(_currentPath, path))
        {
            _backHistory.Push(CreateCurrentNavigationEntry());
            _forwardHistory.Clear();
        }

        _currentPath = path;
        if (!_foldersOnly)
            ExecutableFilePickerCatalog.RememberLastDirectory(path);
        UpdateBreadcrumb();
        UpdateNavButtons();

        var loaded = _foldersOnly
            ? await ExecutableFilePickerCatalog.LoadSubfoldersAsync(_currentPath).ConfigureAwait(true)
            : await ExecutableFilePickerCatalog
                .LoadDirectoryAsync(_currentPath, _catalogExtensionFilter)
                .ConfigureAwait(true);

        if (generation != _navigateGeneration)
            return;

        _allItems.Clear();
        _allItems.AddRange(loaded);

        if (_isSearchMode)
            return;

        // Среди файлов (не папок) только изображения → крупные значки.
        if (!_foldersOnly && DirectoryFilesAreOnlyImages(_allItems))
            SetViewMode(FilePickerViewMode.LargeIcons, refreshIcons: false);

        ApplyBrowseList();
        FileListView.SelectedItem = null;
        UpdatePrimaryEnabled();
        ScheduleListIconsRefresh();

        if (syncTree)
            await SyncTreeToPathAsync(_currentPath).ConfigureAwait(true);
    }

    private async void FolderTreeView_OnExpanding(TreeView sender, TreeViewExpandingEventArgs args)
    {
        if (args.Node.Content is not ExecutableFilePickerTreeItem item)
            return;
        item.IsExpanded = true;
        if (item.HasUnloadedChildren)
        {
            await ExecutableFilePickerTreeHelper
                .LoadChildrenAsync(item, _dispatcherQueue)
                .ConfigureAwait(true);
        }
        RefineTreeSelectionIfRelated(item);
        ScheduleExpandScrollDeferred(sender, item);
        _dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, () =>
        {        });
    }

    private void FolderTreeView_OnCollapsed(TreeView sender, TreeViewCollapsedEventArgs args)
    {
        if (args.Node.Content is not ExecutableFilePickerTreeItem item)
            return;

        item.IsExpanded = false;
        RefineTreeSelectionIfRelated(item);
    }

    private async void ExtensionFilterCombo_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (sender is not ComboBox combo ||
            combo.SelectedItem is not ExecutableFilePickerExtensionFilterItem filter)
            return;

        var resolved = filter.ResolveExtensions();
        if (ExecutableFilePickerCatalog.ExtensionFiltersEqual(_catalogExtensionFilter, resolved))
            return;

        _catalogExtensionFilter = resolved;
        if (_isSearchMode)
        {
            await RestartSearchWithProgressAsync().ConfigureAwait(true);
            return;
        }

        await NavigateListAsync(_currentPath, syncTree: false, recordHistory: false, forceReload: true)
            .ConfigureAwait(true);
    }

    private void PrimaryButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (_dialogContext is null)
            return;

        if (_foldersOnly)
        {
            if (GetSelectedFolderPath() is null)
                return;

            _dialogContext.ClosePrimary();
            return;
        }

        if (_selected is null || _selected.IsFolder)
            return;

        PersistCurrentDirectory();
        _dialogContext.ClosePrimary();
    }

    private void FolderTreeView_OnSelectionChanged(TreeView sender, TreeViewSelectionChangedEventArgs args)
    {
        if (_suppressTreeSync)
            return;

        // Навигация — только по PointerReleased на TreeViewItem.
    }

    private async void ProcessPendingTreeNavigationAsync()
    {
        var item = _pendingTreeNavigateItem;
        var tree = _pendingTreeNavigateView;
        var userInitiated = _pendingTreeNavigateUserInitiated;
        _pendingTreeNavigateItem = null;
        _pendingTreeNavigateView = null;
        _pendingTreeNavigateUserInitiated = false;

        if (item is null || tree is null || item.FullPath is null)
            return;

        if (_suppressTreeSync && !userInitiated)
            return;

        var pathsEqual = ExecutableFilePickerCatalog.PathsEqual(_currentPath, item.FullPath);
        var selectedMatch = ReferenceEquals(tree.SelectedItem, item);

        if (!userInitiated && !selectedMatch)
            return;

        if (pathsEqual)
            return;
        if (ReferenceEquals(tree, FolderTreePaneView))
            CloseTreePaneOverlay();

        await NavigateListAsync(item.FullPath, syncTree: false).ConfigureAwait(true);
    }

    private async void BackButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (_isSearchMode)
        {
            await CloseSearchPageAsync().ConfigureAwait(true);
            return;
        }

        if (_backHistory.Count == 0)
            return;

        var target = _backHistory.Pop();
        _forwardHistory.Push(CreateCurrentNavigationEntry());
        _suppressHistoryRecord = true;
        try
        {
            await RestoreNavigationEntryAsync(target).ConfigureAwait(true);
        }
        finally
        {
            _suppressHistoryRecord = false;
        }
    }

    private async void ForwardButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (_forwardHistory.Count == 0)
            return;

        var target = _forwardHistory.Pop();
        _backHistory.Push(CreateCurrentNavigationEntry());
        _suppressHistoryRecord = true;
        try
        {
            await RestoreNavigationEntryAsync(target).ConfigureAwait(true);
        }
        finally
        {
            _suppressHistoryRecord = false;
        }
    }

    private async void RefreshButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (_isSearchMode)
        {
            await RestartSearchWithProgressAsync().ConfigureAwait(true);
            return;
        }

        await NavigateListAsync(_currentPath, syncTree: true, recordHistory: false, forceReload: true)
            .ConfigureAwait(true);
    }

    private async void HomeButton_OnClick(object sender, RoutedEventArgs e)
    {
        var quickAccess = ExecutableFilePickerTreeHelper.TryGetQuickAccessRoot(TreeRoots);
        if (quickAccess is not null)
        {
            quickAccess.IsExpanded = true;
            ExecutableFilePickerTreeHelper.SelectTreeItem(FolderTreeView, quickAccess);
            ExecutableFilePickerTreeHelper.SelectTreeItem(FolderTreePaneView, quickAccess);
        }

        var path = quickAccess is not null
            ? ExecutableFilePickerTreeHelper.TryGetDefaultQuickAccessPath(quickAccess)
            : null;

        path ??= ExecutableFilePickerCatalog.GetInitialDirectory();
        await NavigateListAsync(path, syncTree: true).ConfigureAwait(true);
    }

    private async void PathBreadcrumb_OnItemClicked(BreadcrumbBar sender, BreadcrumbBarItemClickedEventArgs args)
    {
        if (args.Index < 0 || args.Index >= _breadcrumbTrail.Count)
            return;

        await NavigateListAsync(_breadcrumbTrail[args.Index].FullPath, syncTree: true).ConfigureAwait(true);
    }

    private void SearchBox_OnTextChanged(object sender, TextChangedEventArgs e)
    {
        if (_suppressSearchTextChanged)
            return;

        UpdateSearchGlyphVisibility();
        var query = SearchBox.Text.Trim();
        if (string.IsNullOrEmpty(query))
        {
            _searchDebounceTimer.Stop();
            _pendingSearchQuery = string.Empty;
            if (_isSearchMode)
                _ = CloseSearchPageAsync(recordForward: false);
            return;
        }

        _pendingSearchQuery = query;
        EnsureSearchModeEntered();
        _searchDebounceTimer.Stop();
        _searchDebounceTimer.Start();
    }

    private async void SearchDebounceTimer_OnTick(DispatcherQueueTimer sender, object args)
    {
        var query = _pendingSearchQuery.Trim();
        if (string.IsNullOrEmpty(query) || !_isSearchMode)
            return;

        _activeSearchQuery = query;
        await RunSearchAsync(query).ConfigureAwait(true);
    }

    private async void CloseSearchButton_OnClick(object sender, RoutedEventArgs e) =>
        await CloseSearchPageAsync().ConfigureAwait(true);

    private void FileListView_OnSelectionChanged(object sender, SelectionChangedEventArgs e) =>
        UpdatePrimaryEnabled();

    private async void FileListView_OnDoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
    {
        if (FileListView.SelectedItem is not ExecutableFilePickerItem item)
            return;

        e.Handled = true;

        if (item.IsFolder)
        {
            await NavigateListAsync(item.FullPath, syncTree: true).ConfigureAwait(true);
            return;
        }

        if (_dialogContext is null)
            return;

        _selected = item;
        _dialogContext.ClosePrimary();
    }

    /// <summary>
    /// true, если есть хотя бы один файл и все файлы — изображения (папки не учитываются).
    /// </summary>
    private static bool DirectoryFilesAreOnlyImages(IEnumerable<ExecutableFilePickerItem> items)
    {
        var hasFile = false;
        foreach (var item in items)
        {
            if (item.IsFolder)
                continue;

            hasFile = true;
            if (!ExecutableFilePickerCatalog.IsImageExtension(item.FullPath))
                return false;
        }

        return hasFile;
    }

    private enum FilePickerViewMode
    {
        List,
        Details,
        LargeIcons
    }

    private enum FilePickerSortColumn
    {
        Name,
        Type
    }
}
