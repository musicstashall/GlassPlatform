using System.Diagnostics;
using System.Globalization;
using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.ApplicationModel.DataTransfer;
using Windows.Foundation;
using Windows.UI;

namespace GlassSettings.Controls;

/// <summary>
/// Таблица «процесс × модуль»: точки по ModuleBits, цвет — статус процесса.
/// Клик — select; двойной клик — hosted-диалог со сводкой (live); ПКМ — меню по позиции клика.
/// </summary>
public sealed partial class InjectRuntimeModuleTable : UserControl
{
    private sealed class RowVisual
    {
        public required Border Root { get; init; }
        public required Grid ContentGrid { get; init; }
        public required Image IconImage { get; init; }
        public required FontIcon IconFallback { get; init; }
        public required TextBlock NameText { get; init; }
        public required TextBlock PidText { get; init; }
        public required Grid[] ModuleCells { get; init; }
        public uint Pid { get; set; }
        public InjectRuntimeEntry Entry { get; set; } = null!;
        public bool Zebra { get; set; }
        public uint AppliedModuleBits { get; set; } = uint.MaxValue;
        public InjectRuntimeBeacon? AppliedBeacon { get; set; }
        public string? AppliedIconKey { get; set; }
        public int IconLoadGeneration { get; set; }
    }

    private sealed class LiveDialogState
    {
        public required uint Pid { get; init; }
        public required Grid MatrixHost { get; init; }
        public HostedDialogHostContext? HostContext { get; set; }
    }

    private readonly record struct DiagMatrixRow(
        string Title,
        NativeHostInterop.InjectDiagFlags? Flag,
        NativeHostInterop.InjectModuleBits OwnerModule,
        bool IsSubclassCount);

    private static readonly (string Title, NativeHostInterop.InjectModuleBits Bit)[] Modules =
    [
        ("GlassCompat", NativeHostInterop.InjectModuleBits.Compat),
        ("GlassCore", NativeHostInterop.InjectModuleBits.Core),
        ("WndCompat", NativeHostInterop.InjectModuleBits.Wnd),
        ("ExplorerCompat", NativeHostInterop.InjectModuleBits.Explorer),
        ("TaskbarCompat", NativeHostInterop.InjectModuleBits.Taskbar),
        ("UICompat", NativeHostInterop.InjectModuleBits.UICompat),
    ];

    /// <summary>Имена подсистем — hardcoded EN (не локализуются).</summary>
    private static readonly DiagMatrixRow[] DiagMatrixRows =
    [
        new("CrashGuard", NativeHostInterop.InjectDiagFlags.CrashGuard,
            NativeHostInterop.InjectModuleBits.Compat, false),
        new("WinEventProc", NativeHostInterop.InjectDiagFlags.WinEventProc,
            NativeHostInterop.InjectModuleBits.Compat, false),
        new("Hooks", NativeHostInterop.InjectDiagFlags.Hooks,
            NativeHostInterop.InjectModuleBits.Core, false),
        new("PaintDispatcher", NativeHostInterop.InjectDiagFlags.PaintDispatcher,
            NativeHostInterop.InjectModuleBits.Core, false),
        new("ModuleHandler", NativeHostInterop.InjectDiagFlags.ModuleHandler,
            NativeHostInterop.InjectModuleBits.Core, false),
        new("Subclasses", null,
            NativeHostInterop.InjectModuleBits.Wnd, true),
        new("XAML Islands", NativeHostInterop.InjectDiagFlags.Explorer,
            NativeHostInterop.InjectModuleBits.Explorer, false),
        new("XAML VisualTree", NativeHostInterop.InjectDiagFlags.Taskbar,
            NativeHostInterop.InjectModuleBits.Taskbar, false),
        new("IXDE", NativeHostInterop.InjectDiagFlags.UICompat,
            NativeHostInterop.InjectModuleBits.UICompat, false),
    ];

    private readonly Dictionary<uint, RowVisual> _rowsByPid = [];
    private IReadOnlyList<InjectRuntimeEntry> _entries = Array.Empty<InjectRuntimeEntry>();
    private bool _headerBuilt;
    private bool _detailDialogOpen;
    private uint? _selectedPid;
    private uint? _hoveredPid;
    /// <summary>Якорь follow-строки (не использовать после меню/диалога — строка операции двигается).</summary>
    private uint? _scrollAnchorPid;
    private int _interactionFreeze;
    private double? _pinnedScrollOffset;
    /// <summary>Абсолютный offset после меню/диалога, пока приходят snapshot-мутации.</summary>
    private double? _heldScrollOffset;
    private int _scrollHoldGeneration;
    private LiveDialogState? _liveDialog;
    private ScrollViewer? _hookedScroll;
    private bool _guardRestoring;

