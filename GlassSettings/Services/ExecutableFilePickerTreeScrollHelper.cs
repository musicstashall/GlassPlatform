using GlassSettings.Controls.Processes;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;

namespace GlassSettings.Services;

internal enum ExpandScrollStatus
{
    Scrolled,
    AlreadyAtTarget,
    ScrollPending,
    NoLastChild,
    NoScrollViewer,
    ContainersNotReady,
    ExtentNotReady,
    ModelNotReady
}

/// <summary>Прокрутка ListView file picker и bring-into-view TreeView (Gallery-паттерн).</summary>
internal static class ExecutableFilePickerTreeScrollHelper
{
    private static readonly HashSet<TreeViewList> s_extentSyncAttached = [];
    private static readonly Dictionary<TreeView, int> s_expandScrollGeneration = [];
    private static readonly Dictionary<TreeView, ExecutableFilePickerTreeItem> s_expandScrollParent = [];
    private static readonly Dictionary<TreeView, double> s_treeBottomInset = [];

    public static void ApplyScrollInsets(ListView listView, double bottomInset)
    {
        listView.Padding = new Thickness(0);
        listView.ClearValue(ScrollViewer.PaddingProperty);

        var scrollViewer = FindDescendant<ScrollViewer>(listView);
        if (scrollViewer is null)
            return;

        scrollViewer.Padding = new Thickness(0, 0, 0, bottomInset);
    }

    public static void ApplyTreeScrollInsets(TreeView treeView, double bottomInset)
    {
        s_treeBottomInset[treeView] = bottomInset;
        treeView.ClearValue(ScrollViewer.PaddingProperty);
        var current = treeView.Padding;
        treeView.Padding = new Thickness(current.Left, current.Top, current.Right, 0);

        var scrollViewer = FindDescendant<ScrollViewer>(treeView);
        if (scrollViewer is null)
            return;

        scrollViewer.Padding = new Thickness(0, 0, 0, bottomInset);
    }

    public static void ApplyTreeScrollInsets(TreeView treeView)
    {
        if (!s_treeBottomInset.TryGetValue(treeView, out var bottomInset))
            bottomInset = ExecutableFilePickerLayoutMetrics.GetTreeScrollBottomInset();

        ApplyTreeScrollInsets(treeView, bottomInset);
    }

    public static double GetTreeViewportHeight(TreeView treeView)
    {
        var scrollViewer = FindDescendant<ScrollViewer>(treeView);
        if (scrollViewer is not null && scrollViewer.ViewportHeight > 0)
            return scrollViewer.ViewportHeight;

        return treeView.ActualHeight > 0 ? treeView.ActualHeight : 0;
    }

    public static void ConfigureTreeView(TreeView treeView)
    {
        AttachExtentSync(treeView);
        ApplyTreeScrollInsets(treeView);

        void apply()
        {
            ApplyFlatPanelTopAlignment(treeView);
            ApplyTreeScrollInsets(treeView);
        }

        if (treeView.IsLoaded)
            apply();
        else
            treeView.Loaded += (_, _) => apply();
    }

    /// <summary>
    /// Дефицит extent: сумма высот flat-контейнеров минус фактическая высота ItemsStackPanel.
    /// </summary>
    public static double MeasureExtentDeficit(TreeView treeView)
    {
        var panel = FindDescendant<ItemsStackPanel>(treeView);
        if (panel is null)
            return 0;

        var sumFlat = SumFlatContainerHeights(panel);
        var extentTarget = sumFlat + ExecutableFilePickerLayoutMetrics.GetScrollExtentBottomLift();
        return extentTarget - panel.ActualHeight;
    }

    public static int CountFlatPanelItems(TreeView treeView)
    {
        var panel = FindDescendant<ItemsStackPanel>(treeView);
        if (panel is null)
            return 0;

        return VisualTreeHelper.GetChildrenCount(panel);
    }

