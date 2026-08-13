using System.ComponentModel;
using System.Runtime.InteropServices;
using GlassSettings.Services;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Windows.Foundation;

namespace GlassSettings.Controls.Processes;

public sealed partial class ExecutableFilePickerTreeRow : UserControl
{
    internal static Func<ExecutableFilePickerTreeItem, Task>? ExpandCompletedHandler { get; set; }

    internal static Action<ExecutableFilePickerTreeItem, TreeView>? NavigableFolderPointerReleasedHandler { get; set; }

    private const int HoverFadeInMs = 500;
    private const int ExitIgnoreMsAfterOpen = 80;
    private const double HoverHorizontalPadding = 2;
    private const double HoverBorderThickness = 1;
    private const double HoverExtraWidth = 2;

    private static ExecutableFilePickerTreeRow? s_activeTooltipRow;
    private static Popup? s_sharedPopup;
    private static Border? s_sharedPopupBorder;
    private static Grid? s_sharedPopupContent;
    private static Storyboard? s_sharedFadeStoryboard;
    private static FontIcon? s_tooltipGroupIcon;
    private static SymbolIcon? s_tooltipDriveIcon;
    private static Image? s_tooltipFolderIcon;
    private static FontIcon? s_tooltipFolderFallbackIcon;
    private static TextBlock? s_tooltipNameText;
    private static Brush? s_tooltipBackgroundBrush;
    private static TextBlock? s_measureTextBlock;
    private static bool s_hoverTooltipPrewarmed;
    private double GetTreeItemHostHeight() =>
        _hostTreeViewItem?.ActualHeight > 0
            ? _hostTreeViewItem.ActualHeight
            : ExecutableFilePickerLayoutMetrics.TreeRowFluentMinHeight;

    private const int IdcArrow = 32512;

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    private static extern IntPtr LoadCursor(IntPtr hInstance, int lpCursorName);

    [DllImport("user32.dll")]
    private static extern IntPtr SetCursor(IntPtr hCursor);

    private readonly DispatcherQueue? _dispatcherQueue;
    private ExecutableFilePickerTreeItem? _item;
    private bool _trimCallbackRegistered;
    private bool _pointerOverHost;
    private long _popupOpenedTimestampMs;
    private double _cachedFullTextWidth = double.NaN;
    private string? _cachedFullTextWidthKey;
    private TreeViewItem? _hostTreeViewItem;
    private ScrollViewer? _hostScrollViewer;
    private PointerEventHandler? _hostPointerPressedHandler;
    private PointerEventHandler? _hostPointerReleasedHandler;

    public ExecutableFilePickerTreeRow()
    {
        _dispatcherQueue = DispatcherQueue.GetForCurrentThread();
        InitializeComponent();
        DataContextChanged += OnDataContextChanged;
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    private void OnLoaded(object? sender, RoutedEventArgs e)
    {
        if (_item is null)
            BindItem(ResolveTreeItem(DataContext));

        EnsureTrimCallbackRegistered();
        AttachHostTreeViewItemPointerHandlers();
    }

    /// <summary>Один раз при открытии file picker: создаёт общий popup.</summary>
    public static void PrewarmHoverTooltip(XamlRoot xamlRoot)
    {
        if (s_hoverTooltipPrewarmed)
            return;

        EnsureSharedHoverPopup();
        if (s_sharedPopup is not null)
            s_sharedPopup.XamlRoot = xamlRoot;

        s_hoverTooltipPrewarmed = true;
    }

    private void OnUnloaded(object? sender, RoutedEventArgs e)
    {
        if (ReferenceEquals(s_activeTooltipRow, this))
            CloseHoverPopupImmediate();

        ReleaseActiveTooltipRow();
        DetachHostTreeViewItemPointerHandlers();
    }

    private void OnDataContextChanged(FrameworkElement sender, DataContextChangedEventArgs args) =>
        BindItem(ResolveTreeItem(DataContext));

    internal static ExecutableFilePickerTreeItem? ResolveTreeItem(object? dataContext) =>
        dataContext as ExecutableFilePickerTreeItem;

    private void BindItem(ExecutableFilePickerTreeItem? item)
    {
        if (_item is not null)
            _item.PropertyChanged -= OnItemPropertyChanged;

        _item = item;
        if (item is null)
            return;

        item.PropertyChanged += OnItemPropertyChanged;
        NameText.Text = item.Name;
        NameText.FontSize = ExecutableFilePickerLayoutMetrics.FilePickerContentFontSize;
        NameText.FontWeight = item.Kind is ExecutableFilePickerTreeItemKind.QuickAccessGroup
            or ExecutableFilePickerTreeItemKind.ThisComputer
            ? Microsoft.UI.Text.FontWeights.SemiBold
            : Microsoft.UI.Text.FontWeights.Normal;
        NameText.ClearValue(TextBlock.StyleProperty);
        NameText.TextWrapping = TextWrapping.NoWrap;
        NameText.MaxLines = 1;
        NameText.TextTrimming = TextTrimming.CharacterEllipsis;

        ApplyIcon(item);
        InvalidateMeasuredTextWidth();
        ScheduleRefreshHoverState();
    }

    private void EnsureTrimCallbackRegistered()
    {
        if (_trimCallbackRegistered)
            return;

        _trimCallbackRegistered = true;
        NameText.RegisterPropertyChangedCallback(TextBlock.IsTextTrimmedProperty, (_, _) => ScheduleRefreshHoverState());
    }

    private void OnItemPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (sender is not ExecutableFilePickerTreeItem item)
            return;

        if (e.PropertyName == nameof(ExecutableFilePickerTreeItem.Icon))
        {
            ApplyIcon(item);
            ScheduleRefreshHoverState();
            return;
        }

        if (e.PropertyName == nameof(ExecutableFilePickerTreeItem.Name))
        {
            NameText.Text = item.Name;
            InvalidateMeasuredTextWidth();
            ScheduleRefreshHoverState();
        }
    }

