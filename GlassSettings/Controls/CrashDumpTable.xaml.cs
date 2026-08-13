using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;
using Windows.UI;

namespace GlassSettings.Controls;

/// <summary>
/// Таблица минидампов CrashGuard: имя / дата-время / PID / размер.
/// Клик по заголовку — сортировка как в файловом диалоге (повтор — направление).
/// Клик по строке — select; двойной клик — открыть; ПКМ — Открыть / Удалить.
/// </summary>
public sealed partial class CrashDumpTable : UserControl
{
    private enum SortColumn
    {
        Name,
        Date,
        Pid,
        Size,
    }

    private sealed class HeaderVisual
    {
        public required SortColumn Column { get; init; }
        public required Border Root { get; init; }
        public required TextBlock Title { get; init; }
        public required FontIcon Glyph { get; init; }
    }

    private sealed class RowVisual
    {
        public required Border Root { get; init; }
        public required Grid ContentGrid { get; init; }
        public required Image IconImage { get; init; }
        public required FontIcon IconFallback { get; init; }
        public required TextBlock NameText { get; init; }
        public required TextBlock DateText { get; init; }
        public required TextBlock PidText { get; init; }
        public required TextBlock SizeText { get; init; }
        public CrashDumpEntry Entry { get; set; } = null!;
        public bool Zebra { get; set; }
        public string? AppliedIconKey { get; set; }
        public int IconLoadGeneration { get; set; }
    }

    private readonly Dictionary<string, RowVisual> _rowsByPath = new(StringComparer.OrdinalIgnoreCase);
    private readonly List<HeaderVisual> _headers = [];
    private IReadOnlyList<CrashDumpEntry> _sourceEntries = Array.Empty<CrashDumpEntry>();
    private CrashDumpEntry[] _viewEntries = [];
    private bool _headerBuilt;
    private string? _selectedPath;
    private string? _hoveredPath;
    private SortColumn _sortColumn = SortColumn.Date;
    private bool _sortAscending;

    public event EventHandler<CrashDumpEntry>? OpenRequested;
    public event EventHandler<CrashDumpEntry>? DeleteRequested;

    public CrashDumpTable()
    {
        InitializeComponent();
        ActualThemeChanged += (_, _) => ForceRebuild();
        BuildHeader();
        _headerBuilt = true;
    }

    public void SetEntries(IReadOnlyList<CrashDumpEntry> entries)
    {
        _sourceEntries = entries ?? Array.Empty<CrashDumpEntry>();
        ApplySortAndRebuild();
    }

    public void ApplyLocalizedStrings()
    {
        BuildHeader();
        _headerBuilt = true;
    }

    private void ForceRebuild()
    {
        BuildHeader();
        _headerBuilt = true;
        RebuildRows();
    }