    public static void BringItemIntoView(TreeView treeView, ExecutableFilePickerTreeItem item)
    {
        if (FindDescendantByName(treeView, "ListControl") is TreeViewList treeViewList)
        {
            treeViewList.ScrollIntoView(item);
            return;
        }

        var container = FindTreeViewItem(treeView, item);
        container?.StartBringIntoView(new BringIntoViewOptions
        {
            AnimationDesired = false,
            VerticalAlignmentRatio = 0.5
        });
    }

    public static void RefreshScrollExtent(TreeView treeView)
    {
        ApplyFlatPanelTopAlignment(treeView);

        var panel = FindDescendant<ItemsStackPanel>(treeView);
        panel?.InvalidateMeasure();
        panel?.InvalidateArrange();

        var scrollViewer = FindDescendant<ScrollViewer>(treeView);
        scrollViewer?.InvalidateMeasure();
        scrollViewer?.InvalidateArrange();

        treeView.InvalidateMeasure();
        treeView.UpdateLayout();
    }

    public static bool TryCoerceScrollExtent(TreeView treeView)
    {
        var panel = FindDescendant<ItemsStackPanel>(treeView);
        if (panel is null)
            return false;

        var sumFlat = SumFlatContainerHeights(panel);
        if (sumFlat <= 0)
            return false;

        var extentTarget = sumFlat + ExecutableFilePickerLayoutMetrics.GetScrollExtentBottomLift();
        var deficitBefore = extentTarget - panel.ActualHeight;
        if (Math.Abs(deficitBefore) <= 1)
        {
            panel.ClearValue(FrameworkElement.MinHeightProperty);
            return true;
        }

        panel.MinHeight = extentTarget;
        RefreshListLayout(treeView);

        var deficitAfter = extentTarget - panel.ActualHeight;
        if (Math.Abs(deficitAfter) <= 1)
            return true;

        panel.MinHeight = extentTarget;
        RefreshListLayout(treeView);
        return Math.Abs(extentTarget - panel.ActualHeight) <= 1;
    }

    public static bool TryCoerceScrollExtentFromModel(
        TreeView treeView,
        IEnumerable<ExecutableFilePickerTreeItem> roots)
    {
        var panel = FindDescendant<ItemsStackPanel>(treeView);
        if (panel is null)
            return false;

        var rowHeight = ExecutableFilePickerLayoutMetrics.TreeRowTotalHeight;
        var extentTarget = ExecutableFilePickerTreeHelper.CountVisibleExpandedNodes(roots) * rowHeight
            + ExecutableFilePickerLayoutMetrics.GetScrollExtentBottomLift();

        if (Math.Abs(extentTarget - panel.ActualHeight) <= 1)
        {
            panel.ClearValue(FrameworkElement.MinHeightProperty);
            return true;
        }

        panel.MinHeight = extentTarget;
        RefreshListLayout(treeView);
        return Math.Abs(extentTarget - panel.ActualHeight) <= 1;
    }

    public static async Task<ExpandScrollStatus> RunExpandBranchScrollAsync(
        TreeView treeView,
        IEnumerable<ExecutableFilePickerTreeItem>? roots,
        ExecutableFilePickerTreeItem expandedParent,
        DispatcherQueue dispatcherQueue,
        string? currentPath = null)
    {
        var generation = BeginExpandScroll(treeView, expandedParent);

        await WaitForExpandedBranchReadyAsync(
            treeView,
            roots,
            expandedParent,
            dispatcherQueue,
            generation).ConfigureAwait(true);

        if (!IsExpandScrollCurrent(treeView, generation))
            return ExpandScrollStatus.AlreadyAtTarget;

        var extentDeficit = MeasureExtentDeficit(treeView);
        if (extentDeficit > 1)
            TryCoerceScrollExtent(treeView);
        else if (extentDeficit < -1)
            ReleaseOversizedFlatPanel(treeView);

        var focusChildForScroll = ExecutableFilePickerTreeHelper.ResolveExpandScrollFocusChild(
            expandedParent,
            currentPath);
        if (roots is not null)
        {
            TryCoerceScrollExtentFromModel(treeView, roots);
            if (Math.Abs(GetModelFlatDelta(treeView, roots)) > 0)
                ReleaseOversizedFlatPanel(treeView);
        }
        RequestFlatBranchMaterialization(treeView, expandedParent, focusChildForScroll);

        ExpandScrollStatus status;
        try
        {
            status = TryScrollExpandedBranchIntoView(
                treeView,
                expandedParent,
                attempt: 0,
                roots,
                currentPath);
        }
        catch (Exception)
        {
            return ExpandScrollStatus.ContainersNotReady;
        }

        if (status is ExpandScrollStatus.ScrollPending or ExpandScrollStatus.ExtentNotReady)
            status = await AwaitScrollViewerSettledAsync(
                treeView,
                expandedParent,
                roots,
                dispatcherQueue,
                generation,
                status,
                currentPath).ConfigureAwait(true);
        return status;
    }

