using System.Diagnostics;
using System.Globalization;
using GlassSettings.Controls;
using GlassSettings.Controls.Processes;
using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Documents;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;
using Windows.Foundation;

namespace GlassSettings.Pages;

public sealed partial class ProcessesPage : Page
{
    private const double GridTileWidth = 200;
    private const double CompactTileWidth = 245;
    private const double TileGap = 8;
    private const int MaxTileColumns = 4;

    private enum ProcessFilter
    {
        All,
        Full,
        Popup,
        Forbidden,
        System
    }

    private enum ProcessSort
    {
        NameAsc,
        NameDesc,
        Mode
    }

    private readonly List<ProcessEntryModel> _allEntries = [];
    private readonly List<ProcessEntryModel> _blockListEntries = [];
    private bool _runtimeSubscribed;
    private string _runtimeDescriptionKey = string.Empty;
    private bool _crashesGateBusy;
    private CrashDumpFolderWatcher? _platformCrashDumpWatcher;
    private CrashDumpFolderWatcher? _systemCrashDumpWatcher;
    private IReadOnlyList<CrashDumpEntry> _platformCrashDumps = Array.Empty<CrashDumpEntry>();
    private IReadOnlyList<CrashDumpEntry> _systemCrashDumps = Array.Empty<CrashDumpEntry>();
    private Button? _unloadAllButton;
    private bool _unloadAllBusy;
    private ProcessViewMode _viewMode = ProcessViewMode.List;
    private ProcessFilter _filter = ProcessFilter.All;
    private ProcessSort _sort = ProcessSort.NameAsc;
    private string _searchQuery = string.Empty;
    private int _visibleItemCount;
    private int _visibleBlockListCount;
    private bool _suppressFilterEvents;
    private int _reloadGeneration;
    private ProcessesPageHeaderLayout? _headerLayout;
    private Expander? _processListRootExpander;
    private Expander? _blockListRootExpander;
    private bool _wasProcessListExpanded;
    private bool _wasBlockListExpanded;
    private enum SearchFocusKind { None, UserList, BlockList }
    private SearchFocusKind _searchFocus = SearchFocusKind.None;
    private Button? _addButton;
    private MenuFlyout? _addProcessMenu;
    private Button? _blockListAddButton;
    private MenuFlyout? _blockListAddMenu;
    private bool _layoutRetryScheduled;
    private double _lastProcessItemsHostWidth;
    private double _lastBlockListItemsHostWidth;
    private const int ExpanderAnimationMs = 260;

    public ProcessesPage()
    {
        NavigationCacheMode = NavigationCacheMode.Enabled;
        InitializeComponent();

        // Глушим BringIntoView на списке процессов — клик/двойной клик
        // не должны уводить PageScrollViewer к началу.
        ProcessItems.AddHandler(
            UIElement.BringIntoViewRequestedEvent,
            new TypedEventHandler<UIElement, BringIntoViewRequestedEventArgs>(
                (_, e) =>
                {
                    e.Handled = true;
                }),
            handledEventsToo: true);
        BlockListItems.AddHandler(
            UIElement.BringIntoViewRequestedEvent,
            new TypedEventHandler<UIElement, BringIntoViewRequestedEventArgs>(
                (_, e) =>
                {
                    e.Handled = true;
                }),
            handledEventsToo: true);

        // Подавляем Tapped на списке — клик не должен запускать цепочку фокуса
        // (иначе фокус уходит в SearchBox и страница прокручивается к началу).
        ProcessItems.Tapped += (_, e) =>
        {
            e.Handled = true;
        };
        BlockListItems.Tapped += (_, e) =>
        {
            e.Handled = true;
        };

        // Диагностика фокуса на странице (заглушка — без вывода)
        GotFocus += (_, e) => { };
        LostFocus += (_, e) => { };
        ProcessItems.GotFocus += (_, e) => { };
        BlockListItems.GotFocus += (_, e) => { };
        SearchBox.GotFocus += (_, e) => { };
        SearchBox.LostFocus += (_, e) => { };

        // Подавляем BringIntoView на SearchBox — при получении фокуса
        // (в т.ч. программном от root-окна) страница не должна прокручиваться к полю поиска.
        SearchBox.AddHandler(
            UIElement.BringIntoViewRequestedEvent,
            new TypedEventHandler<UIElement, BringIntoViewRequestedEventArgs>(
                (_, e) =>
                {
                    e.Handled = true;
                }),
            handledEventsToo: true);

        ApplyViewMode(ProcessViewMode.List);
        _headerLayout = new ProcessesPageHeaderLayout(
            HeaderGrid,
            SearchRowGrid,
            SearchBox,
            ViewSwitcher,
            FoundCountText,
            FilterGroup,
            SortGroup);
        ConfigureProcessListExpander();
        ConfigureBlockListExpander();
        ConfigureRuntimeExpander();
        ConfigureCrashesExpander();
        ApplyLocalizedStrings();
        Loaded += (_, _) =>
        {
            HookProcessListExpanderLayout();
            HookSearchTargetExpanders();
            ProcessItemsHost.SizeChanged += ProcessItemsHost_OnSizeChanged;
            BlockListItemsHost.SizeChanged += BlockListItemsHost_OnSizeChanged;
            UpdateHeaderLayout();
            UpdateSearchPlaceholder();
            ScheduleProcessItemsLayout();
            ScheduleItemsRemeasure();
            RefreshCrashDumps();
        };
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        CollapseAllExpanders();
        EnsureListLoaded();
        ApplyFilteredView();
        ApplyBlockListFilteredView();
        UpdateHeaderLayout();
        ScheduleProcessItemsLayout();
        ScheduleItemsRemeasure();
        StartRuntimeMonitor();
        StartCrashDumpWatcher();
        RefreshCrashDumps();
    }

    private void CollapseAllExpanders()
    {
        AdvancedExpander.IsExpanded = false;
        RuntimeExpander.IsExpanded = false;
        CrashesExpander.IsExpanded = false;
        ProcessListExpander.IsExpanded = false;
        BlockListExpander.IsExpanded = false;
    }

    protected override void OnNavigatedFrom(NavigationEventArgs e)
    {
        StopCrashDumpWatcher();
        StopRuntimeMonitor();
        base.OnNavigatedFrom(e);
    }

    private void ConfigureRuntimeExpander()
    {
        RuntimeExpander.IconGlyph = "\uE9D9";
        RuntimeExpander.IsExpanded = false;

        _unloadAllButton = new Button
        {
            MinHeight = 32,
            Padding = new Thickness(12, 4, 12, 4),
            Style = (Style)Application.Current.Resources["DefaultButtonStyle"],
            IsEnabled = false,
        };
        _unloadAllButton.Click += UnloadAllButton_OnClick;
        RuntimeExpander.HeaderActions = _unloadAllButton;
    }

    private void ConfigureCrashesExpander()
    {
        CrashesExpander.IconGlyph = "\uE7BA";
        CrashesExpander.IsExpanded = false;
        PlatformCrashesTable.OpenRequested += (_, entry) => OpenCrashDump(entry);
        PlatformCrashesTable.DeleteRequested += (_, entry) => DeleteCrashDump(entry);
        SystemCrashesTable.OpenRequested += (_, entry) => OpenCrashDump(entry);
        SystemCrashesTable.DeleteRequested += (_, entry) => DeleteCrashDump(entry);
    }

    private void StartCrashDumpWatcher()
    {
        if (_platformCrashDumpWatcher is null)
        {
            _platformCrashDumpWatcher = new CrashDumpFolderWatcher(DispatcherQueue);
            _platformCrashDumpWatcher.Changed += OnPlatformCrashDumpFolderChanged;
            _platformCrashDumpWatcher.Start(CrashDumpCatalog.ResolvePlatformFolder());
        }

        if (_systemCrashDumpWatcher is null)
        {
            _systemCrashDumpWatcher = new CrashDumpFolderWatcher(DispatcherQueue);
            _systemCrashDumpWatcher.Changed += OnSystemCrashDumpFolderChanged;
            _systemCrashDumpWatcher.Start(CrashDumpCatalog.ResolveSystemFolder());
        }
    }

    private void StopCrashDumpWatcher()
    {
        if (_platformCrashDumpWatcher is not null)
        {
            _platformCrashDumpWatcher.Changed -= OnPlatformCrashDumpFolderChanged;
            _platformCrashDumpWatcher.Dispose();
            _platformCrashDumpWatcher = null;
        }

        if (_systemCrashDumpWatcher is not null)
        {
            _systemCrashDumpWatcher.Changed -= OnSystemCrashDumpFolderChanged;
            _systemCrashDumpWatcher.Dispose();
            _systemCrashDumpWatcher = null;
        }
    }