    private void BuildHeader()
    {
        HeaderGrid.ColumnDefinitions.Clear();
        HeaderGrid.Children.Clear();
        _headers.Clear();

        // Имя — Star; дата — Auto; PID/размер — Star.
        HeaderGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1.4, GridUnitType.Star) });
        HeaderGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        HeaderGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(0.55, GridUnitType.Star) });
        HeaderGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(0.7, GridUnitType.Star) });

        AddHeaderCell(SortColumn.Name, GlassSettingsResources.Get("Crashes_ColName"), 0, leftAlign: true);
        AddHeaderCell(SortColumn.Date, GlassSettingsResources.Get("Crashes_ColDate"), 1, leftAlign: false);
        AddHeaderCell(SortColumn.Pid, GlassSettingsResources.Get("Crashes_ColPid"), 2, leftAlign: false);
        AddHeaderCell(SortColumn.Size, GlassSettingsResources.Get("Crashes_ColSize"), 3, leftAlign: false);
        UpdateHeaderGlyphs();
    }

    private void AddHeaderCell(SortColumn column, string text, int gridColumn, bool leftAlign)
    {
        var title = new TextBlock
        {
            Text = text,
            Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
            Foreground = AppThemeVisualResolver.ResolveThemeBrush("TextFillColorSecondaryBrush"),
            TextTrimming = TextTrimming.CharacterEllipsis,
            TextWrapping = TextWrapping.NoWrap,
            TextAlignment = leftAlign ? TextAlignment.Left : TextAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            MaxLines = 1,
        };

        var glyph = new FontIcon
        {
            Glyph = "\uE70D",
            FontSize = 10,
            Foreground = AppThemeVisualResolver.ResolveThemeBrush("TextFillColorSecondaryBrush"),
            VerticalAlignment = VerticalAlignment.Center,
            Visibility = Visibility.Collapsed,
            Margin = new Thickness(4, 0, 0, 0),
        };

        var content = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 0,
            HorizontalAlignment = leftAlign ? HorizontalAlignment.Left : HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            Children = { title, glyph },
        };

        var root = new Border
        {
            Child = content,
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
            Padding = new Thickness(2, 2, 2, 2),
            CornerRadius = new CornerRadius(3),
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Stretch,
        };

        root.PointerEntered += (_, _) =>
        {
            root.Background = AppThemeVisualResolver.ResolveThemeBrush("SubtleFillColorSecondaryBrush")
                ?? new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        };
        root.PointerExited += (_, _) =>
        {
            root.Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        };
        root.Tapped += (_, e) =>
        {
            e.Handled = true;
            OnHeaderClicked(column);
        };

        Grid.SetColumn(root, gridColumn);
        HeaderGrid.Children.Add(root);
        _headers.Add(new HeaderVisual
        {
            Column = column,
            Root = root,
            Title = title,
            Glyph = glyph,
        });
    }

    private void OnHeaderClicked(SortColumn column)
    {
        if (_sortColumn == column)
            _sortAscending = !_sortAscending;
        else
        {
            _sortColumn = column;
            // Как в проводнике: имя A→Z; дата/размер — сначала «новые/большие» (desc).
            _sortAscending = column is SortColumn.Name or SortColumn.Pid;
        }

        ApplySortAndRebuild();
    }

    private void ApplySortAndRebuild()
    {
        _viewEntries = SortEntries(_sourceEntries, _sortColumn, _sortAscending);
        UpdateHeaderGlyphs();
        RebuildRows();
    }

    private void UpdateHeaderGlyphs()
    {
        foreach (var header in _headers)
        {
            var active = header.Column == _sortColumn;
            header.Glyph.Visibility = active ? Visibility.Visible : Visibility.Collapsed;
            if (active)
                header.Glyph.Glyph = _sortAscending ? "\uE70E" : "\uE70D";

            header.Title.Foreground = AppThemeVisualResolver.ResolveThemeBrush(
                active ? "TextFillColorPrimaryBrush" : "TextFillColorSecondaryBrush");
            header.Glyph.Foreground = header.Title.Foreground;
        }
    }

    private static CrashDumpEntry[] SortEntries(
        IReadOnlyList<CrashDumpEntry> source,
        SortColumn column,
        bool ascending)
    {
        if (source.Count == 0)
            return [];

        var list = source.ToList();
        list.Sort((a, b) =>
        {
            var cmp = column switch
            {
                SortColumn.Name => string.Compare(a.DisplayName, b.DisplayName, StringComparison.OrdinalIgnoreCase),
                SortColumn.Date => a.Timestamp.CompareTo(b.Timestamp),
                SortColumn.Pid => ComparePid(a.Pid, b.Pid),
                SortColumn.Size => a.SizeBytes.CompareTo(b.SizeBytes),
                _ => 0,
            };
            if (cmp == 0)
                cmp = string.Compare(a.FullPath, b.FullPath, StringComparison.OrdinalIgnoreCase);
            return ascending ? cmp : -cmp;
        });
        return list.ToArray();
    }

    private static int ComparePid(uint? a, uint? b)
    {
        if (a is null && b is null)
            return 0;
        if (a is null)
            return 1;
        if (b is null)
            return -1;
        return a.Value.CompareTo(b.Value);
    }

    private void RebuildRows()
    {
        if (!_headerBuilt)
        {
            BuildHeader();
            _headerBuilt = true;
        }

        _rowsByPath.Clear();
        RowsHost.Children.Clear();

        if (_selectedPath is not null
            && !_viewEntries.Any(e => string.Equals(e.FullPath, _selectedPath, StringComparison.OrdinalIgnoreCase)))
            _selectedPath = null;
        if (_hoveredPath is not null
            && !_viewEntries.Any(e => string.Equals(e.FullPath, _hoveredPath, StringComparison.OrdinalIgnoreCase)))
            _hoveredPath = null;

        for (var i = 0; i < _viewEntries.Length; i++)
        {
            var row = CreateRow(_viewEntries[i], i % 2 == 1);
            _rowsByPath[row.Entry.FullPath] = row;
            RowsHost.Children.Add(row.Root);
        }
    }

    private RowVisual CreateRow(CrashDumpEntry entry, bool zebra)
    {
        var content = new Grid
        {
            MinHeight = 28,
            Padding = new Thickness(4, 2, 4, 2),
            ColumnSpacing = 4,
        };

        content.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1.4, GridUnitType.Star) });
        content.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        content.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(0.55, GridUnitType.Star) });
        content.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(0.7, GridUnitType.Star) });

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
            Text = entry.DisplayName ?? string.Empty,
            Style = (Style)Application.Current.Resources["BodyTextBlockStyle"],
            Foreground = AppThemeVisualResolver.ResolveThemeBrush("TextFillColorPrimaryBrush"),
            TextTrimming = TextTrimming.CharacterEllipsis,
            TextWrapping = TextWrapping.NoWrap,
            TextAlignment = TextAlignment.Left,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Center,
            FontSize = 12,
            MaxLines = 1,
        };

        var nameHost = new Grid
        {
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Center,
            ColumnSpacing = 6,
        };
        nameHost.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        nameHost.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        Grid.SetColumn(iconHost, 0);
        Grid.SetColumn(nameText, 1);
        nameHost.Children.Add(iconHost);
        nameHost.Children.Add(nameText);

        var dateText = MakeCell(entry.DateText, secondary: false, stretch: false);
        var pidText = MakeCell(entry.PidText, secondary: true, stretch: true);
        var sizeText = MakeCell(entry.SizeText, secondary: true, stretch: true);

        Grid.SetColumn(nameHost, 0);
        Grid.SetColumn(dateText, 1);
        Grid.SetColumn(pidText, 2);
        Grid.SetColumn(sizeText, 3);
        content.Children.Add(nameHost);
        content.Children.Add(dateText);
        content.Children.Add(pidText);
        content.Children.Add(sizeText);

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
            DateText = dateText,
            PidText = pidText,
            SizeText = sizeText,
            Entry = entry,
            Zebra = zebra,
        };
        root.Tag = row;

        if (zebra)
            content.Background = AppThemeVisualResolver.ResolveThemeBrush("SubtleFillColorSecondaryBrush");

        // Подсказка — реальное имя файла на диске.
        ToolTipService.SetToolTip(root, entry.FileName);
        BindRowIcon(row, entry);

        root.BringIntoViewRequested += (_, e) => e.Handled = true;

        root.PointerEntered += (_, _) =>
        {
            if (root.Tag is RowVisual rv)
                SetHoveredPath(rv.Entry.FullPath);
        };
        root.PointerExited += (_, _) =>
        {
            if (root.Tag is not RowVisual rv)
                return;
            if (string.Equals(_hoveredPath, rv.Entry.FullPath, StringComparison.OrdinalIgnoreCase))
                SetHoveredPath(null);
        };
        root.Tapped += (_, e) =>
        {
            e.Handled = true;
            if (root.Tag is RowVisual rv)
                SetSelectedPath(rv.Entry.FullPath);
        };
        root.DoubleTapped += (_, e) =>
        {
            e.Handled = true;
            if (root.Tag is RowVisual rv)
            {
                SetSelectedPath(rv.Entry.FullPath);
                OpenRequested?.Invoke(this, rv.Entry);
            }
        };
        root.RightTapped += (_, e) =>
        {
            e.Handled = true;
            if (root.Tag is not RowVisual rv)
                return;
            SetSelectedPath(rv.Entry.FullPath);
            ShowContextMenu(root, rv.Entry, e.GetPosition(root));
        };

        ApplyRowChrome(row);
        return row;
    }

    private void BindRowIcon(RowVisual row, CrashDumpEntry entry)
    {
        // Иконка типа .dmp, не exe упавшего процесса.
        const string iconKey = "_.dmp";
        row.AppliedIconKey = iconKey;
        var generation = ++row.IconLoadGeneration;

        ApplyIconVisual(row, null);

        var queue = DispatcherQueue;
        _ = LoadRowIconAsync(row, iconKey, generation, queue);
    }

    private async Task LoadRowIconAsync(
        RowVisual row,
        string iconKey,
        int generation,
        DispatcherQueue queue)
    {
        var icon = await ProcessIconResolver.GetFileTypeIconCopyAsync(iconKey, queue).ConfigureAwait(true);

        if (generation != row.IconLoadGeneration)
            return;
        if (!string.Equals(row.AppliedIconKey, iconKey, StringComparison.OrdinalIgnoreCase))
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
        var selected = string.Equals(_selectedPath, row.Entry.FullPath, StringComparison.OrdinalIgnoreCase);
        var hovered = string.Equals(_hoveredPath, row.Entry.FullPath, StringComparison.OrdinalIgnoreCase);
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

    private void SetSelectedPath(string? path)
    {
        if (string.Equals(_selectedPath, path, StringComparison.OrdinalIgnoreCase))
            return;

        var previous = _selectedPath;
        _selectedPath = path;
        if (previous is not null && _rowsByPath.TryGetValue(previous, out var prevRow))
            ApplyRowChrome(prevRow);
        if (path is not null && _rowsByPath.TryGetValue(path, out var nextRow))
            ApplyRowChrome(nextRow);
    }

    private void SetHoveredPath(string? path)
    {
        if (string.Equals(_hoveredPath, path, StringComparison.OrdinalIgnoreCase))
            return;

        var previous = _hoveredPath;
        _hoveredPath = path;
        if (previous is not null && _rowsByPath.TryGetValue(previous, out var prevRow))
            ApplyRowChrome(prevRow);
        if (path is not null && _rowsByPath.TryGetValue(path, out var nextRow))
            ApplyRowChrome(nextRow);
    }

    private static TextBlock MakeCell(string text, bool secondary, bool stretch)
    {
        return new TextBlock
        {
            Text = text ?? string.Empty,
            Style = (Style)Application.Current.Resources[secondary ? "CaptionTextBlockStyle" : "BodyTextBlockStyle"],
            Foreground = AppThemeVisualResolver.ResolveThemeBrush(
                secondary ? "TextFillColorSecondaryBrush" : "TextFillColorPrimaryBrush"),
            TextTrimming = TextTrimming.CharacterEllipsis,
            TextWrapping = TextWrapping.NoWrap,
            TextAlignment = TextAlignment.Center,
            HorizontalAlignment = stretch ? HorizontalAlignment.Stretch : HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            FontSize = secondary ? 11 : 12,
            MaxLines = 1,
        };
    }

    private void ShowContextMenu(FrameworkElement anchor, CrashDumpEntry entry, Point position)
    {
        var flyout = new MenuFlyout();

        var openItem = new MenuFlyoutItem
        {
            Text = GlassSettingsResources.Get("Crashes_Open"),
            Icon = new FontIcon { Glyph = "\uE8A7" },
        };
        openItem.Click += (_, _) => OpenRequested?.Invoke(this, entry);

        var deleteItem = new MenuFlyoutItem
        {
            Text = GlassSettingsResources.Get("Crashes_Delete"),
            Icon = new FontIcon { Glyph = "\uE74D" },
        };
        deleteItem.Click += (_, _) => DeleteRequested?.Invoke(this, entry);

        flyout.Items.Add(openItem);
        flyout.Items.Add(deleteItem);

        flyout.ShowAt(anchor, new FlyoutShowOptions
        {
            Position = position,
            Placement = FlyoutPlacementMode.Bottom,
        });
    }
}