    public InjectRuntimeModuleTable()
    {
        InitializeComponent();
        ActualThemeChanged += (_, _) => ForceRebuild();
        BuildHeader();
        _headerBuilt = true;
        Loaded += (_, _) => EnsureScrollHook();

        // Глушим BringIntoView на всём контроле (в т.ч. от TextBlock внутри строки).
        AddHandler(
            UIElement.BringIntoViewRequestedEvent,
            new TypedEventHandler<UIElement, BringIntoViewRequestedEventArgs>((_, e) => e.Handled = true),
            handledEventsToo: true);

        // Пиним offset до открытия диалога/меню — иначе host dialog уводит page scroll.
        AddHandler(
            UIElement.PointerPressedEvent,
            new PointerEventHandler((_, e) =>
            {
                var props = e.GetCurrentPoint(this).Properties;
                if (props.IsLeftButtonPressed || props.IsRightButtonPressed)
                    PinPageScroll();
            }),
            handledEventsToo: true);
    }

    private void EnsureScrollHook()
    {
        var scroll = FindAncestorScrollViewer(this);
        if (scroll is null || ReferenceEquals(scroll, _hookedScroll))
            return;
        if (_hookedScroll is not null)
            _hookedScroll.ViewChanged -= OnPageScrollViewChanged;
        _hookedScroll = scroll;
        _hookedScroll.ViewChanged += OnPageScrollViewChanged;
    }

    private void OnPageScrollViewChanged(object? sender, ScrollViewerViewChangedEventArgs e)
    {
        if (sender is not ScrollViewer sv)
            return;

        // Диалог/меню/post-menu hold: page scroll не должен уезжать.
        var pin = _pinnedScrollOffset ?? _heldScrollOffset;
        if (_guardRestoring
            || pin is not double target
            || (_interactionFreeze <= 0 && !_detailDialogOpen && _heldScrollOffset is null)
            || Math.Abs(sv.VerticalOffset - target) <= 0.5)
        {
            return;
        }

        _guardRestoring = true;
        try
        {
            sv.ChangeView(null, target, null, disableAnimation: true);
        }
        finally
        {
            _guardRestoring = false;
        }
    }

    public void SetEntries(IReadOnlyList<InjectRuntimeEntry> entries)
    {
        _entries = entries ?? Array.Empty<InjectRuntimeEntry>();
        SyncRowsIncremental();
        RefreshLiveDialogIfOpen();
    }

    private void ForceRebuild()
    {
        _rowsByPid.Clear();
        RowsHost.Children.Clear();
        BuildHeader();
        _headerBuilt = true;
        SyncRowsIncremental();
    }