    private void OnPlatformCrashDumpFolderChanged(object? sender, EventArgs e)
    {
        if (_crashesGateBusy)
            return;

        RefreshPlatformCrashDumps();
        ApplyCrashesDescription();
        ApplyActiveCrashesGateState();
    }

    private void OnSystemCrashDumpFolderChanged(object? sender, EventArgs e)
    {
        if (_crashesGateBusy)
            return;

        RefreshSystemCrashDumps();
        ApplyCrashesDescription();
        ApplyActiveCrashesGateState();
    }

    private void CrashesPivot_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_crashesGateBusy)
            return;

        ApplyActiveCrashesGateState();
    }

    private bool IsSystemCrashesTabSelected() =>
        CrashesPivot.SelectedIndex == 1;

    private IReadOnlyList<CrashDumpEntry> GetActiveCrashDumps() =>
        IsSystemCrashesTabSelected() ? _systemCrashDumps : _platformCrashDumps;

    /// <summary>Обновить оба списка, подстроку (сумма) и gate активной вкладки.</summary>
    private void RefreshCrashDumps()
    {
        RefreshPlatformCrashDumps();
        RefreshSystemCrashDumps();
        ApplyCrashesDescription();
        ApplyActiveCrashesGateState();
    }

    private void RefreshPlatformCrashDumps()
    {
        _platformCrashDumps = CrashDumpCatalog.Enumerate(CrashDumpCatalog.ResolvePlatformFolder());
        ApplyCrashDumpTable(
            PlatformCrashesTable,
            PlatformCrashesEmptyText,
            _platformCrashDumps);
    }

    private void RefreshSystemCrashDumps()
    {
        _systemCrashDumps = CrashDumpCatalog.Enumerate(CrashDumpCatalog.ResolveSystemFolder());
        ApplyCrashDumpTable(
            SystemCrashesTable,
            SystemCrashesEmptyText,
            _systemCrashDumps);
    }

    private static void ApplyCrashDumpTable(
        CrashDumpTable table,
        TextBlock emptyText,
        IReadOnlyList<CrashDumpEntry> dumps)
    {
        var hasRows = dumps.Count > 0;
        table.SetEntries(hasRows ? dumps : Array.Empty<CrashDumpEntry>());
        table.Visibility = hasRows ? Visibility.Visible : Visibility.Collapsed;
        emptyText.Visibility = hasRows ? Visibility.Collapsed : Visibility.Visible;
    }

    /// <summary>
    /// Есть дампы хотя бы в одной папке → gate ON и switch enabled.
    /// Обе пусты → gate OFF и switch disabled.
    /// </summary>
    private void ApplyActiveCrashesGateState()
    {
        var total = _platformCrashDumps.Count + _systemCrashDumps.Count;
        if (total == 0)
        {
            CrashesExpander.IsGateSwitchEnabled = false;
            CrashesExpander.IsGateOn = false;
            return;
        }

        CrashesExpander.IsGateSwitchEnabled = true;
        CrashesExpander.IsGateOn = true;
        // После wipe DP мог уже быть true при рассинхроне InnerSwitch — форсируем.
        CrashesExpander.GateSwitchControl.IsEnabled = true;
        CrashesExpander.GateSwitchControl.SyncInnerEnabled();
    }

    private void ApplyCrashesDescription()
    {
        var baseDesc = GlassSettingsResources.Get("Crashes_SectionDesc");
        // Общее число в обеих папках — не зависит от активной вкладки Pivot.
        var count = _platformCrashDumps.Count + _systemCrashDumps.Count;
        var suffix = count > 0
            ? GlassSettingsResources.Format("Crashes_CountFound", count)
            : GlassSettingsResources.Get("Crashes_CountNone");
        CrashesExpander.Description = $"{baseDesc} {suffix}";
    }

    private async void CrashesExpander_OnGateToggled(object sender, RoutedEventArgs e)
    {
        if (_crashesGateBusy)
            return;

        if (CrashesExpander.IsGateOn)
        {
            RefreshCrashDumps();
            return;
        }

        // Выключение gate — wipe *.dmp в обеих папках (платформа + система).
        _crashesGateBusy = true;
        CrashesExpander.IsGateSwitchEnabled = false;
        try
        {
            await Task.Run(() =>
            {
                CrashDumpCatalog.DeleteAll(CrashDumpCatalog.ResolvePlatformFolder());
                CrashDumpCatalog.DeleteAll(CrashDumpCatalog.ResolveSystemFolder());
            }).ConfigureAwait(true);
            RefreshCrashDumps();
            // Сворачивать только когда обе папки пусты.
            if (_platformCrashDumps.Count + _systemCrashDumps.Count == 0)
                CrashesExpander.IsExpanded = false;
        }
        finally
        {
            _crashesGateBusy = false;
            // Watcher-события во время busy пропускаются — догоняем список/gate.
            RefreshCrashDumps();
        }
    }

    private async void UnloadAllButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (_unloadAllBusy || _unloadAllButton is null)
            return;

        _unloadAllBusy = true;
        _unloadAllButton.IsEnabled = false;
        try
        {
            var snapshot = InjectRuntimeMonitor.Instance.Latest;
            var pids = snapshot.Entries
                .Where(static entry => entry.CanRequestUnload)
                .Select(static entry => entry.Pid)
                .Distinct()
                .ToArray();
            if (pids.Length == 0)
                return;

            // Подтверждение только если в списке больше одного процесса (не условие enable).
            if (snapshot.Entries.Count > 1)
            {
                var confirmed = await HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
                {
                    Title = GlassSettingsResources.Get("Runtime_UnloadAllConfirmTitle"),
                    Body = HostedConfirmationDialogService.CreateBody(
                        GlassSettingsResources.Get("Runtime_UnloadAllConfirmBody")),
                    Footer = HostedConfirmationDialogService.CreateCancelConfirmFooter("OK"),
                    MinWidth = 420,
                    MaxWidth = 480,
                }).ConfigureAwait(true);

                if (!confirmed)
                    return;
            }

            await Task.Run(() =>
            {
                foreach (var pid in pids)
                    InjectRuntimeMonitor.Instance.RequestProcessUnload(pid);
            }).ConfigureAwait(true);
        }
        finally
        {
            _unloadAllBusy = false;
            UpdateUnloadAllButton(InjectRuntimeMonitor.Instance.Latest);
        }
    }

    private void UpdateUnloadAllButton(InjectRuntimeSnapshot snapshot)
    {
        if (_unloadAllButton is null)
            return;

        if (_unloadAllBusy)
        {
            _unloadAllButton.IsEnabled = false;
            return;
        }

        var hasUnloadable = snapshot.Entries.Any(static e => e.CanRequestUnload);
        // Не ждать !PlatformRunning (mutex хоста живёт дольше выгрузки injectee).
        // Триггер как красные Residual / LifecycleBusy тумблеров Главной после stop.
        _unloadAllButton.IsEnabled = hasUnloadable && IsUnloadAllWindowOpen(snapshot);
    }

    /// <summary>
    /// Окно «платформа остановлена / выгружается»: Residual в списке или
    /// Stopping/Residual lifecycle (тот же SoT, что блокирует тумблеры Home).
    /// </summary>
    private static bool IsUnloadAllWindowOpen(InjectRuntimeSnapshot snapshot)
    {
        if (!snapshot.PlatformRunning || snapshot.HasResiduals)
            return true;

        var x64 = HomeStatusSummary.QueryGlassPlatformUi(GlassPlatformArch.X64);
        if (x64.LifecycleBusy || x64.IsUnloading)
            return true;

        var x86 = HomeStatusSummary.QueryGlassPlatformUi(GlassPlatformArch.X86);
        return x86.LifecycleBusy || x86.IsUnloading;
    }

    private void OpenCrashDump(CrashDumpEntry entry)
    {
        if (string.IsNullOrWhiteSpace(entry.FullPath) || !File.Exists(entry.FullPath))
            return;

        try
        {
            Process.Start(new ProcessStartInfo(entry.FullPath) { UseShellExecute = true });
        }
        catch (Exception)
        {
            // ассоциированное приложение недоступно
        }
    }

    private void DeleteCrashDump(CrashDumpEntry entry)
    {
        if (!CrashDumpCatalog.DeleteOne(entry.FullPath))
            return;

        RefreshCrashDumps();
    }

    private void StartRuntimeMonitor()
    {
        if (_runtimeSubscribed)
            return;
        _runtimeSubscribed = true;
        InjectRuntimeMonitor.Instance.SnapshotChanged += OnRuntimeSnapshotChanged;
        InjectRuntimeMonitor.Instance.Subscribe(this, DispatcherQueue);
        ApplyRuntimeSnapshot(InjectRuntimeMonitor.Instance.Latest);
    }

    private void StopRuntimeMonitor()
    {
        if (!_runtimeSubscribed)
            return;
        _runtimeSubscribed = false;
        InjectRuntimeMonitor.Instance.SnapshotChanged -= OnRuntimeSnapshotChanged;
        InjectRuntimeMonitor.Instance.Unsubscribe(this);
    }

    private void OnRuntimeSnapshotChanged(InjectRuntimeSnapshot snapshot) =>
        DispatcherQueue.TryEnqueue(() => ApplyRuntimeSnapshot(snapshot));

    private void ApplyRuntimeSnapshot(InjectRuntimeSnapshot snapshot)
    {
        var hasRows = snapshot.Entries.Count > 0;
        var tableVisibility = hasRows ? Visibility.Visible : Visibility.Collapsed;
        var emptyVisibility = hasRows ? Visibility.Collapsed : Visibility.Visible;
        if (RuntimeTable.Visibility != tableVisibility)
            RuntimeTable.Visibility = tableVisibility;
        if (RuntimeEmptyText.Visibility != emptyVisibility)
            RuntimeEmptyText.Visibility = emptyVisibility;

        // Точечный sync + якорь скролла — внутри InjectRuntimeModuleTable.
        RuntimeTable.SetEntries(hasRows ? snapshot.Entries : Array.Empty<InjectRuntimeEntry>());

        ApplyRuntimeDescription(snapshot);
        UpdateUnloadAllButton(snapshot);
    }

    private void ApplyRuntimeDescription(InjectRuntimeSnapshot snapshot)
    {
        var culture = CultureInfo.CurrentCulture;
        var activeLabel = GlassSettingsResources.Get("Home_RuntimeActive");
        var rejectedLabel = GlassSettingsResources.Get("Home_RuntimeRejected");
        var residualLabel = GlassSettingsResources.Get("Home_RuntimeResidual");
        var key = string.Format(
            culture,
            GlassSettingsResources.Get("Runtime_CountsFormat"),
            activeLabel,
            snapshot.ActiveCount,
            rejectedLabel,
            snapshot.RejectedCount,
            residualLabel,
            snapshot.ResidualCount);
        if (string.Equals(_runtimeDescriptionKey, key, StringComparison.Ordinal))
            return;

        _runtimeDescriptionKey = key;

        // ● перед каждой группой — цвета как у маячков таблицы.
        var inlines = new Inline[]
        {
            MakeBeaconRun(InjectRuntimeBeacon.Active),
            new Run { Text = $"{activeLabel}: {snapshot.ActiveCount.ToString(culture)}" },
            new Run { Text = " · " },
            MakeBeaconRun(InjectRuntimeBeacon.Rejected),
            new Run { Text = $"{rejectedLabel}: {snapshot.RejectedCount.ToString(culture)}" },
            new Run { Text = " · " },
            MakeBeaconRun(InjectRuntimeBeacon.Residual),
            new Run { Text = $"{residualLabel}: {snapshot.ResidualCount.ToString(culture)}" },
        };
        RuntimeExpander.SetDescriptionInlines(inlines);
    }

    private static Run MakeBeaconRun(InjectRuntimeBeacon beacon) =>
        new()
        {
            Text = "\u25CF ",
            Foreground = InjectRuntimeRowModel.BrushFor(beacon),
        };

    private void ProcessItemsHost_OnSizeChanged(object sender, SizeChangedEventArgs e)
    {
        UpdateProcessItemsLayout();

        if (e.PreviousSize.Width <= 0 && e.NewSize.Width > 0 && ProcessItems.ItemsSource is not null)
            ResetItemsViewBinding();
    }

    private void BlockListItemsHost_OnSizeChanged(object sender, SizeChangedEventArgs e)
    {
        UpdateBlockListItemsLayout();

        if (e.PreviousSize.Width <= 0 && e.NewSize.Width > 0 && BlockListItems.ItemsSource is not null)
            ResetBlockListItemsViewBinding();
    }

    private void EnsureListLoaded()
    {
        if (_allEntries.Count == 0)
        {
            if (ProcessIconResolver.TryClonePrebuiltEntries(out var prebuilt))
                _allEntries.AddRange(prebuilt);
            else
                _allEntries.AddRange(ProcessIconResolver.CreateAttachListModelsSafe());

            RefreshFilterComboItems();
            QueueMissingIconsLoad();
        }

        if (_blockListEntries.Count == 0)
        {
            _blockListEntries.AddRange(ProcessIconResolver.CreateBlockListModelsSafe());
            QueueMissingIconsLoad();
        }
    }

    private void ConfigureProcessListExpander()
    {
        ProcessListExpander.IconGlyph = "\uE77B";
        ProcessListExpander.IsExpanded = false;

        _addButton = new Button
        {
            MinHeight = 32,
            Padding = new Thickness(12, 4, 12, 4),
            Style = (Style)Application.Current.Resources["DefaultButtonStyle"]
        };
        _addProcessMenu = CreateAddMenu(
            () => AddFromRunningProcessListAsync(),
            () => AddFromFilePickerAsync(),
            () => AddFromManualInputAsync(),
            () => AddFromWindowPickAsync(ProcessPickerTarget.AttachList));
        _addButton.Flyout = _addProcessMenu;
        ProcessListExpander.HeaderActions = _addButton;
    }

    private void ConfigureBlockListExpander()
    {
        BlockListExpander.IconGlyph = "\uE72E";
        BlockListExpander.IsExpanded = false;

        _blockListAddButton = new Button
        {
            MinHeight = 32,
            Padding = new Thickness(12, 4, 12, 4),
            Style = (Style)Application.Current.Resources["DefaultButtonStyle"]
        };
        _blockListAddMenu = CreateAddMenu(
            () => AddFromRunningProcessListForBlockListAsync(),
            () => AddFromFilePickerForBlockListAsync(),
            () => AddFromManualInputForBlockListAsync(),
            () => AddFromWindowPickAsync(ProcessPickerTarget.BlockList));
        _blockListAddButton.Flyout = _blockListAddMenu;
        BlockListExpander.HeaderActions = _blockListAddButton;
    }

    private MenuFlyout CreateAddMenu(
        Func<Task> fromRunningList,
        Func<Task> pickFile,
        Func<Task> enterManually,
        Func<Task> pickWindow)
    {
        var flyout = new MenuFlyout();

        var fromList = new MenuFlyoutItem
        {
            Text = GlassSettingsResources.Get("Attach_AddFromRunningList"),
            Icon = new FontIcon { Glyph = "\uE9F5" }
        };
        fromList.Click += async (_, _) => await fromRunningList().ConfigureAwait(true);
        flyout.Items.Add(fromList);

        var pickFileItem = new MenuFlyoutItem
        {
            Text = GlassSettingsResources.Get("Attach_AddChooseFile"),
            Icon = new FontIcon { Glyph = "\uE8E5" }
        };
        pickFileItem.Click += async (_, _) => await pickFile().ConfigureAwait(true);
        flyout.Items.Add(pickFileItem);

        var enterManuallyItem = new MenuFlyoutItem
        {
            Text = GlassSettingsResources.Get("Attach_AddEnterString"),
            Icon = new FontIcon { Glyph = "\uE8AC" }
        };
        enterManuallyItem.Click += async (_, _) => await enterManually().ConfigureAwait(true);
        flyout.Items.Add(enterManuallyItem);

        var pickWindowItem = new MenuFlyoutItem
        {
            Text = GlassSettingsResources.Get("Attach_AddPickWindow"),
            Icon = new FontIcon { Glyph = "\uE7C4" }
        };
        pickWindowItem.Click += async (_, _) => await pickWindow().ConfigureAwait(true);
        flyout.Items.Add(pickWindowItem);

        return flyout;
    }

    private void HookProcessListExpanderLayout()
    {
        if (_processListRootExpander is not null)
            return;

        _processListRootExpander = FindDescendant<Expander>(ProcessListExpander);
        if (_processListRootExpander is null)
            return;

        _processListRootExpander.Expanding += (_, _) => RefreshProcessItemsLayoutAfterExpand();
        _processListRootExpander.RegisterPropertyChangedCallback(
            Expander.IsExpandedProperty,
            (_, _) =>
            {
                if (_processListRootExpander?.IsExpanded == true)
                    RefreshItemsViewAfterExpanderOpened();
            });
    }

    private void HookSearchTargetExpanders()
    {
        _processListRootExpander ??= FindDescendant<Expander>(ProcessListExpander);
        _blockListRootExpander ??= FindDescendant<Expander>(BlockListExpander);

        if (_processListRootExpander is not null)
        {
            _processListRootExpander.RegisterPropertyChangedCallback(
                Expander.IsExpandedProperty,
                (_, _) => OnSearchTargetExpanderChanged());
        }

        if (_blockListRootExpander is not null)
        {
            _blockListRootExpander.RegisterPropertyChangedCallback(
                Expander.IsExpandedProperty,
                (_, _) => OnSearchTargetExpanderChanged());
        }

        UpdateSearchPlaceholder();
    }

    private void OnSearchTargetExpanderChanged()
    {
        var userOpen = ProcessListExpander.IsExpanded;
        var blockedOpen = BlockListExpander.IsExpanded;

        if (userOpen && !_wasProcessListExpanded)
            _searchFocus = SearchFocusKind.UserList;
        else if (blockedOpen && !_wasBlockListExpanded)
            _searchFocus = SearchFocusKind.BlockList;
        else if (!userOpen && !blockedOpen)
            _searchFocus = SearchFocusKind.None;
        else if (_searchFocus == SearchFocusKind.UserList && !userOpen)
            _searchFocus = blockedOpen ? SearchFocusKind.BlockList : SearchFocusKind.None;
        else if (_searchFocus == SearchFocusKind.BlockList && !blockedOpen)
            _searchFocus = userOpen ? SearchFocusKind.UserList : SearchFocusKind.None;

        _wasProcessListExpanded = userOpen;
        _wasBlockListExpanded = blockedOpen;

        UpdateSearchPlaceholder();
        ApplyFilteredView();
    }

    private void UpdateSearchPlaceholder()
    {
        SearchBox.PlaceholderText = _searchFocus switch
        {
            SearchFocusKind.UserList => GlassSettingsResources.Get("Attach_SearchUserPlaceholder"),
            SearchFocusKind.BlockList => GlassSettingsResources.Get("Attach_SearchBlockedPlaceholder"),
            _ => GlassSettingsResources.Get("Attach_SearchPlaceholder"),
        };
    }

    private void AdvancedExpander_OnExpanded(object sender, RoutedEventArgs e)
    {
        if (!AdvancedExpander.IsExpanded)
            return;

        AdvancedExpander.RefreshDividerChrome();
    }

    private void HeaderHost_OnSizeChanged(object sender, SizeChangedEventArgs e) =>
        UpdateHeaderLayout();

    private void UpdateHeaderLayout()
    {
        var width = HeaderHost.ActualWidth;
        if (width <= 0)
            return;

        _headerLayout?.Update(width);
    }

    private void ApplyLocalizedStrings()
    {
        SearchBox.PlaceholderText = GlassSettingsResources.Get("Attach_SearchPlaceholder");
        UpdateSearchPlaceholder();
        ViewSwitcher.ApplyLocalizedStrings();
        FilterLabel.Text = GlassSettingsResources.Get("Attach_FilterByLabel");
        SortLabel.Text = GlassSettingsResources.Get("Attach_SortByLabel");
        _addButton!.Content = GlassSettingsResources.Get("Attach_AddProcess");
        _blockListAddButton!.Content = GlassSettingsResources.Get("Attach_AddProcess");
        ProcessListExpander.Title = GlassSettingsResources.Get("Attach_ListSection");
        ProcessListExpander.Description = GlassSettingsResources.Get("Attach_ListSectionDesc");
        AdvancedExpander.Title = GlassSettingsResources.Get("Dwm_AdvancedColorizationTitle");
        RuntimeExpander.Title = GlassSettingsResources.Get("Runtime_Section");
        if (_unloadAllButton is not null)
            _unloadAllButton.Content = GlassSettingsResources.Get("Runtime_UnloadAll");
        _runtimeDescriptionKey = string.Empty;
        ApplyRuntimeDescription(InjectRuntimeMonitor.Instance.Latest);
        UpdateUnloadAllButton(InjectRuntimeMonitor.Instance.Latest);
        RuntimeEmptyText.Text = GlassSettingsResources.Get("Runtime_Empty");
        CrashesExpander.Title = GlassSettingsResources.Get("Crashes_Section");
        PlatformCrashesPivotItem.Header = GlassSettingsResources.Get("Crashes_TabPlatform");
        SystemCrashesPivotItem.Header = GlassSettingsResources.Get("Crashes_TabSystem");
        PlatformCrashesEmptyText.Text = GlassSettingsResources.Get("Crashes_Empty");
        SystemCrashesEmptyText.Text = GlassSettingsResources.Get("Crashes_Empty");
        PlatformCrashesTable.ApplyLocalizedStrings();
        SystemCrashesTable.ApplyLocalizedStrings();
        ApplyCrashesDescription();
        BlockListExpander.Title = GlassSettingsResources.Get("BlockList_Section");
        BlockListExpander.Description = GlassSettingsResources.Get("BlockList_SectionDesc");
        EmptyText.Text = GlassSettingsResources.Get("Attach_EmptyList");
        BlockListEmptyText.Text = GlassSettingsResources.Get("BlockList_EmptyList");
        FilterCombo.Items.Clear();
        FilterCombo.Items.Add(CreateFilterItem(ProcessFilter.All, "Attach_FilterAll"));
        FilterCombo.Items.Add(CreateFilterItem(ProcessFilter.Full, "Attach_ModeFull"));
        FilterCombo.Items.Add(CreateFilterItem(ProcessFilter.Popup, "Attach_ModePopup"));
        FilterCombo.Items.Add(CreateFilterItem(ProcessFilter.Forbidden, "Attach_FilterForbidden"));
        FilterCombo.Items.Add(CreateFilterItem(ProcessFilter.System, "Attach_FilterSystem"));

        SortCombo.Items.Clear();
        SortCombo.Items.Add(CreateSortItem(ProcessSort.NameAsc, "Attach_SortNameAsc"));
        SortCombo.Items.Add(CreateSortItem(ProcessSort.NameDesc, "Attach_SortNameDesc"));
        SortCombo.Items.Add(CreateSortItem(ProcessSort.Mode, "Attach_SortMode"));

        _suppressFilterEvents = true;
        try
        {
            FilterCombo.SelectedIndex = 0;
            SortCombo.SelectedIndex = 0;
        }
        finally
        {
            _suppressFilterEvents = false;
        }
    }

    private static ComboBoxItem CreateFilterItem(ProcessFilter filter, string resourceKey) =>
        new() { Content = GlassSettingsResources.Get(resourceKey), Tag = filter };

    private static ComboBoxItem CreateSortItem(ProcessSort sort, string resourceKey) =>
        new() { Content = GlassSettingsResources.Get(resourceKey), Tag = sort };

    private void RefreshFilterComboItems()
    {
        var selectedTag = (FilterCombo.SelectedItem as ComboBoxItem)?.Tag;
        var selectedIndex = FilterCombo.SelectedIndex;

        _suppressFilterEvents = true;
        try
        {
            FilterCombo.Items.Clear();
            FilterCombo.Items.Add(CreateFilterItem(ProcessFilter.All, "Attach_FilterAll"));
            FilterCombo.Items.Add(CreateFilterItem(ProcessFilter.Full, "Attach_ModeFull"));
            FilterCombo.Items.Add(CreateFilterItem(ProcessFilter.Popup, "Attach_ModePopup"));
            FilterCombo.Items.Add(CreateFilterItem(ProcessFilter.Forbidden, "Attach_FilterForbidden"));
            FilterCombo.Items.Add(CreateFilterItem(ProcessFilter.System, "Attach_FilterSystem"));

            var restored = false;
            for (var i = 0; i < FilterCombo.Items.Count; i++)
            {
                if (FilterCombo.Items[i] is not ComboBoxItem item)
                    continue;

                if (!Equals(item.Tag, selectedTag))
                    continue;

                FilterCombo.SelectedIndex = i;
                restored = true;
                break;
            }

            if (!restored && selectedIndex >= 0 && selectedIndex < FilterCombo.Items.Count)
                FilterCombo.SelectedIndex = selectedIndex;
            else if (!restored)
                FilterCombo.SelectedIndex = 0;
        }
        finally
        {
            _suppressFilterEvents = false;
        }

        if (FilterCombo.SelectedItem is ComboBoxItem { Tag: ProcessFilter filter })
            _filter = filter;
    }

    private void ReloadFromRegistry()
    {
        var generation = Interlocked.Increment(ref _reloadGeneration);
        var models = ProcessIconResolver.CreateAttachListModels();

        if (generation != _reloadGeneration)
            return;

        _allEntries.Clear();
        _allEntries.AddRange(models);
        RefreshFilterComboItems();
        ApplyFilteredView();
        QueueMissingIconsLoad();
    }

    private void ReloadBlockListFromRegistry()
    {
        var models = ProcessIconResolver.CreateBlockListModels();
        _blockListEntries.Clear();
        _blockListEntries.AddRange(models);
        ApplyBlockListFilteredView();
        QueueMissingIconsLoad();
    }

    private void QueueMissingIconsLoad()
    {
        if (ProcessIconResolver.IsPreloadComplete
            && !_allEntries.Any(static e => e.Icon is null)
            && !_blockListEntries.Any(static e => e.Icon is null))
            return;

        if (!_allEntries.Any(static e => e.Icon is null)
            && !_blockListEntries.Any(static e => e.Icon is null))
            return;

        _ = LoadMissingIconsAsync();
    }

    private async Task LoadMissingIconsAsync()
    {
        try
        {
            var missing = _allEntries.Where(static e => e.Icon is null).Select(static e => e.ExeName)
                .Concat(_blockListEntries.Where(static e => e.Icon is null).Select(static e => e.ExeName))
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .ToList();
            if (missing.Count == 0)
                return;

            var queue = DispatcherQueue;
            await ProcessIconResolver.EnsureIconsAsync(missing).ConfigureAwait(true);

            foreach (var entry in _allEntries.Concat(_blockListEntries))
            {
                if (entry.Icon is not null)
                    continue;

                entry.Icon = ProcessIconResolver.TryGetCachedIcon(entry.ExeName)
                             ?? await ProcessIconResolver.GetIconAsync(entry.ExeName, queue).ConfigureAwait(true)
                             ?? ProcessIconResolver.TryGetDefaultFileTypeIcon(entry.ExeName);
            }
        }
        catch
        {
            // догрузка иконок не критична для страницы
        }
    }

    private void ApplyFilteredView()
    {
        IEnumerable<ProcessEntryModel> query = _allEntries;

        // Поиск только в развёрнутом спойлере пользовательского списка (не Runtime).
        if (ProcessListExpander.IsExpanded && !string.IsNullOrWhiteSpace(_searchQuery))
        {
            query = query.Where(e =>
                e.DisplayName.Contains(_searchQuery, StringComparison.OrdinalIgnoreCase) ||
                e.ExeName.Contains(_searchQuery, StringComparison.OrdinalIgnoreCase) ||
                (e.CompanyName?.Contains(_searchQuery, StringComparison.OrdinalIgnoreCase) ?? false) ||
                (e.FileDescription?.Contains(_searchQuery, StringComparison.OrdinalIgnoreCase) ?? false));
        }

        query = _filter switch
        {
            ProcessFilter.Full => query.Where(e => e.Mode == AttachProcessMode.Full),
            ProcessFilter.Popup => query.Where(e => e.Mode == AttachProcessMode.PopupOnly),
            ProcessFilter.Forbidden => query.Where(e => e.Mode == AttachProcessMode.Forbidden),
            ProcessFilter.System => query.Where(static e =>
                ProcessEntryModel.IsMicrosoftPublisher(e.CompanyName, e.ResolvedPath)),
            _ => query
        };

        query = _sort switch
        {
            ProcessSort.NameDesc => query.OrderByDescending(e => e.DisplayName, StringComparer.OrdinalIgnoreCase),
            ProcessSort.Mode => query.OrderBy(e => e.Mode).ThenBy(e => e.DisplayName, StringComparer.OrdinalIgnoreCase),
            _ => query.OrderBy(e => e.DisplayName, StringComparer.OrdinalIgnoreCase)
        };

        var list = query.ToList();
        _visibleItemCount = list.Count;
        ProcessItems.ItemsSource = list;
        FoundCountText.Text = GlassSettingsResources.Format("Attach_FoundCount", list.Count);
        EmptyText.Visibility = list.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        ProcessItems.Visibility = list.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
        UpdateProcessItemsLayout();
        ScheduleItemsRemeasure();
        ApplyBlockListFilteredView();
    }

    private void ApplyBlockListFilteredView()
    {
        IEnumerable<ProcessEntryModel> query = _blockListEntries;

        // Поиск только в развёрнутом спойлере запрещённых (не Runtime).
        if (BlockListExpander.IsExpanded && !string.IsNullOrWhiteSpace(_searchQuery))
        {
            query = query.Where(e =>
                e.DisplayName.Contains(_searchQuery, StringComparison.OrdinalIgnoreCase) ||
                e.ExeName.Contains(_searchQuery, StringComparison.OrdinalIgnoreCase) ||
                (e.CompanyName?.Contains(_searchQuery, StringComparison.OrdinalIgnoreCase) ?? false) ||
                (e.FileDescription?.Contains(_searchQuery, StringComparison.OrdinalIgnoreCase) ?? false));
        }

        query = _sort switch
        {
            ProcessSort.NameDesc => query.OrderByDescending(e => e.DisplayName, StringComparer.OrdinalIgnoreCase),
            _ => query.OrderBy(e => e.DisplayName, StringComparer.OrdinalIgnoreCase)
        };

        var list = query.ToList();
        _visibleBlockListCount = list.Count;
        BlockListItems.ItemsSource = list;
        BlockListEmptyText.Visibility = list.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        BlockListItems.Visibility = list.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
        UpdateBlockListItemsLayout();
    }

    private void UpdateProcessItemsLayout()
    {
        if (_visibleItemCount <= 0)
            return;

        var hostWidth = ProcessItemsHost.ActualWidth;
        var hostBecameReady = _lastProcessItemsHostWidth <= 0 && hostWidth > 0;
        _lastProcessItemsHostWidth = hostWidth;

        if (hostWidth <= 0)
        {
            ProcessItems.ClearValue(FrameworkElement.WidthProperty);
            ProcessItems.HorizontalAlignment = HorizontalAlignment.Stretch;
            ScheduleProcessItemsLayout();
            return;
        }

        if (_viewMode == ProcessViewMode.List)
        {
            ProcessItems.Width = hostWidth;
            ProcessItems.HorizontalAlignment = HorizontalAlignment.Stretch;
            if (hostBecameReady && ProcessItems.ItemsSource is not null)
                ResetItemsViewBinding();
            return;
        }

        var tileWidth = _viewMode == ProcessViewMode.Tiles ? GridTileWidth : CompactTileWidth;
        var tileUnit = tileWidth + TileGap;
        var maxColumns = Math.Clamp((int)((hostWidth + TileGap) / tileUnit), 1, MaxTileColumns);
        var columns = Math.Min(_visibleItemCount, maxColumns);
        var gridWidth = columns * tileWidth + Math.Max(0, columns - 1) * TileGap;

        if (ProcessItems.Layout is UniformGridLayout layout)
            layout.MaximumRowsOrColumns = columns;

        ProcessItems.Width = gridWidth;
        ProcessItems.HorizontalAlignment = HorizontalAlignment.Center;

        if (hostBecameReady && ProcessItems.ItemsSource is not null)
            ResetItemsViewBinding();
    }

    private void UpdateBlockListItemsLayout()
    {
        if (_visibleBlockListCount <= 0)
            return;

        var hostWidth = BlockListItemsHost.ActualWidth;
        var hostBecameReady = _lastBlockListItemsHostWidth <= 0 && hostWidth > 0;
        _lastBlockListItemsHostWidth = hostWidth;

        if (hostWidth <= 0)
        {
            BlockListItems.ClearValue(FrameworkElement.WidthProperty);
            BlockListItems.HorizontalAlignment = HorizontalAlignment.Stretch;
            ScheduleProcessItemsLayout();
            return;
        }

        if (_viewMode == ProcessViewMode.List)
        {
            BlockListItems.Width = hostWidth;
            BlockListItems.HorizontalAlignment = HorizontalAlignment.Stretch;
            if (hostBecameReady && BlockListItems.ItemsSource is not null)
                ResetBlockListItemsViewBinding();
            return;
        }

        var tileWidth = _viewMode == ProcessViewMode.Tiles ? GridTileWidth : CompactTileWidth;
        var tileUnit = tileWidth + TileGap;
        var maxColumns = Math.Clamp((int)((hostWidth + TileGap) / tileUnit), 1, MaxTileColumns);
        var columns = Math.Min(_visibleBlockListCount, maxColumns);
        var gridWidth = columns * tileWidth + Math.Max(0, columns - 1) * TileGap;

        if (BlockListItems.Layout is UniformGridLayout layout)
            layout.MaximumRowsOrColumns = columns;

        BlockListItems.Width = gridWidth;
        BlockListItems.HorizontalAlignment = HorizontalAlignment.Center;

        if (hostBecameReady && BlockListItems.ItemsSource is not null)
            ResetBlockListItemsViewBinding();
    }

    private void ScheduleProcessItemsLayout()
    {
        if (_layoutRetryScheduled)
            return;

        _layoutRetryScheduled = true;
        DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, () =>
        {
            _layoutRetryScheduled = false;
            UpdateProcessItemsLayout();
            UpdateBlockListItemsLayout();
        });
    }

    private void RefreshProcessItemsLayout()
    {
        if (ProcessItems.Visibility != Visibility.Visible)
            return;

        ProcessItems.InvalidateMeasure();
        ProcessItems.UpdateLayout();
    }

    private void RefreshProcessItemsLayoutAfterExpand()
    {
        ScheduleProcessItemsLayout();
        RefreshProcessItemsLayout();
    }

    private void RefreshItemsViewAfterExpanderOpened()
    {
        ScheduleProcessItemsLayout();
        _ = RefreshItemsViewAfterExpanderOpenedAsync();
    }

    private async Task RefreshItemsViewAfterExpanderOpenedAsync()
    {
        await Task.Delay(ExpanderAnimationMs).ConfigureAwait(true);

        if (ProcessItems.Visibility != Visibility.Visible)
            return;

        UpdateProcessItemsLayout();
        ResetItemsViewBinding();
        ProcessItems.InvalidateMeasure();
        ProcessItems.UpdateLayout();

        await Task.Delay(1).ConfigureAwait(true);
        UpdateProcessItemsLayout();
        ProcessItems.UpdateLayout();
    }

    private void ResetItemsViewBinding()
    {
        var source = ProcessItems.ItemsSource;
        if (source is null)
            return;

        ProcessItems.ItemsSource = null;
        ProcessItems.ItemsSource = source;
    }

    private void ResetBlockListItemsViewBinding()
    {
        var source = BlockListItems.ItemsSource;
        if (source is null)
            return;

        BlockListItems.ItemsSource = null;
        BlockListItems.ItemsSource = source;
    }

    private void ScheduleItemsRemeasure()
    {
        _ = RemeasureItemsAfterLayoutAsync();
    }

    private async Task RemeasureItemsAfterLayoutAsync()
    {
        await Task.Delay(1).ConfigureAwait(true);

        if (ProcessItems.Visibility != Visibility.Visible)
            return;

        UpdateProcessItemsLayout();
        ResetItemsViewBinding();
        ProcessItems.UpdateLayout();

        if (ProcessItemsHost.ActualWidth > 0)
            return;

        await Task.Delay(ExpanderAnimationMs).ConfigureAwait(true);

        if (ProcessItems.Visibility != Visibility.Visible)
            return;

        UpdateProcessItemsLayout();
        ResetItemsViewBinding();
        ProcessItems.UpdateLayout();
    }

    private void ApplyViewMode(ProcessViewMode mode)
    {
        var modeChanged = _viewMode != mode;
        _viewMode = mode;
        ProcessItems.ItemTemplate = mode switch
        {
            ProcessViewMode.Tiles => (DataTemplate)Resources["ProcessGridTemplate"],
            ProcessViewMode.Compact => (DataTemplate)Resources["ProcessCompactTemplate"],
            _ => (DataTemplate)Resources["ProcessListTemplate"]
        };

        ProcessItems.Layout = mode switch
        {
            ProcessViewMode.Tiles => new UniformGridLayout
            {
                MaximumRowsOrColumns = MaxTileColumns,
                MinColumnSpacing = TileGap,
                MinRowSpacing = TileGap,
                MinItemWidth = (float)GridTileWidth,
                MinItemHeight = (float)GridTileWidth
            },
            ProcessViewMode.Compact => new UniformGridLayout
            {
                MaximumRowsOrColumns = MaxTileColumns,
                MinColumnSpacing = TileGap,
                MinRowSpacing = TileGap,
                MinItemWidth = (float)CompactTileWidth,
                MinItemHeight = 70
            },
            _ => new StackLayout { Orientation = Orientation.Vertical, Spacing = TileGap }
        };

        BlockListItems.ItemTemplate = mode switch
        {
            ProcessViewMode.Tiles => (DataTemplate)Resources["BlockListGridTemplate"],
            ProcessViewMode.Compact => (DataTemplate)Resources["BlockListCompactTemplate"],
            _ => (DataTemplate)Resources["BlockListListTemplate"]
        };

        BlockListItems.Layout = mode switch
        {
            ProcessViewMode.Tiles => new UniformGridLayout
            {
                MaximumRowsOrColumns = MaxTileColumns,
                MinColumnSpacing = TileGap,
                MinRowSpacing = TileGap,
                MinItemWidth = (float)GridTileWidth,
                MinItemHeight = (float)GridTileWidth
            },
            ProcessViewMode.Compact => new UniformGridLayout
            {
                MaximumRowsOrColumns = MaxTileColumns,
                MinColumnSpacing = TileGap,
                MinRowSpacing = TileGap,
                MinItemWidth = (float)CompactTileWidth,
                MinItemHeight = 70
            },
            _ => new StackLayout { Orientation = Orientation.Vertical, Spacing = TileGap }
        };

        UpdateProcessItemsLayout();
        UpdateBlockListItemsLayout();

        if (modeChanged && ProcessItems.ItemsSource is not null)
            ResetItemsViewBinding();

        if (modeChanged && BlockListItems.ItemsSource is not null)
            ResetBlockListItemsViewBinding();
    }

    private void SearchBox_OnTextChanged(AutoSuggestBox sender, AutoSuggestBoxTextChangedEventArgs args)
    {
        if (args.Reason != AutoSuggestionBoxTextChangeReason.UserInput)
            return;

        _searchQuery = sender.Text?.Trim() ?? string.Empty;
        ApplyFilteredView();
    }

    private void FilterCombo_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_suppressFilterEvents)
            return;

        if (FilterCombo.SelectedItem is ComboBoxItem { Tag: ProcessFilter filter })
            _filter = filter;

        ApplyFilteredView();
    }

    private void SortCombo_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_suppressFilterEvents)
            return;

        if (SortCombo.SelectedItem is ComboBoxItem { Tag: ProcessSort sort })
            _sort = sort;
        ApplyFilteredView();
    }

    private void ViewSwitcher_OnViewModeChanged(object sender, ProcessViewMode mode) =>
        ApplyViewMode(mode);

    private void ProcessTile_OnActionRequested(object sender, ProcessActionRequestedEventArgs e)
    {
        switch (e.Action)
        {
            case ProcessMenuAction.SetFull:
                SetProcessMode(e.Entry, AttachProcessMode.Full);
                break;
            case ProcessMenuAction.SetPopup:
                SetProcessMode(e.Entry, AttachProcessMode.PopupOnly);
                break;
            case ProcessMenuAction.SetForbidden:
                SetProcessMode(e.Entry, AttachProcessMode.Forbidden);
                break;
            case ProcessMenuAction.Delete:
                RemoveProcess(e.Entry);
                break;
            case ProcessMenuAction.DeleteAndBlock:
                RemoveProcessAndBlock(e.Entry);
                break;
        }
    }

    private void BlockListTile_OnActionRequested(object sender, ProcessActionRequestedEventArgs e)
    {
        if (e.Action == ProcessMenuAction.RemoveFromBlockList)
            RemoveFromBlockList(e.Entry);
    }

    private void ShowActionSuccess() =>
        _ = ShowActionNoticeAsync(SettingsNoticeSeverity.Success, GlassSettingsResources.Get("Attach_ActionSuccess"));

    private void ShowActionError(string resourceKey) =>
        _ = ShowActionNoticeAsync(SettingsNoticeSeverity.Error, GlassSettingsResources.Get(resourceKey));

    private Task ShowActionNoticeAsync(SettingsNoticeSeverity severity, string message)
    {
        var host = SettingsNoticeCoordinator.CreateLegacyHost(
            this,
            PageShell.PageScrollViewer,
            ActionNoticeViewportAnchor);
        return SettingsNoticeCoordinator.PresentAsync(host, new SettingsNoticeCoordinator.NoticePresentRequest
        {
            Strip = ActionNoticeStrip,
            Severity = severity,
            Message = message,
            ShowSave = false,
            ShowAction = ActionNoticeStrip.ShowActionButton,
            ActionButtonText = ActionNoticeStrip.ActionButtonText,
            Animate = true
        });
    }

    private SettingsNoticeCoordinator.LegacyNoticePageHost CreateActionNoticeHost() =>
        SettingsNoticeCoordinator.CreateLegacyHost(
            this,
            PageShell.PageScrollViewer,
            ActionNoticeViewportAnchor);

    private Task DismissActionNoticeStripAsync() =>
        SettingsNoticeCoordinator.CloseStripAsync(CreateActionNoticeHost(), ActionNoticeStrip, animate: false);

    private void SetProcessMode(ProcessEntryModel entry, AttachProcessMode mode)
    {
        SettingsSession.Current.WriteGlassDword("AttachList", entry.ExeName, (uint)mode);
        entry.SetMode(mode);
        AttachListLiveReload.NotifyAttachListChanged();
        ShowActionSuccess();
        ApplyFilteredView();
    }

    private void RemoveProcess(ProcessEntryModel entry)
    {
        SettingsSession.Current.DeleteGlassValue("AttachList", entry.ExeName);
        ProcessIconResolver.Invalidate(entry.ExeName);
        _allEntries.Remove(entry);
        AttachListLiveReload.NotifyAttachListChanged();
        RefreshFilterComboItems();
        ShowActionSuccess();
        ApplyFilteredView();
    }

    private void RemoveProcessAndBlock(ProcessEntryModel entry)
    {
        SettingsSession.Current.DeleteGlassValue("AttachList", entry.ExeName);
        BlockListRegistry.Add(entry.ExeName);
        ProcessIconResolver.Invalidate(entry.ExeName);
        _allEntries.Remove(entry);
        AttachListLiveReload.NotifyAttachListChanged();
        RefreshFilterComboItems();
        ShowActionSuccess();
        ApplyFilteredView();
        ReloadBlockListFromRegistry();
    }

    private void RemoveFromBlockList(ProcessEntryModel entry)
    {
        if (!BlockListRegistry.CanRemove(entry.ExeName))
            return;

        BlockListRegistry.Remove(entry.ExeName);
        ProcessIconResolver.Invalidate(entry.ExeName);
        AttachListLiveReload.NotifyAttachListChanged();
        ShowActionSuccess();
        ReloadBlockListFromRegistry();
    }

    private async Task AddFromRunningProcessListForBlockListAsync()
    {
        try
        {
            var exeName = await RunningProcessPickerDialogService.PickBlockListAsync(DispatcherQueue)
                .ConfigureAwait(true);
            if (exeName is null)
                return;

            TryAddToBlockList(exeName);
        }
        catch
        {
            // диалог отменён или недоступен
        }
    }

    private async Task AddFromFilePickerForBlockListAsync()
    {
        await Task.Delay(100).ConfigureAwait(true);

        try
        {
            var exeName = await ExecutableFilePickerDialogService.PickBlockListAsync(DispatcherQueue)
                .ConfigureAwait(true);
            if (exeName is null)
                return;

            TryAddToBlockList(exeName);
        }
        catch
        {
            // picker недоступен
        }
    }

    private async Task AddFromManualInputAsync()
    {
        await Task.Delay(100).ConfigureAwait(true);

        try
        {
            var exeName = await ProcessNameInputDialogService.PickAttachListAsync(
                    DispatcherQueue,
                    retainOverlayAfterSelection: true)
                .ConfigureAwait(true);
            if (exeName is null)
                return;

            await CompleteAttachProcessSelectionAsync(exeName, overlayAlreadyUp: true).ConfigureAwait(true);
        }
        catch
        {
            // диалог отменён или недоступен
        }
    }

    private async Task AddFromManualInputForBlockListAsync()
    {
        await Task.Delay(100).ConfigureAwait(true);

        try
        {
            var exeName = await ProcessNameInputDialogService.PickBlockListAsync(DispatcherQueue)
                .ConfigureAwait(true);
            if (exeName is null)
                return;

            TryAddToBlockList(exeName);
        }
        catch
        {
            // диалог отменён или недоступен
        }
    }

    private async Task AddFromRunningProcessListAsync()
    {
        try
        {
            var exeName = await RunningProcessPickerDialogService.PickAttachListAsync(
                    DispatcherQueue,
                    retainOverlayAfterSelection: true)
                .ConfigureAwait(true);
            if (exeName is null)
                return;

            await CompleteAttachProcessSelectionAsync(exeName, overlayAlreadyUp: true).ConfigureAwait(true);
        }
        catch
        {
            // диалог отменён или недоступен
        }
    }

    private async Task AddFromWindowPickAsync(ProcessPickerTarget target)
    {
        // MenuFlyout должен закрыться до смены курсора / overlay.
        await Task.Delay(100).ConfigureAwait(true);

        string? exeName;
        try
        {
            exeName = await WindowProcessPicker.PickExeNameAsync(DispatcherQueue).ConfigureAwait(true);
        }
        catch
        {
            return;
        }

        if (string.IsNullOrWhiteSpace(exeName))
            return;

        if (target == ProcessPickerTarget.BlockList)
        {
            exeName = BlockListRegistry.NormalizeExeName(exeName);
            if (BlockListRegistry.IsBlocked(exeName))
            {
                await ShowWindowPickDuplicateDialogAsync(
                        "BlockList_WindowPickDuplicateTitle",
                        "BlockList_WindowPickDuplicateBody",
                        exeName)
                    .ConfigureAwait(true);
                return;
            }

            if (!await ConfirmWindowPickAddAsync(
                        "BlockList_WindowPickConfirmTitle",
                        "BlockList_WindowPickConfirmBody",
                        exeName)
                    .ConfigureAwait(true))
                return;

            TryAddToBlockList(exeName);
            return;
        }

        exeName = AttachListRegistry.NormalizeExeName(exeName);
        if (BlockListRegistry.IsBlocked(exeName))
        {
            await ShowActionNoticeAsync(
                    SettingsNoticeSeverity.Error,
                    GlassSettingsResources.Get("Attach_BlockedProcess"))
                .ConfigureAwait(true);
            return;
        }

        if (AttachListRegistry.Contains(exeName) || IsAttachListDuplicateAfterReconcile(exeName))
        {
            await ShowWindowPickDuplicateDialogAsync(
                    "Attach_WindowPickDuplicateTitle",
                    "Attach_WindowPickDuplicateBody",
                    exeName)
                .ConfigureAwait(true);
            EnsureAddedEntryVisible();
            ApplyFilteredView();
            await FocusAttachEntryOnlyAsync(exeName).ConfigureAwait(true);
            return;
        }

        if (!await ConfirmWindowPickAddAsync(
                    "Attach_WindowPickConfirmTitle",
                    "Attach_WindowPickConfirmBody",
                    exeName)
                .ConfigureAwait(true))
            return;

        await CompleteAttachProcessSelectionAsync(exeName).ConfigureAwait(true);
    }

    private static Task ShowWindowPickDuplicateDialogAsync(string titleKey, string bodyKey, string exeName) =>
        HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
        {
            Title = GlassSettingsResources.Get(titleKey),
            Body = HostedConfirmationDialogService.CreateBody(
                string.Format(
                    System.Globalization.CultureInfo.CurrentCulture,
                    GlassSettingsResources.Get(bodyKey),
                    exeName)),
            Footer = HostedConfirmationDialogService.CreateFooter(
                GlassSettingsResources.Get("Dialog_Understood"),
                primaryButtonWidth: 108),
            MinWidth = 420,
            MaxWidth = 480
        });

    private static Task<bool> ConfirmWindowPickAddAsync(string titleKey, string bodyKey, string exeName) =>
        HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
        {
            Title = GlassSettingsResources.Get(titleKey),
            Body = HostedConfirmationDialogService.CreateBody(
                string.Format(
                    System.Globalization.CultureInfo.CurrentCulture,
                    GlassSettingsResources.Get(bodyKey),
                    exeName)),
            Footer = HostedConfirmationDialogService.CreateCancelConfirmFooter(
                GlassSettingsResources.Get("Attach_WindowPickConfirmButton")),
            MinWidth = 420,
            MaxWidth = 480
        });

    private async Task AddFromFilePickerAsync()
    {
        // MenuFlyout должен закрыться до показа hosted-диалога.
        await Task.Delay(100).ConfigureAwait(true);

        try
        {
            var exeName = await ExecutableFilePickerDialogService.PickAttachListAsync(
                    DispatcherQueue,
                    retainOverlayAfterSelection: true)
                .ConfigureAwait(true);
            if (exeName is null)
                return;

            await CompleteAttachProcessSelectionAsync(exeName, overlayAlreadyUp: true).ConfigureAwait(true);
        }
        catch
        {
            // picker недоступен
        }
    }

    /// <summary>Штора → обновление списка и прокрутка → закрытие шторы → notice.</summary>
    /// <param name="overlayAlreadyUp">true после диалога списка процессов (штора уже открыта).</param>
    private async Task CompleteAttachProcessSelectionAsync(string exeName, bool overlayAlreadyUp = false)
    {
        PendingAttachNotice? notice = null;
        try
        {
            if (!overlayAlreadyUp)
                WindowBusyOverlayService.Push(showProgress: true);

            notice = await TryAddProcessCoreAsync(exeName).ConfigureAwait(true);
        }
        finally
        {
            WindowBusyOverlayService.Pop();
        }

        if (notice is PendingAttachNotice pending)
        {
            await DismissActionNoticeStripAsync().ConfigureAwait(true);
            await ShowActionNoticeAsync(pending.Severity, pending.Message).ConfigureAwait(true);
        }
    }

    private readonly record struct PendingAttachNotice(SettingsNoticeSeverity Severity, string Message);

    private async Task<PendingAttachNotice?> TryAddProcessCoreAsync(string exeName)
    {
        if (string.IsNullOrWhiteSpace(exeName))
            return null;

        exeName = AttachListRegistry.NormalizeExeName(exeName);
        if (BlockListRegistry.IsBlocked(exeName))
        {
            return new PendingAttachNotice(
                SettingsNoticeSeverity.Error,
                GlassSettingsResources.Get("Attach_BlockedProcess"));
        }

        if (IsAttachListDuplicateAfterReconcile(exeName))
        {
            EnsureAddedEntryVisible();
            ApplyFilteredView();
            await FocusAttachEntryOnlyAsync(exeName).ConfigureAwait(true);
            return new PendingAttachNotice(
                SettingsNoticeSeverity.Error,
                GlassSettingsResources.Get("Attach_DuplicateProcess"));
        }

        SettingsSession.Current.WriteGlassDword("AttachList", exeName, (uint)AttachProcessMode.Full);
        AttachListLiveReload.NotifyAttachListChanged();
        EnsureAddedEntryVisible();
        ReloadFromRegistry();
        await FocusAttachEntryOnlyAsync(exeName).ConfigureAwait(true);
        return new PendingAttachNotice(
            SettingsNoticeSeverity.Success,
            GlassSettingsResources.Get("Attach_ActionSuccess"));
    }

    private bool IsAttachListDuplicateAfterReconcile(string exeName)
    {
        var normalized = AttachListRegistry.NormalizeExeName(exeName);
        var inMemory = _allEntries.Any(e =>
            string.Equals(e.ExeName, normalized, StringComparison.OrdinalIgnoreCase));
        var inRegistry = AttachListRegistry.Contains(normalized);

        if (!inMemory && inRegistry)
            AttachListRegistry.Remove(normalized);

        if (inMemory && !inRegistry)
            _allEntries.RemoveAll(e => string.Equals(e.ExeName, normalized, StringComparison.OrdinalIgnoreCase));

        inMemory = _allEntries.Any(e =>
            string.Equals(e.ExeName, normalized, StringComparison.OrdinalIgnoreCase));
        inRegistry = AttachListRegistry.Contains(normalized);
        return inMemory && inRegistry;
    }

    private async Task FocusAttachEntryOnlyAsync(string exeName)
    {
        HighlightAttachEntry(exeName);
        ProcessListExpander.IsExpanded = true;
        await WaitForProcessListLayoutAsync().ConfigureAwait(true);
        await BringProcessItemIntoViewAsync(exeName, animate: false).ConfigureAwait(true);
    }

    private void TryAddToBlockList(string exeName)
    {
        if (string.IsNullOrWhiteSpace(exeName))
            return;

        exeName = BlockListRegistry.NormalizeExeName(exeName);
        if (BlockListRegistry.IsBlocked(exeName))
        {
            ShowActionError("BlockList_DuplicateProcess");
            return;
        }

        BlockListRegistry.Add(exeName);
        AttachListLiveReload.NotifyAttachListChanged();
        ShowActionSuccess();
        BlockListExpander.IsExpanded = true;
        ReloadBlockListFromRegistry();
        FocusAddedBlockListEntry(exeName);
    }

    private void EnsureAddedEntryVisible()
    {
        _searchQuery = string.Empty;
        SearchBox.Text = string.Empty;
        _filter = ProcessFilter.All;

        _suppressFilterEvents = true;
        try
        {
            FilterCombo.SelectedIndex = 0;
        }
        finally
        {
            _suppressFilterEvents = false;
        }
    }

    private void HighlightAttachEntry(string exeName)
    {
        foreach (var item in _allEntries)
            item.SetListHighlighted(false);

        _allEntries.FirstOrDefault(e =>
            string.Equals(e.ExeName, exeName, StringComparison.OrdinalIgnoreCase))?.SetListHighlighted(true);
    }

    private async Task BringProcessItemIntoViewAsync(string exeName, bool animate)
    {
        var index = FindVisibleProcessIndex(exeName);
        if (index < 0)
            return;

        var scroll = PageShell.PageScrollViewer;
        var repeater = FindDescendant<ItemsRepeater>(ProcessItems);
        if (repeater is null)
            return;

        for (var attempt = 0; attempt < 24; attempt++)
        {
            await Task.Delay(16).ConfigureAwait(true);
            ProcessListExpander.UpdateLayout();
            ProcessItems.UpdateLayout();
            ProcessItemsHost.UpdateLayout();
            scroll.UpdateLayout();

            if (repeater.TryGetElement(index) is FrameworkElement element)
            {
                ScrollElementIntoPageView(scroll, element, animate);
                return;
            }

            var estimatedTop = EstimateProcessItemTop(index);
            var targetOffset = Math.Max(0, estimatedTop - scroll.ViewportHeight * 0.35);
            scroll.ChangeView(null, targetOffset, null, disableAnimation: true);
        }
    }

    private int FindVisibleProcessIndex(string exeName)
    {
        if (ProcessItems.ItemsSource is not IEnumerable<ProcessEntryModel> source)
            return -1;

        var index = -1;
        var i = 0;
        foreach (var item in source)
        {
            if (string.Equals(item.ExeName, exeName, StringComparison.OrdinalIgnoreCase))
            {
                index = i;
                break;
            }

            i++;
        }

        return index;
    }

    private double EstimateProcessItemTop(int index)
    {
        const double rowStride = 80;

        var scroll = PageShell.PageScrollViewer;
        if (scroll.Content is not FrameworkElement contentRoot)
            return index * rowStride;

        try
        {
            var hostTop = ProcessItemsHost.TransformToVisual(contentRoot).TransformPoint(new Point(0, 0)).Y;
            return hostTop + index * rowStride;
        }
        catch
        {
            return index * rowStride;
        }
    }

    private static void ScrollElementIntoPageView(ScrollViewer scroll, FrameworkElement element, bool animate)
    {
        if (scroll.Content is not FrameworkElement contentRoot)
            return;

        try
        {
            var topLeft = element.TransformToVisual(contentRoot).TransformPoint(new Point(0, 0));
            var elementHeight = Math.Max(element.ActualHeight, 1);
            var targetOffset = topLeft.Y - (scroll.ViewportHeight - elementHeight) / 2;
            targetOffset = Math.Clamp(targetOffset, 0, Math.Max(0, scroll.ScrollableHeight));
            scroll.ChangeView(null, targetOffset, null, disableAnimation: !animate);
        }
        catch
        {
            // layout ещё не готов
        }
    }

    private async Task WaitForProcessListLayoutAsync()
    {
        for (var i = 0; i < 4; i++)
        {
            await Task.Delay(16).ConfigureAwait(true);
            ProcessItems.UpdateLayout();
            PageShell.PageScrollViewer.UpdateLayout();
        }
    }

    private void FocusAddedBlockListEntry(string exeName) =>
        _ = FocusAddedBlockListEntryAsync(exeName);

    private async Task FocusAddedBlockListEntryAsync(string exeName)
    {
        foreach (var item in _blockListEntries)
            item.SetListHighlighted(false);

        var entry = _blockListEntries.FirstOrDefault(e =>
            string.Equals(e.ExeName, exeName, StringComparison.OrdinalIgnoreCase));
        if (entry is null)
            return;

        entry.SetListHighlighted(true);

        await Task.Delay(1).ConfigureAwait(true);
        BlockListItems.UpdateLayout();

        if (BlockListItems.ItemsSource is not IEnumerable<ProcessEntryModel> source)
            return;

        var index = -1;
        var i = 0;
        foreach (var item in source)
        {
            if (string.Equals(item.ExeName, exeName, StringComparison.OrdinalIgnoreCase))
            {
                index = i;
                break;
            }

            i++;
        }

        if (index < 0)
            return;

        var repeater = FindDescendant<ItemsRepeater>(BlockListItems);
        if (repeater is null)
            return;

        await Task.Delay(1).ConfigureAwait(true);
        repeater.TryGetElement(index)?.StartBringIntoView(new BringIntoViewOptions
        {
            AnimationDesired = true,
            VerticalAlignmentRatio = 0.5
        });
    }

    private static T? FindDescendant<T>(DependencyObject root) where T : DependencyObject
    {
        var count = Microsoft.UI.Xaml.Media.VisualTreeHelper.GetChildrenCount(root);
        for (var i = 0; i < count; i++)
        {
            var child = Microsoft.UI.Xaml.Media.VisualTreeHelper.GetChild(root, i);
            if (child is T match)
                return match;

            var nested = FindDescendant<T>(child);
            if (nested is not null)
                return nested;
        }

        return null;
    }
}