    private void ApplyIcon(ExecutableFilePickerTreeItem item)
    {
        GroupIcon.Visibility = Visibility.Collapsed;
        DriveIcon.Visibility = Visibility.Collapsed;
        FolderFallbackIcon.Visibility = Visibility.Collapsed;
        FolderIcon.Visibility = Visibility.Collapsed;
        FolderIcon.Source = null;

        switch (item.Kind)
        {
            case ExecutableFilePickerTreeItemKind.QuickAccessGroup
                or ExecutableFilePickerTreeItemKind.ThisComputer:
                GroupIcon.Visibility = Visibility.Visible;
                break;
            case ExecutableFilePickerTreeItemKind.Drive:
                DriveIcon.Visibility = Visibility.Visible;
                break;
            default:
                if (item.Icon is not null)
                {
                    FolderIcon.Source = item.Icon;
                    FolderIcon.Visibility = Visibility.Visible;
                }
                else
                {
                    FolderFallbackIcon.Visibility = Visibility.Visible;
                }

                break;
        }
    }

    private void AttachHostTreeViewItemPointerHandlers()
    {
        if (_hostTreeViewItem is not null)
            return;

        _hostTreeViewItem = FindHostTreeViewItem();
        if (_hostTreeViewItem is null)
            return;

        _hostTreeViewItem.PointerEntered += HostTreeViewItem_OnPointerEntered;
        _hostTreeViewItem.PointerMoved += HostTreeViewItem_OnPointerMoved;
        _hostTreeViewItem.PointerExited += HostTreeViewItem_OnPointerExited;
        _hostPointerPressedHandler ??= HostTreeViewItem_OnPointerPressed;
        _hostTreeViewItem.AddHandler(
            UIElement.PointerPressedEvent,
            _hostPointerPressedHandler,
            true);
        _hostPointerReleasedHandler ??= HostTreeViewItem_OnPointerReleased;
        _hostTreeViewItem.AddHandler(
            UIElement.PointerReleasedEvent,
            _hostPointerReleasedHandler,
            true);

        _hostScrollViewer = FindAncestor<ScrollViewer>(_hostTreeViewItem);
        if (_hostScrollViewer is not null)
        {
            _hostScrollViewer.ViewChanged += HostScrollViewer_OnViewChanged;
            _hostScrollViewer.PointerMoved += HostScrollViewer_OnPointerMoved;
        }
    }