    private void BuildHeader()
    {
        HeaderGrid.ColumnDefinitions.Clear();
        HeaderGrid.Children.Clear();

        // Процесс шире; модульные колонки узкие (6 модулей + UICompat).
        HeaderGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(2.2, GridUnitType.Star) });
        for (var i = 0; i < Modules.Length; i++)
            HeaderGrid.ColumnDefinitions.Add(new ColumnDefinition
            {
                Width = new GridLength(0.55, GridUnitType.Star),
                MinWidth = 44,
            });

        var headerStyle = (Style)Resources["RuntimeTableHeaderTextStyle"];
        var processHeader = new TextBlock
        {
            Text = GlassSettingsResources.Get("Runtime_ColProcess"),
            Style = headerStyle,
        };
        HeaderGrid.Children.Add(processHeader);

        for (var i = 0; i < Modules.Length; i++)
        {
            var title = new TextBlock
            {
                Text = ShortModuleTitle(Modules[i].Title),
                Style = headerStyle,
                TextAlignment = TextAlignment.Center,
                HorizontalAlignment = HorizontalAlignment.Stretch,
            };
            Grid.SetColumn(title, i + 1);
            HeaderGrid.Children.Add(title);
        }
    }

    /// <summary>
    /// Точечный sync: удалить устаревшие PID, обновить существующие, вставить новые,
    /// выровнять порядок под snapshot. Прокрутку якорим на выбранной/hovered/последней строке.
    /// Во время клика/диалога — только in-place (без Remove/Insert), иначе второй клик уводит в начало.
    /// </summary>
    private void SyncRowsIncremental()
    {
        if (!_headerBuilt)
        {
            BuildHeader();
            _headerBuilt = true;
        }

        if (_interactionFreeze > 0 || _detailDialogOpen)
        {
            // Без Restore здесь: отложенный ChangeView даёт видимую «пляску» скролла.
            SyncRowsInPlaceOnly();
            return;
        }

        var scroll = FindAncestorScrollViewer(this);
        // Hold — абсолютный offset; без hold не цепляться к _pinnedScrollOffset
        // (иначе sync откатывает ручную прокрутку после отпускания).
        var scrollOffset = _heldScrollOffset ?? scroll?.VerticalOffset ?? 0;
        FrameworkElement? anchorEl = null;
        double? anchorContentY = null;
        // После меню/диалога не follow'ить строку операции (Active→Residual / удаление).
        if (_heldScrollOffset is null)
        {
            var anchorPid = _scrollAnchorPid ?? _selectedPid ?? _hoveredPid;
            if (anchorPid is uint ap
                && _rowsByPid.TryGetValue(ap, out var anchorRow)
                && scroll is not null)
            {
                anchorEl = anchorRow.Root;
                anchorContentY = TryGetOffsetInScrollContent(anchorEl, scroll);
            }
        }

        var mutated = false;
        var keep = new HashSet<uint>(_entries.Select(static e => e.Pid));
        for (var i = RowsHost.Children.Count - 1; i >= 0; i--)
        {
            if (RowsHost.Children[i] is not FrameworkElement { Tag: RowVisual rv })
            {
                RowsHost.Children.RemoveAt(i);
                mutated = true;
                continue;
            }

            if (keep.Contains(rv.Pid))
                continue;

            RowsHost.Children.RemoveAt(i);
            _rowsByPid.Remove(rv.Pid);
            mutated = true;
        }

        for (var i = 0; i < _entries.Count; i++)
        {
            var entry = _entries[i];
            var zebra = i % 2 == 1;

            if (i < RowsHost.Children.Count
                && RowsHost.Children[i] is FrameworkElement { Tag: RowVisual atIndex }
                && atIndex.Pid == entry.Pid)
            {
                ApplyEntry(atIndex, entry, zebra);
                continue;
            }

            if (_rowsByPid.TryGetValue(entry.Pid, out var existing))
            {
                var cur = RowsHost.Children.IndexOf(existing.Root);
                if (cur >= 0 && cur != i)
                {
                    RowsHost.Children.RemoveAt(cur);
                    RowsHost.Children.Insert(i, existing.Root);
                    mutated = true;
                }
                else if (cur < 0)
                {
                    RowsHost.Children.Insert(i, existing.Root);
                    mutated = true;
                }

                ApplyEntry(existing, entry, zebra);
            }
            else
            {
                var row = CreateRow(entry, zebra);
                _rowsByPid[entry.Pid] = row;
                RowsHost.Children.Insert(i, row.Root);
                mutated = true;
            }
        }

        // Якорь только если дерево реально менялось — иначе не трогаем offset пользователя.
        if (mutated)
            RestoreScrollAfterSync(scroll, scrollOffset, anchorEl, anchorContentY);
    }

    /// <summary>Без структурных мутаций Children — только данные существующих строк.</summary>
    private void SyncRowsInPlaceOnly()
    {
        foreach (var entry in _entries)
        {
            if (!_rowsByPid.TryGetValue(entry.Pid, out var row))
                continue;
            ApplyEntry(row, entry, row.Zebra);
        }
    }

    private void PinPageScroll()
    {
        // Новый жест — снимаем post-menu hold.
        if (_heldScrollOffset is not null)
        {
            _scrollHoldGeneration++;
            _heldScrollOffset = null;
        }

        var scroll = FindAncestorScrollViewer(this);
        if (scroll is null)
            return;

        var offset = scroll.VerticalOffset;
        // После закрытия диалога layout может на кадр дать 0 — не затираем хороший pin.
        if (offset < 0.5 && _pinnedScrollOffset is > 0.5)
            return;

        _pinnedScrollOffset = offset;
    }

    private void BeginRowInteraction(uint pid)
    {
        _scrollHoldGeneration++;
        _heldScrollOffset = null;
        _scrollAnchorPid = pid;
        _interactionFreeze++;
        EnsureScrollHook();
        PinPageScroll();
        SetSelectedPid(pid);
    }

    private void EndRowInteraction()
    {
        // Вернуть offset после диалога/меню и держать абсолютный pin,
        // пока snapshot не переставит/удалит строку операции.
        var scroll = FindAncestorScrollViewer(this);
        var hold = _pinnedScrollOffset ?? scroll?.VerticalOffset;
        if (hold is double pin
            && scroll is not null
            && Math.Abs(scroll.VerticalOffset - pin) > 0.5)
        {
            _guardRestoring = true;
            try
            {
                scroll.ChangeView(null, pin, null, disableAnimation: true);
            }
            finally
            {
                _guardRestoring = false;
            }
        }

        _heldScrollOffset = hold;
        _pinnedScrollOffset = hold;
        _scrollAnchorPid = null;
        if (_interactionFreeze > 0)
            _interactionFreeze--;
        SetSelectedPid(null);
        SetHoveredPid(null);

        var gen = ++_scrollHoldGeneration;
        SyncRowsIncremental();
        _ = ReleaseScrollHoldAsync(gen);
    }

    private async Task ReleaseScrollHoldAsync(int generation)
    {
        try
        {
            await Task.Delay(350).ConfigureAwait(true);
        }
        catch (Exception)
        {
            return;
        }

        if (generation != _scrollHoldGeneration)
            return;

        _heldScrollOffset = null;
        _pinnedScrollOffset = null;
    }

    private static void RestoreScrollAfterSync(
        ScrollViewer? scroll,
        double scrollOffsetBefore,
        FrameworkElement? anchorEl,
        double? anchorContentYBefore)
    {
        if (scroll is null)
            return;

        scroll.UpdateLayout();

        if (anchorEl is not null
            && anchorContentYBefore is double beforeY
            && TryGetOffsetInScrollContent(anchorEl, scroll) is double afterY)
        {
            var delta = afterY - beforeY;
            var target = scrollOffsetBefore + delta;
            if (Math.Abs(scroll.VerticalOffset - target) > 0.5)
                scroll.ChangeView(null, target, null, disableAnimation: true);
            return;
        }

        if (Math.Abs(scroll.VerticalOffset - scrollOffsetBefore) > 0.5)
            scroll.ChangeView(null, scrollOffsetBefore, null, disableAnimation: true);
    }

    private static ScrollViewer? FindAncestorScrollViewer(DependencyObject start)
    {
        DependencyObject? current = start;
        while (current is not null)
        {
            if (current is ScrollViewer sv)
                return sv;
            current = VisualTreeHelper.GetParent(current);
        }

        return null;
    }

    private static double? TryGetOffsetInScrollContent(FrameworkElement element, ScrollViewer scroll)
    {
        if (scroll.Content is not UIElement content)
            return null;

        try
        {
            return element.TransformToVisual(content).TransformPoint(new Point(0, 0)).Y;
        }
        catch (Exception)
        {
            return null;
        }
    }

    private RowVisual CreateRow(InjectRuntimeEntry entry, bool zebra)
    {
        var content = new Grid
        {
            MinHeight = 28,
            Padding = new Thickness(4, 2, 4, 2),
            ColumnSpacing = 4,
        };

        content.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(2.2, GridUnitType.Star) });
        for (var i = 0; i < Modules.Length; i++)
            content.ColumnDefinitions.Add(new ColumnDefinition
            {
                Width = new GridLength(0.55, GridUnitType.Star),
                MinWidth = 44,
            });

        var iconImage = new Image
        {
            Width = 16,
            Height = 16,
            Visibility = Visibility.Collapsed,
            VerticalAlignment = VerticalAlignment.Center,
        };
        var iconFallback = new FontIcon
        {
            Glyph = "\uE7C3",
            FontSize = 14,
            Foreground = AppThemeVisualResolver.ResolveThemeBrush("TextFillColorSecondaryBrush"),
            VerticalAlignment = VerticalAlignment.Center,
        };
        var iconHost = new Grid
        {
            Width = 16,
            Height = 16,
            VerticalAlignment = VerticalAlignment.Center,
            Children = { iconImage, iconFallback },
        };

        var nameText = new TextBlock
        {
            Style = (Style)Application.Current.Resources["BodyTextBlockStyle"],
            TextTrimming = TextTrimming.CharacterEllipsis,
            TextWrapping = TextWrapping.NoWrap,
            MaxLines = 1,
            FontSize = 12,
        };
        var pidText = new TextBlock
        {
            Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
            Foreground = AppThemeVisualResolver.ResolveThemeBrush("TextFillColorSecondaryBrush"),
            TextTrimming = TextTrimming.CharacterEllipsis,
            TextWrapping = TextWrapping.NoWrap,
            MaxLines = 1,
            FontSize = 10,
        };

        var textPanel = new StackPanel
        {
            Spacing = 0,
            VerticalAlignment = VerticalAlignment.Center,
            MinWidth = 0,
        };
        textPanel.Children.Add(nameText);
        textPanel.Children.Add(pidText);

        var namePanel = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 8,
            VerticalAlignment = VerticalAlignment.Center,
            Padding = new Thickness(4, 2, 4, 2),
        };
        namePanel.Children.Add(iconHost);
        namePanel.Children.Add(textPanel);
        content.Children.Add(namePanel);

        var moduleCells = new Grid[Modules.Length];
        for (var i = 0; i < Modules.Length; i++)
        {
            var cell = new Grid { HorizontalAlignment = HorizontalAlignment.Stretch };
            Grid.SetColumn(cell, i + 1);
            moduleCells[i] = cell;
            content.Children.Add(cell);
        }

        var root = new Border
        {
            Child = content,
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
            BorderThickness = new Thickness(2),
            BorderBrush = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
            CornerRadius = new CornerRadius(4),
            Margin = new Thickness(0, 1, 0, 1),
            IsTabStop = false,
            AllowFocusOnInteraction = false,
            UseSystemFocusVisuals = false,
        };

        var row = new RowVisual
        {
            Root = root,
            ContentGrid = content,
            IconImage = iconImage,
            IconFallback = iconFallback,
            NameText = nameText,
            PidText = pidText,
            ModuleCells = moduleCells,
            Pid = entry.Pid,
            Entry = entry,
            Zebra = zebra,
        };
        root.Tag = row;

        // Клик/фокус не должны уводить PageScrollViewer через BringIntoView.
        root.BringIntoViewRequested += (_, e) => e.Handled = true;

        root.PointerEntered += (_, _) =>
        {
            if (root.Tag is not RowVisual rv)
                return;
            SetHoveredPid(rv.Pid);
        };
        root.PointerExited += (_, _) =>
        {
            if (root.Tag is not RowVisual rv)
                return;
            if (_hoveredPid == rv.Pid)
                SetHoveredPid(null);
        };
        root.Tapped += (_, e) =>
        {
            e.Handled = true;
            if (root.Tag is RowVisual rv)
                SetSelectedPid(rv.Pid);
        };
        root.DoubleTapped += async (_, e) =>
        {
            e.Handled = true;
            if (root.Tag is not RowVisual rv)
                return;
            SetSelectedPid(rv.Pid);
            await ShowDetailDialogAsync(rv.Entry).ConfigureAwait(true);
        };
        root.RightTapped += (_, e) =>
        {
            e.Handled = true;
            if (root.Tag is not RowVisual rv)
                return;
            // RightTapped часто без PointerPressed на том же пути — пин здесь.
            BeginRowInteraction(rv.Pid);
            ShowContextMenu(root, rv.Entry, e.GetPosition(root));
        };

        ApplyEntry(row, entry, zebra, force: true);
        return row;
    }

    private void ApplyEntry(RowVisual row, InjectRuntimeEntry entry, bool zebra, bool force = false)
    {
        row.Pid = entry.Pid;
        row.Entry = entry;

        var name = entry.FormatProcessTitle();
        if (force || !string.Equals(row.NameText.Text, name, StringComparison.Ordinal))
            row.NameText.Text = name;

        var pidText = $"PID {entry.Pid}";
        if (force || !string.Equals(row.PidText.Text, pidText, StringComparison.Ordinal))
            row.PidText.Text = pidText;

        if (force || !string.Equals(row.AppliedIconKey, entry.ExeName, StringComparison.OrdinalIgnoreCase))
            BindProcessIcon(row, entry.ExeName);

        if (force || row.Zebra != zebra)
        {
            row.Zebra = zebra;
            row.ContentGrid.Background = zebra
                ? AppThemeVisualResolver.ResolveThemeBrush("SubtleFillColorSecondaryBrush")
                : null;
        }

        var bitsChanged = force
            || row.AppliedModuleBits != entry.ModuleBits
            || row.AppliedBeacon != entry.Beacon;
        if (bitsChanged)
        {
            row.AppliedModuleBits = entry.ModuleBits;
            row.AppliedBeacon = entry.Beacon;

            var brush = InjectRuntimeRowModel.BrushFor(entry.Beacon);
            var bits = (NativeHostInterop.InjectModuleBits)entry.ModuleBits;
            for (var i = 0; i < Modules.Length; i++)
            {
                var cell = row.ModuleCells[i];
                var on = bits.HasFlag(Modules[i].Bit);
                if (on)
                {
                    if (cell.Children.Count == 1 && cell.Children[0] is Ellipse ellipse)
                    {
                        ellipse.Fill = brush;
                    }
                    else
                    {
                        cell.Children.Clear();
                        cell.Children.Add(new Ellipse
                        {
                            Width = 8,
                            Height = 8,
                            Fill = brush,
                            HorizontalAlignment = HorizontalAlignment.Center,
                            VerticalAlignment = VerticalAlignment.Center,
                        });
                    }
                }
                else if (cell.Children.Count > 0)
                {
                    cell.Children.Clear();
                }
            }
        }

        ApplyRowChrome(row);
    }

    private void BindProcessIcon(RowVisual row, string exeName)
    {
        row.AppliedIconKey = exeName;
        var generation = ++row.IconLoadGeneration;

        ApplyIconVisual(row, null);

        if (string.IsNullOrWhiteSpace(exeName))
            return;

        var queue = DispatcherQueue;
        _ = LoadProcessIconAsync(row, exeName, generation, queue);
    }

    private async Task LoadProcessIconAsync(
        RowVisual row,
        string exeName,
        int generation,
        DispatcherQueue queue)
    {
        // Отдельная копия BitmapImage — не шарить с плитками AttachList.
        var icon = await ProcessIconResolver.GetIconCopyAsync(exeName, queue).ConfigureAwait(true)
                   ?? await ProcessIconResolver.GetFileTypeIconCopyAsync(exeName, queue).ConfigureAwait(true);

        if (generation != row.IconLoadGeneration)
            return;
        if (!string.Equals(row.AppliedIconKey, exeName, StringComparison.OrdinalIgnoreCase))
            return;

        ApplyIconVisual(row, icon);
    }

    private static void ApplyIconVisual(RowVisual row, ImageSource? icon)
    {
        if (icon is not null)
        {
            row.IconImage.Source = icon;
            row.IconImage.Visibility = Visibility.Visible;
            row.IconFallback.Visibility = Visibility.Collapsed;
            return;
        }

        row.IconImage.Source = null;
        row.IconImage.Visibility = Visibility.Collapsed;
        row.IconFallback.Visibility = Visibility.Visible;
    }

    private void ApplyRowChrome(RowVisual row)
    {
        var selected = _selectedPid == row.Pid;
        var hovered = _hoveredPid == row.Pid;
        if (selected || hovered)
        {
            row.Root.BorderBrush = AppThemeVisualResolver.ResolveThemeBrush("AccentFillColorDefaultBrush")
                ?? new SolidColorBrush(Color.FromArgb(0xFF, 0x00, 0x78, 0xD4));
        }
        else if (row.Root.BorderBrush is not SolidColorBrush { Color.A: 0 })
        {
            row.Root.BorderBrush = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        }
    }

    private void SetSelectedPid(uint? pid)
    {
        if (_selectedPid == pid)
            return;

        var previous = _selectedPid;
        _selectedPid = pid;
        if (previous is uint prev && _rowsByPid.TryGetValue(prev, out var prevRow))
            ApplyRowChrome(prevRow);
        if (pid is uint next && _rowsByPid.TryGetValue(next, out var nextRow))
            ApplyRowChrome(nextRow);
    }

    private void SetHoveredPid(uint? pid)
    {
        if (_hoveredPid == pid)
            return;

        var previous = _hoveredPid;
        _hoveredPid = pid;
        if (previous is uint prev && _rowsByPid.TryGetValue(prev, out var prevRow))
            ApplyRowChrome(prevRow);
        if (pid is uint next && _rowsByPid.TryGetValue(next, out var nextRow))
            ApplyRowChrome(nextRow);
    }

    private void ShowContextMenu(FrameworkElement anchor, InjectRuntimeEntry entry, Point position)
    {
        var menu = new MenuFlyout();

        var exeName = string.IsNullOrWhiteSpace(entry.ExeName)
            ? string.Empty
            : AttachListRegistry.NormalizeExeName(entry.ExeName);
        var inUserList = exeName.Length > 0 && AttachListRegistry.Contains(exeName);
        var inForbiddenList = exeName.Length > 0 && BlockListRegistry.IsBlocked(exeName);

        var unload = CreateUnloadMenuItem(
            GlassSettingsResources.Get("Runtime_Unload"),
            entry.CanRequestUnload,
            entry.UnloadDisabledReason,
            () => RequestUnload(entry.Pid));
        menu.Items.Add(unload);

        var unloadAndExclude = CreateUnloadMenuItem(
            GlassSettingsResources.Get("Runtime_UnloadAndExclude"),
            inUserList && entry.CanRequestUnload,
            entry.UnloadDisabledReason,
            () => UnloadAndExclude(entry));
        menu.Items.Add(unloadAndExclude);

        var unloadAndForbid = CreateUnloadMenuItem(
            GlassSettingsResources.Get("Runtime_UnloadAndForbid"),
            exeName.Length > 0 && !inForbiddenList && entry.CanRequestUnload,
            entry.UnloadDisabledReason,
            () => UnloadAndForbid(entry));
        menu.Items.Add(unloadAndForbid);

        menu.Items.Add(new MenuFlyoutSeparator());

        var copyName = new MenuFlyoutItem
        {
            Text = GlassSettingsResources.Get("Runtime_CopyName"),
            IsEnabled = exeName.Length > 0,
        };
        copyName.Click += (_, _) => CopyProcessName(exeName);
        menu.Items.Add(copyName);

        var subsystemState = new MenuFlyoutItem
        {
            Text = GlassSettingsResources.Get("Runtime_SubsystemState"),
        };
        subsystemState.Click += async (_, _) =>
        {
            await ShowDetailDialogAsync(entry).ConfigureAwait(true);
        };
        menu.Items.Add(subsystemState);

        menu.Items.Add(new MenuFlyoutSeparator());

        var endTask = new MenuFlyoutItem
        {
            Text = GlassSettingsResources.Get("Runtime_EndTask"),
        };
        endTask.Click += (_, _) => EndProcess(entry.Pid, hardKill: false);
        menu.Items.Add(endTask);

        var kill = new MenuFlyoutItem
        {
            Text = GlassSettingsResources.Get("Runtime_Kill"),
        };
        kill.Click += (_, _) => EndProcess(entry.Pid, hardKill: true);
        menu.Items.Add(kill);

        menu.ShowAt(anchor, new FlyoutShowOptions
        {
            Position = position,
            ShowMode = FlyoutShowMode.Standard,
        });
        menu.Closed += (_, _) => EndRowInteraction();
    }

    private async Task ShowDetailDialogAsync(InjectRuntimeEntry entry)
    {
        if (_detailDialogOpen)
            return;

        BeginRowInteraction(entry.Pid);
        _detailDialogOpen = true;

        var matrixHost = new Grid { HorizontalAlignment = HorizontalAlignment.Stretch };
        MountDiagMatrix(matrixHost, entry);

        var body = new StackPanel { Spacing = 12 };
        body.Children.Add(new TextBlock
        {
            Text = entry.FormatPidMeta(),
            Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
            Foreground = AppThemeVisualResolver.ResolveThemeBrush("TextFillColorSecondaryBrush"),
        });
        body.Children.Add(matrixHost);

        _liveDialog = new LiveDialogState
        {
            Pid = entry.Pid,
            MatrixHost = matrixHost,
        };

        void OnSnapshotChanged(InjectRuntimeSnapshot snapshot) =>
            DispatcherQueue.TryEnqueue(() => ApplyLiveDialogSnapshot(snapshot));

        InjectRuntimeMonitor.Instance.SnapshotChanged += OnSnapshotChanged;
        try
        {
            var title = string.Format(
                CultureInfo.CurrentCulture,
                GlassSettingsResources.Get("Runtime_SubsystemsTitleFormat"),
                string.IsNullOrWhiteSpace(entry.ExeName) ? entry.FormatProcessTitle() : entry.ExeName);

            var result = await HostedDialogService.ShowAsync(new HostedDialogSpec
            {
                Title = title,
                Body = HostedDialogLayout.WrapBodyWithContentPadding(body),
                MinWidth = 560,
                MaxWidth = 720,
                FooterDividerFullWidth = true,
                FooterDividerTopMargin = 16,
                Footer = new HostedDialogFooterSpec
                {
                    PrimaryButtonText = GlassSettingsResources.Get("Runtime_Unload"),
                    PrimaryButtonWidth = 120,
                    IsPrimaryEnabled = entry.CanRequestUnload,
                },
            }, ctx => _liveDialog.HostContext = ctx).ConfigureAwait(true);

            if (result == HostedDialogResult.Primary)
                RequestUnload(entry.Pid);
        }
        finally
        {
            InjectRuntimeMonitor.Instance.SnapshotChanged -= OnSnapshotChanged;
            _liveDialog = null;
            _detailDialogOpen = false;
            EndRowInteraction();
        }
    }

    private void RefreshLiveDialogIfOpen()
    {
        if (!_detailDialogOpen || _liveDialog is null)
            return;
        ApplyLiveDialogSnapshot(InjectRuntimeMonitor.Instance.Latest);
    }

    private void ApplyLiveDialogSnapshot(InjectRuntimeSnapshot snapshot)
    {
        if (_liveDialog is null)
            return;

        var entry = snapshot.Entries.FirstOrDefault(e => e.Pid == _liveDialog.Pid);
        if (entry is null)
            return;

        MountDiagMatrix(_liveDialog.MatrixHost, entry);
        _liveDialog.HostContext?.SetPrimaryEnabled(entry.CanRequestUnload);
        if (_liveDialog.HostContext is not null && !entry.CanRequestUnload && entry.UnloadDisabledReason is { Length: > 0 } reason)
            ToolTipService.SetToolTip(_liveDialog.HostContext.PrimaryButton, reason);

        if (_rowsByPid.TryGetValue(entry.Pid, out var row))
            ApplyEntry(row, entry, row.ContentGrid.Background is not null);
    }

    private static void MountDiagMatrix(Grid host, InjectRuntimeEntry entry)
    {
        host.Children.Clear();
        host.Children.Add(BuildDiagMatrix(entry));
    }

    private static Grid BuildDiagMatrix(InjectRuntimeEntry entry)
    {
        var diagFlags = (NativeHostInterop.InjectDiagFlags)entry.DiagFlags;
        var moduleBits = (NativeHostInterop.InjectModuleBits)entry.ModuleBits;
        var borderBrush = AppThemeVisualResolver.ResolveThemeBrush("DividerStrokeColorDefaultBrush")
            ?? new SolidColorBrush(Color.FromArgb(0x40, 0x80, 0x80, 0x80));
        var activeBrush = new SolidColorBrush(Color.FromArgb(0xFF, 0x16, 0xC6, 0xA));

        var visibleMods = new List<(string Title, NativeHostInterop.InjectModuleBits Bit, int Col)>();
        for (var i = 0; i < Modules.Length; i++)
        {
            if (moduleBits.HasFlag(Modules[i].Bit))
                visibleMods.Add((Modules[i].Title, Modules[i].Bit, visibleMods.Count + 1));
        }

        if (visibleMods.Count == 0)
        {
            for (var i = 0; i < Modules.Length; i++)
                visibleMods.Add((Modules[i].Title, Modules[i].Bit, i + 1));
        }

        var visibleRows = DiagMatrixRows
            .Where(r => moduleBits == 0 || moduleBits.HasFlag(r.OwnerModule))
            .ToArray();

        var grid = new Grid { HorizontalAlignment = HorizontalAlignment.Stretch };
        grid.ColumnDefinitions.Add(new ColumnDefinition
        {
            Width = new GridLength(2.6, GridUnitType.Star),
            MinWidth = 148,
        });
        foreach (var _ in visibleMods)
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

        for (var r = 0; r <= visibleRows.Length; r++)
            grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        // Заголовок колонки подсистем — локализованный.
        AddMatrixCell(grid, 0, 0, borderBrush, new TextBlock
        {
            Text = GlassSettingsResources.Get("Runtime_FlyoutSubsystems"),
            FontSize = 10,
            Foreground = AppThemeVisualResolver.ResolveThemeBrush("TextFillColorSecondaryBrush"),
            Margin = new Thickness(6, 4, 6, 4),
            TextTrimming = TextTrimming.CharacterEllipsis,
        });
        foreach (var mod in visibleMods)
        {
            AddMatrixCell(grid, 0, mod.Col, borderBrush, new TextBlock
            {
                Text = ShortModuleTitle(mod.Title),
                FontSize = 10,
                Foreground = AppThemeVisualResolver.ResolveThemeBrush("TextFillColorSecondaryBrush"),
                HorizontalAlignment = HorizontalAlignment.Center,
                TextAlignment = TextAlignment.Center,
                Margin = new Thickness(2, 4, 2, 4),
                TextTrimming = TextTrimming.CharacterEllipsis,
            });
        }

        for (var ri = 0; ri < visibleRows.Length; ri++)
        {
            var row = visibleRows[ri];
            var gridRow = ri + 1;
            var label = row.Title;
            if (row.IsSubclassCount && entry.SubclassWindowTotal > 0)
                label = $"{label} ({entry.SubclassWindowTotal})";

            AddMatrixCell(grid, gridRow, 0, borderBrush, new TextBlock
            {
                Text = label,
                FontSize = 12,
                Margin = new Thickness(6, 4, 6, 4),
                TextTrimming = TextTrimming.CharacterEllipsis,
                VerticalAlignment = VerticalAlignment.Center,
            });

            foreach (var mod in visibleMods)
            {
                var active = row.IsSubclassCount
                    ? entry.SubclassWindowTotal > 0 && mod.Bit == row.OwnerModule
                    : row.Flag is { } flag
                        && diagFlags.HasFlag(flag)
                        && mod.Bit == row.OwnerModule;

                UIElement? content = active
                    ? new Ellipse
                    {
                        Width = 8,
                        Height = 8,
                        Fill = activeBrush,
                        HorizontalAlignment = HorizontalAlignment.Center,
                        VerticalAlignment = VerticalAlignment.Center,
                    }
                    : null;
                AddMatrixCell(grid, gridRow, mod.Col, borderBrush, content);
            }
        }

        return grid;
    }

    private static string ShortModuleTitle(string title) => title switch
    {
        "GlassCompat" => "Compat",
        "GlassCore" => "Core",
        "WndCompat" => "Wnd",
        "ExplorerCompat" => "Expl",
        "TaskbarCompat" => "Task",
        "UICompat" => "UI",
        _ => title,
    };

    private static void AddMatrixCell(Grid grid, int row, int col, Brush borderBrush, UIElement? content)
    {
        var border = new Border
        {
            BorderBrush = borderBrush,
            BorderThickness = new Thickness(0.5),
            Padding = new Thickness(2),
            MinHeight = 26,
            Child = content,
        };
        Grid.SetRow(border, row);
        Grid.SetColumn(border, col);
        grid.Children.Add(border);
    }

    private static void CopyProcessName(string exeName)
    {
        if (string.IsNullOrWhiteSpace(exeName))
            return;

        try
        {
            var data = new DataPackage();
            data.SetText(exeName);
            Clipboard.SetContent(data);
        }
        catch (Exception)
        {
            // буфер обмена недоступен
        }
    }

    private static MenuFlyoutItem CreateUnloadMenuItem(
        string text,
        bool isEnabled,
        string? disabledReason,
        Action onClick)
    {
        var item = new MenuFlyoutItem
        {
            Text = text,
            IsEnabled = isEnabled,
        };
        if (!isEnabled && disabledReason is { Length: > 0 })
            ToolTipService.SetToolTip(item, disabledReason);
        item.Click += (_, _) => onClick();
        return item;
    }

    private static void RequestUnload(uint pid)
    {
        if (pid == 0)
            return;
        // ForceUnload — CreateRemoteThread+Wait; не на UI-потоке.
        _ = System.Threading.Tasks.Task.Run(
            () => InjectRuntimeMonitor.Instance.RequestProcessUnload(pid));
    }

    private static void UnloadAndExclude(InjectRuntimeEntry entry)
    {
        if (entry.Pid == 0 || string.IsNullOrWhiteSpace(entry.ExeName))
            return;

        var exeName = AttachListRegistry.NormalizeExeName(entry.ExeName);
        if (!AttachListRegistry.Contains(exeName))
            return;

        AttachListRegistry.Remove(exeName);
        AttachListLiveReload.NotifyAttachListChanged();
        RequestUnload(entry.Pid);
    }

    private static void UnloadAndForbid(InjectRuntimeEntry entry)
    {
        if (entry.Pid == 0 || string.IsNullOrWhiteSpace(entry.ExeName))
            return;

        var exeName = AttachListRegistry.NormalizeExeName(entry.ExeName);
        if (BlockListRegistry.IsBlocked(exeName))
            return;

        // 1) Убрать из списка пользовательских процессов (AttachList), если есть.
        if (AttachListRegistry.Contains(exeName))
            AttachListRegistry.Remove(exeName);

        // 2) В список запрещённых (BlockList).
        BlockListRegistry.Add(exeName);

        AttachListLiveReload.NotifyAttachListChanged();

        // 3) Выгрузка модулей.
        RequestUnload(entry.Pid);
    }

    private static void EndProcess(uint pid, bool hardKill)
    {
        if (pid == 0 || pid == (uint)Environment.ProcessId)
            return;

        try
        {
            using var process = Process.GetProcessById(unchecked((int)pid));
            if (hardKill)
            {
                // Жёстко, как «Завершить задачу» / дерево в Диспетчере задач.
                process.Kill(entireProcessTree: true);
                return;
            }

            // Стандартная остановка: WM_CLOSE главному окну; иначе TerminateProcess.
            if (!process.CloseMainWindow())
                process.Kill(entireProcessTree: false);
        }
        catch (ArgumentException)
        {
            // процесс уже завершён
        }
        catch (InvalidOperationException)
        {
            // нет доступа / уже вышел
        }
        catch (System.ComponentModel.Win32Exception)
        {
            // отказ в доступе
        }
    }
}