    public static void SyncFlatListAfterExpand(
        TreeView treeView,
        IEnumerable<ExecutableFilePickerTreeItem> roots,
        ExecutableFilePickerTreeItem expandedParent,
        DispatcherQueue dispatcherQueue) =>
        _ = RunExpandBranchScrollAsync(treeView, roots, expandedParent, dispatcherQueue);

    public static void ScheduleExpandedBranchScroll(
        TreeView treeView,
        ExecutableFilePickerTreeItem expandedParent,
        DispatcherQueue dispatcherQueue) =>
        _ = RunExpandBranchScrollAsync(treeView, roots: null, expandedParent, dispatcherQueue);

    private static bool IsBranchContainersReady(
        TreeView treeView,
        ExecutableFilePickerTreeItem expandedParent,
        ExecutableFilePickerTreeItem? lastChild)
    {
        if (!ExecutableFilePickerTreeHelper.IsBranchModelReady(expandedParent))
            return false;

        if (lastChild is null)
            return true;

        var parentItem = TryFindFlatTreeViewItem(treeView, expandedParent);
        var lastChildItem = TryFindFlatTreeViewItem(treeView, lastChild);
        return parentItem is not null && parentItem.ActualHeight > 0
            && lastChildItem is not null && lastChildItem.ActualHeight > 0;
    }

    private static void RequestFlatBranchMaterialization(
        TreeView treeView,
        ExecutableFilePickerTreeItem expandedParent,
        ExecutableFilePickerTreeItem? lastChild)
    {
        if (FindDescendantByName(treeView, "ListControl") is not TreeViewList list)
            return;

        list.ScrollIntoView(expandedParent);
        if (lastChild is not null)
            list.ScrollIntoView(lastChild);
    }