    private void DetachHostTreeViewItemPointerHandlers()
    {
        if (_hostScrollViewer is not null)
        {
            _hostScrollViewer.ViewChanged -= HostScrollViewer_OnViewChanged;
            _hostScrollViewer.PointerMoved -= HostScrollViewer_OnPointerMoved;
            _hostScrollViewer = null;
        }

        if (_hostTreeViewItem is null)
            return;

        if (_hostPointerPressedHandler is not null)
        {
            _hostTreeViewItem.RemoveHandler(UIElement.PointerPressedEvent, _hostPointerPressedHandler);
        }

        if (_hostPointerReleasedHandler is not null)
        {
            _hostTreeViewItem.RemoveHandler(UIElement.PointerReleasedEvent, _hostPointerReleasedHandler);
        }

        _hostTreeViewItem.PointerEntered -= HostTreeViewItem_OnPointerEntered;
        _hostTreeViewItem.PointerMoved -= HostTreeViewItem_OnPointerMoved;
        _hostTreeViewItem.PointerExited -= HostTreeViewItem_OnPointerExited;
        _hostTreeViewItem = null;
    }

    private void HostScrollViewer_OnViewChanged(object? sender, ScrollViewerViewChangedEventArgs e)
    {
        if (!ReferenceEquals(s_activeTooltipRow, this) || s_sharedPopup?.IsOpen != true)
            return;

        CloseHoverPopupImmediate();
    }

    private void HostScrollViewer_OnPointerMoved(object sender, PointerRoutedEventArgs e)
    {
        if (!ReferenceEquals(s_activeTooltipRow, this) || s_sharedPopup?.IsOpen != true)
            return;

        if (IsPointerInsideHostItem(e))
            return;

        _pointerOverHost = false;
        CloseHoverPopupImmediate();
    }

    private void HostTreeViewItem_OnPointerEntered(object sender, PointerRoutedEventArgs e)
    {
        _pointerOverHost = true;
        TryShowHoverTooltip();
    }

    private void HostTreeViewItem_OnPointerMoved(object sender, PointerRoutedEventArgs e)
    {
        if (_hostTreeViewItem is null)
            return;

        if (IsPointerInsideHostItem(e))
        {
            _pointerOverHost = true;
            TryShowHoverTooltip();
        }
    }

    private void HostTreeViewItem_OnPointerExited(object sender, PointerRoutedEventArgs e)
    {
        if (!ReferenceEquals(s_activeTooltipRow, this))
            return;

        if (IsPointerInsideHostItem(e))
            return;

        var elapsedMs = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() - _popupOpenedTimestampMs;
        if (elapsedMs < ExitIgnoreMsAfterOpen && s_sharedPopup?.IsOpen == true)
            return;

        _pointerOverHost = false;
        CloseHoverPopupImmediate();
    }

    private void TryShowHoverTooltip()
    {
        if (!IsNameTrimmed())
        {
            if (ReferenceEquals(s_activeTooltipRow, this) && s_sharedPopup?.IsOpen == true)
                CloseHoverPopupImmediate();
            return;
        }

        if (ReferenceEquals(s_activeTooltipRow, this) && s_sharedPopup?.IsOpen == true)
            return;

        ClaimActiveTooltipRow();
        OpenHoverPopup();
    }

    private void ClaimActiveTooltipRow()
    {
        if (s_activeTooltipRow is not null && !ReferenceEquals(s_activeTooltipRow, this))
            s_activeTooltipRow.CloseHoverPopupImmediate();

        s_activeTooltipRow = this;
    }

    private void ReleaseActiveTooltipRow()
    {
        if (ReferenceEquals(s_activeTooltipRow, this))
            s_activeTooltipRow = null;
    }

    private void ScheduleRefreshHoverState()
    {
        if (_dispatcherQueue is null)
        {
            RefreshHoverState();
            return;
        }

        _dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, RefreshHoverState);
    }

    private void RefreshHoverState()
    {
        if (!ReferenceEquals(s_activeTooltipRow, this))
            return;

        if (!IsNameTrimmed())
        {
            if (s_sharedPopup?.IsOpen == true)
                CloseHoverPopupImmediate();
            return;
        }

        if (_pointerOverHost && s_sharedPopup?.IsOpen == true)
        {
            UpdateHoverPopupContent();
            PositionHoverPopup();
        }
    }

    private void OpenHoverPopup()
    {
        EnsureSharedHoverPopup();
        UpdateHoverPopupContent();
        PositionHoverPopup();

        if (s_sharedPopup!.IsOpen)
            return;

        if (s_sharedPopupBorder is not null)
            s_sharedPopupBorder.Opacity = 0;

        s_sharedPopup.XamlRoot = XamlRoot;
        s_sharedPopup.IsOpen = true;
    }

    private void CloseHoverPopupImmediate()
    {
        if (s_sharedFadeStoryboard is not null)
        {
            s_sharedFadeStoryboard.Completed -= OnSharedFadeInCompleted;
            s_sharedFadeStoryboard.Stop();
            s_sharedFadeStoryboard = null;
        }

        if (s_sharedPopup is not null)
            s_sharedPopup.IsOpen = false;

        if (s_sharedPopupBorder is not null)
            s_sharedPopupBorder.Opacity = 0;

        if (ReferenceEquals(s_activeTooltipRow, this))
            s_activeTooltipRow = null;
    }

    private bool IsNameTrimmed()
    {
        if (string.IsNullOrEmpty(NameText.Text) || RootPanel.ActualWidth <= 28)
            return false;

        var textBudget = Math.Max(0, RootPanel.ActualWidth - ExecutableFilePickerLayoutMetrics.TreeIconSize - 8);
        if (textBudget <= 0)
            return false;

        if (NameText.ActualWidth > 0 && NameText.IsTextTrimmed)
            return true;

        return GetFullTextWidth() > textBudget;
    }

    private void InvalidateMeasuredTextWidth()
    {
        _cachedFullTextWidthKey = null;
        _cachedFullTextWidth = double.NaN;
    }

    private double GetFullTextWidth()
    {
        var key = _item?.Name ?? string.Empty;
        if (key == _cachedFullTextWidthKey && !double.IsNaN(_cachedFullTextWidth))
            return _cachedFullTextWidth;

        _cachedFullTextWidthKey = key;
        _cachedFullTextWidth = MeasureTextWidth();
        return _cachedFullTextWidth;
    }

    private double MeasureTextWidth()
    {
        s_measureTextBlock ??= new TextBlock
        {
            TextWrapping = TextWrapping.NoWrap,
            FontSize = ExecutableFilePickerLayoutMetrics.FilePickerContentFontSize,
            MaxLines = 1
        };
        s_measureTextBlock.Text = _item?.Name ?? string.Empty;
        s_measureTextBlock.FontWeight = NameText.FontWeight;
        s_measureTextBlock.Measure(new Size(double.PositiveInfinity, ExecutableFilePickerLayoutMetrics.TreeRowTotalHeight));
        return s_measureTextBlock.DesiredSize.Width;
    }

    private bool IsPointerInsideHostItem(PointerRoutedEventArgs e)
    {
        if (_hostTreeViewItem is null)
            return false;

        var pt = e.GetCurrentPoint(_hostTreeViewItem).Position;
        return pt.X >= 0 && pt.Y >= 0
            && pt.X <= _hostTreeViewItem.ActualWidth
            && pt.Y <= _hostTreeViewItem.ActualHeight;
    }

    private static void EnsureSharedHoverPopup()
    {
        if (s_sharedPopup is not null)
            return;

        s_sharedPopupContent = CreateHoverContentGrid();
        s_tooltipBackgroundBrush ??= CreateHoverBackgroundBrush();
        s_sharedPopupBorder = new Border
        {
            Child = s_sharedPopupContent,
            Padding = new Thickness(HoverHorizontalPadding, 0, HoverHorizontalPadding, 0),
            Background = s_tooltipBackgroundBrush,
            BorderBrush = Application.Current.Resources["SurfaceStrokeColorFlyoutBrush"] as Brush,
            BorderThickness = new Thickness(HoverBorderThickness),
            CornerRadius = new CornerRadius(4),
            IsHitTestVisible = false,
            Opacity = 0,
            Height = s_activeTooltipRow?.GetTreeItemHostHeight() ?? ExecutableFilePickerLayoutMetrics.TreeRowFluentMinHeight
        };
        s_sharedPopupContent.Height = s_activeTooltipRow?.GetTreeItemHostHeight()
            ?? ExecutableFilePickerLayoutMetrics.TreeRowFluentMinHeight;
        s_sharedPopupContent.IsHitTestVisible = false;

        s_sharedPopup = new Popup
        {
            Child = s_sharedPopupBorder,
            IsLightDismissEnabled = false,
            ShouldConstrainToRootBounds = false,
            IsHitTestVisible = false
        };
        s_sharedPopup.Opened += OnSharedHoverPopupOpened;
        s_sharedPopup.Closed += OnSharedHoverPopupClosed;
    }

    private static void OnSharedHoverPopupOpened(object? sender, object e)
    {
        if (s_activeTooltipRow is null)
            return;

        s_activeTooltipRow._popupOpenedTimestampMs = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        ForceArrowCursor();
        s_activeTooltipRow.PositionHoverPopup();
        s_activeTooltipRow.StartFadeIn();
    }

    private static void ForceArrowCursor()
    {
        var cursor = LoadCursor(IntPtr.Zero, IdcArrow);
        if (cursor != IntPtr.Zero)
            SetCursor(cursor);
    }

    private static void OnSharedHoverPopupClosed(object? sender, object e)
    {
        if (s_sharedFadeStoryboard is not null)
        {
            s_sharedFadeStoryboard.Completed -= OnSharedFadeInCompleted;
            s_sharedFadeStoryboard.Stop();
            s_sharedFadeStoryboard = null;
        }

        if (s_sharedPopupBorder is not null)
            s_sharedPopupBorder.Opacity = 0;
    }

    private void StartFadeIn()
    {
        if (s_sharedPopupBorder is null || s_sharedFadeStoryboard is not null)
            return;

        s_sharedPopupBorder.Opacity = 0;

        var animation = new DoubleAnimation
        {
            From = 0,
            To = 1,
            Duration = TimeSpan.FromMilliseconds(HoverFadeInMs),
            EnableDependentAnimation = false
        };

        s_sharedFadeStoryboard = new Storyboard();
        Storyboard.SetTarget(animation, s_sharedPopupBorder);
        Storyboard.SetTargetProperty(animation, nameof(UIElement.Opacity));
        s_sharedFadeStoryboard.Children.Add(animation);
        s_sharedFadeStoryboard.Completed += OnSharedFadeInCompleted;
        s_sharedFadeStoryboard.Begin();
    }

    private static void OnSharedFadeInCompleted(object? sender, object e)
    {
        if (ReferenceEquals(sender, s_sharedFadeStoryboard))
            s_sharedFadeStoryboard = null;

        if (s_activeTooltipRow is not null && !s_activeTooltipRow._pointerOverHost)
            s_activeTooltipRow.CloseHoverPopupImmediate();
    }

    private void PositionHoverPopup()
    {
        if (s_sharedPopup is null || s_sharedPopupContent is null || s_sharedPopupBorder is null)
            return;

        if (_hostTreeViewItem is null || XamlRoot?.Content is not UIElement rootElement)
            return;

        var hostHeight = GetTreeItemHostHeight();
        var rowWidth = RootPanel.ActualWidth;
        if (hostHeight <= 0 || rowWidth <= 0)
            return;

        var contentWidth = MeasureHoverContentWidth();
        var innerWidth = Math.Max(rowWidth, contentWidth);
        var popupWidth = innerWidth + HoverHorizontalPadding * 2 + HoverExtraWidth;
        var hostOrigin = _hostTreeViewItem.TransformToVisual(rootElement).TransformPoint(new Point(0, 0));
        var contentOrigin = RootPanel.TransformToVisual(rootElement).TransformPoint(new Point(0, 0));
        var horizontalOffset = contentOrigin.X - HoverHorizontalPadding - HoverBorderThickness;
        var verticalOffset = hostOrigin.Y - HoverBorderThickness;

        s_sharedPopup.XamlRoot = XamlRoot;
        s_sharedPopup.HorizontalOffset = horizontalOffset;
        s_sharedPopup.VerticalOffset = verticalOffset;
        s_sharedPopupBorder.Width = popupWidth;
        s_sharedPopupBorder.MinWidth = popupWidth;
        s_sharedPopupBorder.Height = hostHeight;
        s_sharedPopupContent.Width = innerWidth + HoverExtraWidth;
        s_sharedPopupContent.MinWidth = innerWidth + HoverExtraWidth;
        s_sharedPopupContent.Height = hostHeight;
    }

    private double MeasureHoverContentWidth()
    {
        const double iconColumn = ExecutableFilePickerLayoutMetrics.TreeIconSize;
        const double columnGap = 8;
        return iconColumn + columnGap + GetFullTextWidth() + HoverExtraWidth;
    }

    private void UpdateHoverPopupContent()
    {
        if (s_tooltipNameText is null)
            return;

        ApplyTooltipIconFromRow();
        s_tooltipNameText.Text = _item?.Name ?? string.Empty;
        s_tooltipNameText.Style = NameText.Style;
    }

    private void ApplyTooltipIconFromRow()
    {
        if (s_tooltipGroupIcon is null || s_tooltipDriveIcon is null
            || s_tooltipFolderIcon is null || s_tooltipFolderFallbackIcon is null)
            return;

        s_tooltipGroupIcon.Visibility = Visibility.Collapsed;
        s_tooltipDriveIcon.Visibility = Visibility.Collapsed;
        s_tooltipFolderIcon.Visibility = Visibility.Collapsed;
        s_tooltipFolderFallbackIcon.Visibility = Visibility.Collapsed;
        s_tooltipFolderIcon.Source = null;

        if (GroupIcon.Visibility == Visibility.Visible)
        {
            s_tooltipGroupIcon.Glyph = GroupIcon.Glyph;
            s_tooltipGroupIcon.Foreground = GroupIcon.Foreground;
            s_tooltipGroupIcon.Visibility = Visibility.Visible;
            return;
        }

        if (DriveIcon.Visibility == Visibility.Visible)
        {
            s_tooltipDriveIcon.Symbol = DriveIcon.Symbol;
            s_tooltipDriveIcon.Visibility = Visibility.Visible;
            return;
        }

        if (FolderIcon.Visibility == Visibility.Visible && FolderIcon.Source is not null)
        {
            s_tooltipFolderIcon.Source = FolderIcon.Source;
            s_tooltipFolderIcon.Visibility = Visibility.Visible;
            return;
        }

        s_tooltipFolderFallbackIcon.Glyph = FolderFallbackIcon.Glyph;
        s_tooltipFolderFallbackIcon.Foreground = FolderFallbackIcon.Foreground;
        s_tooltipFolderFallbackIcon.Visibility = Visibility.Visible;
    }

    private static Grid CreateHoverContentGrid()
    {
        var iconHost = new Grid
        {
            Width = ExecutableFilePickerLayoutMetrics.TreeIconSize,
            Height = ExecutableFilePickerLayoutMetrics.TreeIconSize,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center
        };

        s_tooltipGroupIcon = new FontIcon
        {
            Width = ExecutableFilePickerLayoutMetrics.TreeIconSize,
            Height = ExecutableFilePickerLayoutMetrics.TreeIconSize,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            FontSize = 16,
            Visibility = Visibility.Collapsed
        };
        s_tooltipDriveIcon = new SymbolIcon
        {
            Width = ExecutableFilePickerLayoutMetrics.TreeIconSize,
            Height = ExecutableFilePickerLayoutMetrics.TreeIconSize,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            Visibility = Visibility.Collapsed
        };
        s_tooltipFolderIcon = new Image
        {
            Width = ExecutableFilePickerLayoutMetrics.TreeIconSize,
            Height = ExecutableFilePickerLayoutMetrics.TreeIconSize,
            Visibility = Visibility.Collapsed
        };
        s_tooltipFolderFallbackIcon = new FontIcon
        {
            Width = ExecutableFilePickerLayoutMetrics.TreeIconSize,
            Height = ExecutableFilePickerLayoutMetrics.TreeIconSize,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            FontSize = 16,
            Visibility = Visibility.Collapsed
        };

        iconHost.Children.Add(s_tooltipGroupIcon);
        iconHost.Children.Add(s_tooltipDriveIcon);
        iconHost.Children.Add(s_tooltipFolderIcon);
        iconHost.Children.Add(s_tooltipFolderFallbackIcon);

        s_tooltipNameText = new TextBlock
        {
            FontSize = ExecutableFilePickerLayoutMetrics.FilePickerContentFontSize,
            MaxLines = 1,
            TextWrapping = TextWrapping.NoWrap,
            VerticalAlignment = VerticalAlignment.Center
        };

        var grid = new Grid
        {
            ColumnSpacing = 8,
            VerticalAlignment = VerticalAlignment.Center,
            IsHitTestVisible = false,
            MinHeight = ExecutableFilePickerLayoutMetrics.TreeRowFluentMinHeight
        };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(ExecutableFilePickerLayoutMetrics.TreeIconSize) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star), MinWidth = 0 });
        Grid.SetColumn(iconHost, 0);
        Grid.SetColumn(s_tooltipNameText, 1);
        grid.Children.Add(iconHost);
        grid.Children.Add(s_tooltipNameText);
        return grid;
    }

    private static Brush CreateHoverBackgroundBrush()
    {
        if (Application.Current.Resources["AcrylicInAppFillColorDefaultBrush"] is AcrylicBrush template)
        {
            return new AcrylicBrush
            {
                TintColor = template.TintColor,
                TintOpacity = 1.0,
                TintLuminosityOpacity = 1.0,
                FallbackColor = template.FallbackColor
            };
        }

        if (Application.Current.Resources["LayerFillColorDefaultBrush"] is Brush layer)
            return layer;

        return new SolidColorBrush(Microsoft.UI.Colors.White);
    }

    private TreeViewItem? FindHostTreeViewItem()
    {
        var node = VisualTreeHelper.GetParent(this);
        while (node is not null)
        {
            if (node is TreeViewItem item)
                return item;

            node = VisualTreeHelper.GetParent(node);
        }

        return null;
    }

    private static T? FindAncestor<T>(DependencyObject? node) where T : DependencyObject
    {
        while (node is not null)
        {
            if (node is T match)
                return match;

            node = VisualTreeHelper.GetParent(node);
        }

        return null;
    }

    private void HostTreeViewItem_OnPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        if (_item is null || sender is not UIElement element)
            return;

        if (!IsPrimaryPointerPress(element, e))
            return;

        var item = _item;
        var canExpand = CanExpandTreeItem(item);
        var pressedAtTick = Environment.TickCount64;
        var wasExpanded = item.IsExpanded;

        ExecutableFilePickerTreeClickHelper.RegisterPointerPressed(
            item,
            e.Pointer.PointerId,
            pressedAtTick,
            wasExpanded,
            wasExpandedAtFirstClick =>
            {
                if (!canExpand)
                    return;

                var targetExpanded = !wasExpandedAtFirstClick;
                _dispatcherQueue?.TryEnqueue(
                    DispatcherQueuePriority.Low,
                    () => SetTreeExpanded(item, targetExpanded));
            });
    }

    private void SetTreeExpanded(ExecutableFilePickerTreeItem item, bool expanded)
    {
        if (!CanExpandTreeItem(item))
            return;

        item.IsExpanded = expanded;
        if (_hostTreeViewItem is not null)
            _hostTreeViewItem.IsExpanded = expanded;

        if (!expanded && ExpandCompletedHandler is { } handler)
            _ = handler(item);
    }

    private void HostTreeViewItem_OnPointerReleased(object sender, PointerRoutedEventArgs e)
    {
        if (_item is null || !_item.IsNavigableFolder || _item.FullPath is null)
            return;

        if (sender is not UIElement element || !IsPrimaryPointerPress(element, e))
            return;

        if (ExecutableFilePickerTreeClickHelper.TryConsumePointerReleased(_item))
            return;

        var tree = FindHostTreeView();
        if (tree is null)
            return;

        NavigableFolderPointerReleasedHandler?.Invoke(_item, tree);
    }

    private static bool IsPrimaryPointerPress(UIElement element, PointerRoutedEventArgs e)
    {
        var point = e.GetCurrentPoint(element);
        if (point.PointerDeviceType != PointerDeviceType.Mouse)
            return true;

        var kind = point.Properties.PointerUpdateKind;
        return kind is not PointerUpdateKind.RightButtonPressed
            and not PointerUpdateKind.MiddleButtonPressed;
    }

    private static bool CanExpandTreeItem(ExecutableFilePickerTreeItem item) =>
        item.Kind is ExecutableFilePickerTreeItemKind.QuickAccessGroup
            or ExecutableFilePickerTreeItemKind.ThisComputer
            or ExecutableFilePickerTreeItemKind.Drive
            or ExecutableFilePickerTreeItemKind.Folder
        && (item.HasUnloadedChildren || item.Children.Count > 0);

    private TreeView? FindHostTreeView()
    {
        DependencyObject? node = _hostTreeViewItem;
        node ??= this;
        while (node is not null)
        {
            if (node is TreeView tree)
                return tree;

            node = VisualTreeHelper.GetParent(node);
        }

        return null;
    }
}