    private static async Task WaitForExpandedBranchReadyAsync(
        TreeView treeView,
        IEnumerable<ExecutableFilePickerTreeItem>? roots,
        ExecutableFilePickerTreeItem expandedParent,
        DispatcherQueue dispatcherQueue,
        int generation)
    {
        _ = roots;
        var lastChild = ExecutableFilePickerTreeHelper.FindLastDirectChild(expandedParent);
        if (ExecutableFilePickerTreeHelper.IsBranchModelReady(expandedParent))
        {            return;
        }
        RequestFlatBranchMaterialization(treeView, expandedParent, lastChild);

        var listControl = FindDescendantByName(treeView, "ListControl") as TreeViewList;
        if (listControl is null)
        {
            await WaitForUiLayoutPassAsync(dispatcherQueue).ConfigureAwait(true);            return;
        }

        var tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        void TryComplete(string reason)
        {
            if (!IsExpandScrollCurrent(treeView, generation))
            {
                Detach();
                tcs.TrySetCanceled();
                return;
            }

            if (!ExecutableFilePickerTreeHelper.IsBranchModelReady(expandedParent)
                && !IsBranchContainersReady(treeView, expandedParent, lastChild))
            {
                RequestFlatBranchMaterialization(treeView, expandedParent, lastChild);
                return;
            }            Detach();
            tcs.TrySetResult();
        }

        void Detach() => listControl.ContainerContentChanging -= OnContainerContentChanging;

        void OnContainerContentChanging(ListViewBase sender, ContainerContentChangingEventArgs args)
        {
            if (args.InRecycleQueue)
                return;

            TryComplete("container_content_changing");
        }

        listControl.ContainerContentChanging += OnContainerContentChanging;

        try
        {
            dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, () => TryComplete("post_expand"));
            await tcs.Task.WaitAsync(TimeSpan.FromSeconds(2)).ConfigureAwait(true);
        }
        catch (TimeoutException)
        {
            // таймаут ожидания контейнеров
        }
        catch (TaskCanceledException)
        {
            // stale generation — scroll aborted
        }
        finally
        {
            Detach();
        }
    }

    private static async Task WaitForUiLayoutPassAsync(DispatcherQueue dispatcherQueue)
    {
        var tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, () => tcs.TrySetResult());
        await tcs.Task.ConfigureAwait(true);
    }

    private static async Task<ExpandScrollStatus> AwaitScrollViewerSettledAsync(
        TreeView treeView,
        ExecutableFilePickerTreeItem expandedParent,
        IEnumerable<ExecutableFilePickerTreeItem>? roots,
        DispatcherQueue dispatcherQueue,
        int generation,
        ExpandScrollStatus priorStatus,
        string? currentPath = null)
    {
        var scrollViewer = FindDescendant<ScrollViewer>(treeView);
        if (scrollViewer is null)
            return priorStatus;

        var tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        void OnViewChanged(object? sender, ScrollViewerViewChangedEventArgs e)
        {
            if (e.IsIntermediate)
                return;

            scrollViewer.ViewChanged -= OnViewChanged;
            tcs.TrySetResult();
        }

        scrollViewer.ViewChanged += OnViewChanged;
        dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, () =>
        {
            scrollViewer.ViewChanged -= OnViewChanged;
            tcs.TrySetResult();
        });

        await tcs.Task.ConfigureAwait(true);

        if (!IsExpandScrollCurrent(treeView, generation))
            return ExpandScrollStatus.AlreadyAtTarget;

        var extentDeficit = MeasureExtentDeficit(treeView);
        if (extentDeficit > 1)
            TryCoerceScrollExtent(treeView);
        else if (extentDeficit < -1)
            ReleaseOversizedFlatPanel(treeView);

        if (roots is not null)
            TryCoerceScrollExtentFromModel(treeView, roots);

        return TryScrollExpandedBranchIntoView(treeView, expandedParent, attempt: 1, roots, currentPath);
    }

    private static bool TryResolveBranchContainers(
        TreeView treeView,
        ExecutableFilePickerTreeItem expandedParent,
        ExecutableFilePickerTreeItem lastChild,
        out TreeViewItem? parentItem,
        out TreeViewItem? lastChildItem)
    {
        parentItem = TryFindFlatTreeViewItem(treeView, expandedParent);
        lastChildItem = TryFindFlatTreeViewItem(treeView, lastChild);
        return parentItem is not null && lastChildItem is not null;
    }

    private static FrameworkElement GetScrollAnchor(TreeViewItem item) =>
        (FrameworkElement?)FindTreeRow(item) ?? item;

    private static double GetScrollAnchorTopInViewer(ScrollViewer scrollViewer, TreeViewItem item) =>
        GetScrollAnchor(item).TransformToVisual(scrollViewer).TransformPoint(new Point(0, 0)).Y;

    private static bool IsDirectFlatChild(TreeViewItem item, ItemsStackPanel panel) =>
        ReferenceEquals(VisualTreeHelper.GetParent(item), panel);

    private static int GetModelFlatDelta(
        TreeView treeView,
        IEnumerable<ExecutableFilePickerTreeItem>? roots) =>
        roots is null
            ? 0
            : ExecutableFilePickerTreeHelper.CountVisibleExpandedNodes(roots) - CountFlatPanelItems(treeView);

    private static bool TryMeasureFlatLastBottomInViewer(TreeView treeView, out double bottomInViewer)
    {
        bottomInViewer = double.NaN;

        var panel = FindDescendant<ItemsStackPanel>(treeView);
        var scrollViewer = FindDescendant<ScrollViewer>(treeView);
        if (panel is null || scrollViewer is null || scrollViewer.ViewportHeight <= 0)
            return false;

        var count = VisualTreeHelper.GetChildrenCount(panel);
        if (count == 0)
        {
            bottomInViewer = 0;
            return true;
        }

        if (VisualTreeHelper.GetChild(panel, count - 1) is not FrameworkElement lastFlat)
            return false;

        bottomInViewer = lastFlat
            .TransformToVisual(scrollViewer)
            .TransformPoint(new Point(0, lastFlat.ActualHeight)).Y;
        return true;
    }

    private static bool IsFlatTransformStable(TreeView treeView)
    {
        if (!TryMeasureFlatLastBottomInViewer(treeView, out var bottomInViewer))
            return false;

        if (double.IsNaN(bottomInViewer))
            return false;

        var scrollViewer = FindDescendant<ScrollViewer>(treeView);
        if (scrollViewer is null || scrollViewer.ViewportHeight <= 0)
            return false;

        var viewportHeight = scrollViewer.ViewportHeight;
        if (bottomInViewer < -viewportHeight)
            return false;

        var maxReasonableBottom = viewportHeight + scrollViewer.ScrollableHeight + viewportHeight;
        return bottomInViewer <= maxReasonableBottom;
    }

    private static int BeginExpandScroll(TreeView treeView, ExecutableFilePickerTreeItem expandedParent)
    {
        if (!s_expandScrollGeneration.TryGetValue(treeView, out var generation))
            generation = 0;

        generation++;
        s_expandScrollGeneration[treeView] = generation;
        s_expandScrollParent[treeView] = expandedParent;
        return generation;
    }

    /// <summary>
    /// Y2 &lt;= offset+H−h → none; иначе span&gt;H → parentTop; иначе childBottom.
    /// parentTop — только когда ветка выше viewport; иначе минимальная прокрутка к последнему ребёнку.
    /// </summary>
    private static (double? TargetOffset, string Strategy, double BranchSpan) ComputeExpandScrollTarget(
        double y1ParentTop,
        double y2LastChildTop,
        double parentTopInViewer,
        double currentOffset,
        double viewportHeight,
        double rowHeight,
        double scrollableMax,
        bool preferPathChildScroll = false)
    {
        var branchSpan = y2LastChildTop - y1ParentTop;
        var scrollThreshold = currentOffset + viewportHeight - rowHeight;

        if (y2LastChildTop <= scrollThreshold)
            return (null, "none", branchSpan);

        if (!preferPathChildScroll && parentTopInViewer > 0.5 && branchSpan > viewportHeight)
        {
            return (
                Math.Clamp(currentOffset + parentTopInViewer, 0, scrollableMax),
                "parentTop",
                branchSpan);
        }

        var rawTarget = y2LastChildTop - viewportHeight + rowHeight;
        return (
            Math.Clamp(rawTarget, 0, scrollableMax),
            preferPathChildScroll ? "pathChildBottom" : "childBottom",
            branchSpan);
    }

    private static bool IsOffsetAtTarget(ScrollViewer scrollViewer, double targetOffset) =>
        Math.Abs(scrollViewer.VerticalOffset - targetOffset) <= 0.5;

    private static bool IsExpandScrollCurrent(TreeView treeView, int generation) =>
        s_expandScrollGeneration.TryGetValue(treeView, out var current) && current == generation;

    /// <summary>После expand: parentTop / childBottom / none по формуле Y1,Y2,H,h.</summary>
    public static ExpandScrollStatus TryScrollExpandedBranchIntoView(
        TreeView treeView,
        ExecutableFilePickerTreeItem expandedParent,
        int attempt = 0,
        IEnumerable<ExecutableFilePickerTreeItem>? roots = null,
        string? currentPath = null)
    {
        var lastChild = ExecutableFilePickerTreeHelper.ResolveExpandScrollFocusChild(expandedParent, currentPath);
        if (lastChild is null)
        {            return ExpandScrollStatus.NoLastChild;
        }

        var scrollViewer = FindDescendant<ScrollViewer>(treeView);
        if (scrollViewer is null || scrollViewer.ViewportHeight <= 0)
        {            return ExpandScrollStatus.NoScrollViewer;
        }

        if (!TryMeasureFlatBranchBounds(
                treeView,
                expandedParent,
                lastChild,
                roots,
                out var y1ParentTop,
                out var lastChildBottom,
                out var flatChildCount,
                out var boundsEstimated))
        {            return ExpandScrollStatus.ContainersNotReady;
        }

        var rowHeight = ExecutableFilePickerLayoutMetrics.TreeRowTotalHeight;
        var viewportHeight = scrollViewer.ViewportHeight;
        var outerTreeHeight = treeView.ActualHeight;
        var currentOffset = scrollViewer.VerticalOffset;
        var scrollableMax = scrollViewer.ScrollableHeight;
        var y2LastChildTop = lastChildBottom - rowHeight;
        var scrollThreshold = currentOffset + viewportHeight - rowHeight;
        var modelFlatDelta = GetModelFlatDelta(treeView, roots);
        TryMeasureFlatLastBottomInViewer(treeView, out var flatLastBottomInViewer);
        var topInViewerForLog = y1ParentTop - currentOffset;
        var preferPathChildScroll = !string.IsNullOrWhiteSpace(currentPath)
            && ExecutableFilePickerTreeHelper.IsExpandRelatedToPath(expandedParent, currentPath)
            && ExecutableFilePickerTreeHelper.FindDirectPathChild(expandedParent, currentPath) is not null;

        if (y2LastChildTop < y1ParentTop)
        {            return ExpandScrollStatus.ContainersNotReady;
        }

        var (targetOffset, strategy, branchSpan) = ComputeExpandScrollTarget(
            y1ParentTop,
            y2LastChildTop,
            topInViewerForLog,
            currentOffset,
            viewportHeight,
            rowHeight,
            scrollableMax,
            preferPathChildScroll);

        if (targetOffset is null)
        {            return ExpandScrollStatus.AlreadyAtTarget;
        }

        var rawTarget = targetOffset.Value;
        if (rawTarget > scrollableMax + 1
            && roots is not null
            && TryCoerceScrollExtentFromModel(treeView, roots))
            scrollableMax = scrollViewer.ScrollableHeight;

        if (rawTarget > scrollableMax + 1)
        {            return ExpandScrollStatus.ExtentNotReady;
        }

        if (IsOffsetAtTarget(scrollViewer, targetOffset.Value))
        {            return ExpandScrollStatus.AlreadyAtTarget;
        }

        scrollViewer.ChangeView(null, targetOffset.Value, null, disableAnimation: true);
        scrollViewer.UpdateLayout();
        var appliedOffset = scrollViewer.VerticalOffset;
        var status = IsOffsetAtTarget(scrollViewer, targetOffset.Value)
            ? ExpandScrollStatus.AlreadyAtTarget
            : ExpandScrollStatus.ScrollPending;        return status;
    }

    private static bool TryMeasureBranchBoundsFromModel(
        IEnumerable<ExecutableFilePickerTreeItem> roots,
        ExecutableFilePickerTreeItem expandedParent,
        ExecutableFilePickerTreeItem lastChild,
        out double parentTop,
        out double lastChildBottom)
    {
        parentTop = 0;
        lastChildBottom = 0;

        if (!ExecutableFilePickerTreeHelper.TryGetVisibleFlatIndex(roots, expandedParent, out var parentIndex)
            || !ExecutableFilePickerTreeHelper.TryGetVisibleFlatIndex(roots, lastChild, out var lastChildIndex)
            || lastChildIndex < parentIndex)
            return false;

        var rowHeight = ExecutableFilePickerLayoutMetrics.TreeRowTotalHeight;
        parentTop = parentIndex * rowHeight;
        lastChildBottom = (lastChildIndex + 1) * rowHeight;
        return parentTop <= lastChildBottom + 0.5;
    }

    private static bool TryMeasureFlatBranchBounds(
        TreeView treeView,
        ExecutableFilePickerTreeItem expandedParent,
        ExecutableFilePickerTreeItem lastChild,
        IEnumerable<ExecutableFilePickerTreeItem>? roots,
        out double parentTop,
        out double lastChildBottom,
        out int flatChildCount,
        out bool boundsEstimated)
    {
        parentTop = 0;
        lastChildBottom = 0;
        boundsEstimated = false;
        flatChildCount = CountFlatPanelItems(treeView);

        if (roots is not null
            && TryMeasureBranchBoundsFromModel(
                roots,
                expandedParent,
                lastChild,
                out parentTop,
                out lastChildBottom))
        {
            boundsEstimated = true;
            return true;
        }

        var parentItem = TryFindFlatTreeViewItem(treeView, expandedParent);
        var lastChildItem = TryFindFlatTreeViewItem(treeView, lastChild);
        if (parentItem is null || parentItem.ActualHeight <= 0
            || lastChildItem is null || lastChildItem.ActualHeight <= 0)
            return false;

        var scrollViewer = FindDescendant<ScrollViewer>(treeView);
        if (scrollViewer is null)
            return false;

        try
        {
            parentTop = GetContentTop(scrollViewer, GetScrollAnchor(parentItem));
            lastChildBottom = GetContentBottom(scrollViewer, GetScrollAnchor(lastChildItem));
        }
        catch
        {
            return false;
        }

        return parentTop <= lastChildBottom + 0.5;
    }

    public static void ScrollExpandedBranchIntoView(TreeView treeView, ExecutableFilePickerTreeItem expandedParent) =>
        TryScrollExpandedBranchIntoView(treeView, expandedParent);

    public static void ScheduleScrollExtentRefresh(TreeView treeView, DispatcherQueue dispatcherQueue)
    {
        RefreshScrollExtent(treeView);
        dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, () => RefreshScrollExtent(treeView));
    }

    public static void ClearTreeScrollOverrides(TreeView treeView) =>
        ApplyTreeScrollInsets(treeView);

    private static void AttachExtentSync(TreeView treeView)
    {
        if (FindDescendantByName(treeView, "ListControl") is not TreeViewList listControl)
            return;

        if (!s_extentSyncAttached.Add(listControl))
            return;

        listControl.ContainerContentChanging += (_, _) =>
            treeView.DispatcherQueue.TryEnqueue(() =>
            {
                var deficit = MeasureExtentDeficit(treeView);
                if (deficit > 1)
                    TryCoerceScrollExtent(treeView);
                else if (deficit < -1)
                    ReleaseOversizedFlatPanel(treeView);
            });
    }

    private static void RefreshListLayout(TreeView treeView)
    {
        if (FindDescendantByName(treeView, "ListControl") is FrameworkElement listControl)
        {
            listControl.InvalidateMeasure();
            listControl.InvalidateArrange();
            listControl.UpdateLayout();
        }

        var panel = FindDescendant<ItemsStackPanel>(treeView);
        panel?.InvalidateMeasure();
        panel?.InvalidateArrange();
        panel?.UpdateLayout();

        var scrollViewer = FindDescendant<ScrollViewer>(treeView);
        scrollViewer?.InvalidateMeasure();
        scrollViewer?.UpdateLayout();
    }

    private static void ApplyFlatPanelTopAlignment(TreeView treeView)
    {
        var panel = FindDescendant<ItemsStackPanel>(treeView);
        if (panel is null)
            return;

        panel.VerticalAlignment = VerticalAlignment.Top;
        panel.ClearValue(FrameworkElement.MaxHeightProperty);
        panel.ClearValue(FrameworkElement.HeightProperty);
    }

    private static void ReleaseOversizedFlatPanel(TreeView treeView)
    {
        var panel = FindDescendant<ItemsStackPanel>(treeView);
        if (panel is null)
            return;

        panel.ClearValue(FrameworkElement.MinHeightProperty);
        RefreshListLayout(treeView);
    }

    private static TreeViewItem? TryFindFlatTreeViewItem(TreeView treeView, ExecutableFilePickerTreeItem item)
    {
        var panel = FindDescendant<ItemsStackPanel>(treeView);
        if (panel is null)
            return null;

        var count = VisualTreeHelper.GetChildrenCount(panel);
        for (var i = 0; i < count; i++)
        {
            if (VisualTreeHelper.GetChild(panel, i) is TreeViewItem treeViewItem &&
                ReferenceEquals(treeViewItem.DataContext, item))
                return treeViewItem;
        }

        return null;
    }

    private static TreeViewItem? FindFlatTreeViewItem(TreeView treeView, ExecutableFilePickerTreeItem item) =>
        TryFindFlatTreeViewItem(treeView, item) ?? FindTreeViewItem(treeView, item);

    private static double GetContentTop(ScrollViewer scrollViewer, FrameworkElement element)
    {
        var topInViewer = element
            .TransformToVisual(scrollViewer)
            .TransformPoint(new Point(0, 0)).Y;

        return topInViewer + scrollViewer.VerticalOffset;
    }

    private static double GetContentBottom(ScrollViewer scrollViewer, FrameworkElement element) =>
        GetContentTop(scrollViewer, element) + element.ActualHeight;

    private static ExecutableFilePickerTreeRow? FindTreeRow(TreeViewItem item)
    {
        var count = VisualTreeHelper.GetChildrenCount(item);
        for (var i = 0; i < count; i++)
        {
            var child = VisualTreeHelper.GetChild(item, i);
            if (child is ExecutableFilePickerTreeRow row)
                return row;

            var nested = FindTreeRowInTree(child);
            if (nested is not null)
                return nested;
        }

        return null;
    }

    private static ExecutableFilePickerTreeRow? FindTreeRowInTree(DependencyObject node)
    {
        var count = VisualTreeHelper.GetChildrenCount(node);
        for (var i = 0; i < count; i++)
        {
            var child = VisualTreeHelper.GetChild(node, i);
            if (child is ExecutableFilePickerTreeRow row)
                return row;

            var nested = FindTreeRowInTree(child);
            if (nested is not null)
                return nested;
        }

        return null;
    }

    private static TreeViewItem? FindTreeViewItem(DependencyObject root, ExecutableFilePickerTreeItem item)
    {
        var count = VisualTreeHelper.GetChildrenCount(root);
        for (var i = 0; i < count; i++)
        {
            var child = VisualTreeHelper.GetChild(root, i);
            if (child is TreeViewItem treeViewItem)
            {
                if (ReferenceEquals(treeViewItem.DataContext, item))
                    return treeViewItem;

                var nested = FindTreeViewItem(treeViewItem, item);
                if (nested is not null)
                    return nested;
            }
            else
            {
                var nested = FindTreeViewItem(child, item);
                if (nested is not null)
                    return nested;
            }
        }

        return null;
    }

    private static double SumFlatContainerHeights(ItemsStackPanel panel)
    {
        double sum = 0;
        var count = VisualTreeHelper.GetChildrenCount(panel);
        for (var i = 0; i < count; i++)
        {
            if (VisualTreeHelper.GetChild(panel, i) is FrameworkElement element)
                sum += element.ActualHeight;
        }

        return sum;
    }

    private static FrameworkElement? FindDescendantByName(DependencyObject root, string name)
    {
        var count = VisualTreeHelper.GetChildrenCount(root);
        for (var i = 0; i < count; i++)
        {
            var child = VisualTreeHelper.GetChild(root, i);
            if (child is FrameworkElement element && element.Name == name)
                return element;

            var nested = FindDescendantByName(child, name);
            if (nested is not null)
                return nested;
        }

        return null;
    }

    private static T? FindDescendant<T>(DependencyObject root) where T : DependencyObject
    {
        var count = VisualTreeHelper.GetChildrenCount(root);
        for (var i = 0; i < count; i++)
        {
            var child = VisualTreeHelper.GetChild(root, i);
            if (child is T match)
                return match;

            var nested = FindDescendant<T>(child);
            if (nested is not null)
                return nested;
        }

        return null;
    }
}
