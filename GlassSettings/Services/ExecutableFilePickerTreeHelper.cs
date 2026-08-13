using GlassSettings.Strings;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml.Controls;
using System.Collections.ObjectModel;

namespace GlassSettings.Services;

/// <summary>Построение и lazy-load иерархии TreeView (ItemsSource + recursive TreeViewItem, как WinUI Gallery).</summary>
internal static class ExecutableFilePickerTreeHelper
{
    public static ObservableCollection<ExecutableFilePickerTreeItem> BuildRootItems()
    {
        var roots = new ObservableCollection<ExecutableFilePickerTreeItem>();

        var quickAccess = new ExecutableFilePickerTreeItem
        {
            Name = GlassSettingsResources.Get("Attach_FilePickerQuickAccess"),
            Kind = ExecutableFilePickerTreeItemKind.QuickAccessGroup,
            IsExpanded = true
        };
        TryAddKnownFolder(quickAccess.Children, Environment.SpecialFolder.DesktopDirectory, "Attach_FilePickerQuickDesktop");
        TryAddKnownFolder(quickAccess.Children, Environment.SpecialFolder.MyDocuments, "Attach_FilePickerQuickDocuments");
        TryAddKnownFolder(quickAccess.Children, Environment.SpecialFolder.MyPictures, "Attach_FilePickerQuickPictures");
        TryAddDownloads(quickAccess.Children);
        roots.Add(quickAccess);

        var thisComputer = new ExecutableFilePickerTreeItem
        {
            Name = GlassSettingsResources.Get("Attach_FilePickerQuickComputer"),
            Kind = ExecutableFilePickerTreeItemKind.ThisComputer,
            IsExpanded = true
        };

        foreach (var drive in DriveInfo.GetDrives())
        {
            if (!drive.IsReady)
                continue;

            thisComputer.Children.Add(CreateDriveItem(
                drive.RootDirectory.FullName,
                FormatDriveDisplayName(drive)));
        }

        roots.Add(thisComputer);
        return roots;
    }

    public static async Task LoadChildrenAsync(ExecutableFilePickerTreeItem parent, DispatcherQueue dispatcherQueue)
    {
        if (!parent.HasUnloadedChildren || parent.FullPath is null)
            return;

        var subfolders = await ExecutableFilePickerCatalog.LoadSubfoldersAsync(parent.FullPath).ConfigureAwait(false);
        var completion = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        dispatcherQueue.TryEnqueue(() =>
        {
            try
            {
                parent.Children.Clear();
                foreach (var folder in subfolders)
                    parent.Children.Add(CreateFolderItem(folder.FullPath, folder.Name));

                parent.HasUnloadedChildren = false;
                completion.SetResult();
            }
            catch (Exception ex)
            {
                completion.SetException(ex);
            }
        });

        await completion.Task.ConfigureAwait(true);
    }

    public static ExecutableFilePickerTreeItem? FindVisibleSelectionForPath(
        IEnumerable<ExecutableFilePickerTreeItem> roots,
        string path) =>
        FindPathTreeSelection(roots, path);

    /// <summary>
    /// Лучший узел для выделения: deepest среди развёрнутых; при свёрнутом L0 — сам L0, иначе L1 на пути.
    /// </summary>
    public static ExecutableFilePickerTreeItem? FindPathTreeSelection(
        IEnumerable<ExecutableFilePickerTreeItem> roots,
        string path)
    {
        if (!ExecutableFilePickerCatalog.IsValidDirectory(path))
            return null;

        var normalized = NormalizeDirectoryPath(path);
        ExecutableFilePickerTreeItem? deepestVisible = null;

        foreach (var root in roots)
            TryFindDeepestVisibleMatch(root, normalized, ref deepestVisible);

        if (deepestVisible is not null)
            return deepestVisible;

        var quickAccess = TryGetQuickAccessRoot(roots);
        if (quickAccess is not null)
        {
            foreach (var child in quickAccess.Children)
            {
                if (child.FullPath is null)
                    continue;

                var childPath = NormalizeDirectoryPath(child.FullPath);
                if (string.Equals(childPath, normalized, StringComparison.OrdinalIgnoreCase)
                    || IsAncestorPath(childPath, normalized))
                    return quickAccess.IsExpanded ? child : quickAccess;
            }
        }

        var pathRoot = Path.GetPathRoot(normalized);
        if (string.IsNullOrWhiteSpace(pathRoot))
            return null;

        var thisComputer = TryGetThisComputerRoot(roots);
        var drive = TryFindDriveRoot(roots, pathRoot);
        if (drive is null)
            return null;

        if (thisComputer is not null && !thisComputer.IsExpanded)
            return thisComputer;

        return drive;
    }

    /// <summary>Прямой потомок родителя на пути к целевой папке.</summary>
    public static ExecutableFilePickerTreeItem? FindDirectPathChild(
        ExecutableFilePickerTreeItem parent,
        string path)
    {
        if (!ExecutableFilePickerCatalog.IsValidDirectory(path))
            return null;

        var normalized = NormalizeDirectoryPath(path);

        foreach (var child in parent.Children)
        {
            if (child.Kind == ExecutableFilePickerTreeItemKind.LoadingPlaceholder)
                continue;

            if (child.FullPath is null)
                continue;

            var childPath = NormalizeDirectoryPath(child.FullPath);
            if (string.Equals(childPath, normalized, StringComparison.OrdinalIgnoreCase)
                || IsAncestorPath(childPath, normalized))
                return child;
        }

        return null;
    }

    /// <summary>Фокус прокрутки после expand: дочерний узел на пути или последний ребёнок.</summary>
    public static ExecutableFilePickerTreeItem? ResolveExpandScrollFocusChild(
        ExecutableFilePickerTreeItem expandedParent,
        string? currentPath)
    {
        var lastChild = FindLastDirectChild(expandedParent);
        if (lastChild is null)
            return null;

        if (string.IsNullOrWhiteSpace(currentPath)
            || !IsExpandRelatedToPath(expandedParent, currentPath))
            return lastChild;

        return FindDirectPathChild(expandedParent, currentPath) ?? lastChild;
    }

    public static bool IsExpandRelatedToPath(ExecutableFilePickerTreeItem node, string path)
    {
        if (!ExecutableFilePickerCatalog.IsValidDirectory(path))
            return false;

        if (node.Kind == ExecutableFilePickerTreeItemKind.LoadingPlaceholder)
            return false;

        var normalized = NormalizeDirectoryPath(path);

        if (node.FullPath is not null)
        {
            var nodePath = NormalizeDirectoryPath(node.FullPath);
            return string.Equals(nodePath, normalized, StringComparison.OrdinalIgnoreCase)
                || IsAncestorPath(nodePath, normalized);
        }

        foreach (var child in node.Children)
        {
            if (IsExpandRelatedToPath(child, path))
                return true;
        }

        return false;
    }

    public static void SelectTreeItem(TreeView treeView, ExecutableFilePickerTreeItem item) =>
        treeView.SelectedItem = item;

    private static void TryFindDeepestVisibleMatch(
        ExecutableFilePickerTreeItem node,
        string targetPath,
        ref ExecutableFilePickerTreeItem? deepestVisible)
    {
        if (node.FullPath is not null)
        {
            var nodePath = NormalizeDirectoryPath(node.FullPath);
            if (string.Equals(nodePath, targetPath, StringComparison.OrdinalIgnoreCase)
                || IsAncestorPath(nodePath, targetPath))
            {
                if (deepestVisible is null || IsStrictAncestorPath(deepestVisible.FullPath!, nodePath))
                    deepestVisible = node;
            }
        }

        if (!node.IsExpanded)
            return;

        foreach (var child in node.Children)
        {
            if (child.Kind == ExecutableFilePickerTreeItemKind.LoadingPlaceholder)
                continue;

            TryFindDeepestVisibleMatch(child, targetPath, ref deepestVisible);
        }
    }

    private static ExecutableFilePickerTreeItem? TryFindDriveRoot(
        IEnumerable<ExecutableFilePickerTreeItem> roots,
        string pathRoot)
    {
        var normalizedRoot = NormalizeDirectoryPath(pathRoot);
        if (string.IsNullOrWhiteSpace(normalizedRoot))
            return null;

        foreach (var root in roots)
        {
            if (root.Kind != ExecutableFilePickerTreeItemKind.ThisComputer)
                continue;

            foreach (var child in root.Children)
            {
                if (child.Kind != ExecutableFilePickerTreeItemKind.Drive || child.FullPath is null)
                    continue;

                if (string.Equals(
                        NormalizeDirectoryPath(child.FullPath),
                        normalizedRoot,
                        StringComparison.OrdinalIgnoreCase))
                    return child;
            }
        }

        return null;
    }

    private static bool IsStrictAncestorPath(string ancestorPath, string descendantPath)
    {
        if (string.Equals(
                NormalizeDirectoryPath(ancestorPath),
                NormalizeDirectoryPath(descendantPath),
                StringComparison.OrdinalIgnoreCase))
            return false;

        return IsAncestorPath(ancestorPath, descendantPath);
    }

    private static ExecutableFilePickerTreeItem CreateDriveItem(string path, string displayName) =>
        CreateFolderItem(path, displayName, ExecutableFilePickerTreeItemKind.Drive);

    internal static string FormatDriveDisplayName(DriveInfo drive)
    {
        var letter = drive.Name.TrimEnd('\\');
        var label = drive.VolumeLabel?.Trim();
        if (!string.IsNullOrWhiteSpace(label))
            return $"{label} ({letter})";

        return GlassSettingsResources.Format("Attach_FilePickerDriveDefault", letter);
    }

    private static ExecutableFilePickerTreeItem CreateFolderItem(
        string path,
        string displayName,
        ExecutableFilePickerTreeItemKind kind = ExecutableFilePickerTreeItemKind.Folder)
    {
        var item = new ExecutableFilePickerTreeItem
        {
            Name = displayName,
            FullPath = ExecutableFilePickerCatalog.NormalizeBrowsablePath(path),
            Kind = kind
        };

        if (ExecutableFilePickerCatalog.HasSubfolders(item.FullPath!))
            item.HasUnloadedChildren = true;

        ShellFolderIconResolver.ApplyTreeItemIcon(item);
        return item;
    }

    private static void TryAddKnownFolder(
        ICollection<ExecutableFilePickerTreeItem> nodes,
        Environment.SpecialFolder folder,
        string resourceKey)
    {
        var path = Environment.GetFolderPath(folder);
        if (!Directory.Exists(path))
            return;

        nodes.Add(CreateFolderItem(path, GlassSettingsResources.Get(resourceKey)));
    }

    private static void TryAddDownloads(ICollection<ExecutableFilePickerTreeItem> nodes)
    {
        var userProfile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        if (string.IsNullOrWhiteSpace(userProfile))
            return;

        var downloads = Path.Combine(userProfile, "Downloads");
        if (!Directory.Exists(downloads))
            return;

        nodes.Add(CreateFolderItem(downloads, GlassSettingsResources.Get("Attach_FilePickerQuickDownloads")));
    }

    private static string NormalizeDirectoryPath(string path) =>
        path.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);

    private static bool IsAncestorPath(string ancestorPath, string descendantPath)
    {
        var ancestor = NormalizeDirectoryPath(ancestorPath);
        var descendant = NormalizeDirectoryPath(descendantPath);
        if (string.Equals(ancestor, descendant, StringComparison.OrdinalIgnoreCase))
            return true;

        var prefix = ancestor.EndsWith(Path.DirectorySeparatorChar)
            ? ancestor
            : ancestor + Path.DirectorySeparatorChar;

        return descendant.StartsWith(prefix, StringComparison.OrdinalIgnoreCase);
    }

    public static int CountVisibleExpandedNodes(IEnumerable<ExecutableFilePickerTreeItem> roots)
    {
        var count = 0;
        foreach (var root in roots)
            CountVisibleExpandedNodesCore(root, ref count);

        return count;
    }

    public static ExecutableFilePickerTreeItem? FindLastVisibleExpandedNode(
        IEnumerable<ExecutableFilePickerTreeItem> roots)
    {
        ExecutableFilePickerTreeItem? last = null;
        foreach (var root in roots)
            FindLastVisibleExpandedNodeCore(root, ref last);

        return last;
    }

    /// <summary>Последний прямой потомок узла (без placeholder).</summary>
    public static ExecutableFilePickerTreeItem? FindLastDirectChild(ExecutableFilePickerTreeItem parent)
    {
        for (var i = parent.Children.Count - 1; i >= 0; i--)
        {
            if (parent.Children[i] is { Kind: not ExecutableFilePickerTreeItemKind.LoadingPlaceholder } child)
                return child;
        }

        return null;
    }

    /// <summary>Индекс узла в плоском списке TreeView (pre-order, только развёрнутые ветки).</summary>
    public static bool TryGetVisibleFlatIndex(
        IEnumerable<ExecutableFilePickerTreeItem> roots,
        ExecutableFilePickerTreeItem target,
        out int index)
    {
        var current = 0;
        foreach (var root in roots)
        {
            if (TryGetVisibleFlatIndexCore(root, target, ref current, out index))
                return true;
        }

        index = -1;
        return false;
    }

    public static bool IsBranchModelReady(ExecutableFilePickerTreeItem expandedParent) =>
        !expandedParent.HasUnloadedChildren
        && FindLastDirectChild(expandedParent) is not null;

    private static bool TryGetVisibleFlatIndexCore(
        ExecutableFilePickerTreeItem node,
        ExecutableFilePickerTreeItem target,
        ref int current,
        out int index)
    {
        if (node.Kind == ExecutableFilePickerTreeItemKind.LoadingPlaceholder)
        {
            index = -1;
            return false;
        }

        if (ReferenceEquals(node, target))
        {
            index = current;
            return true;
        }

        current++;

        if (!node.IsExpanded)
        {
            index = -1;
            return false;
        }

        foreach (var child in node.Children)
        {
            if (TryGetVisibleFlatIndexCore(child, target, ref current, out index))
                return true;
        }

        index = -1;
        return false;
    }

    private static void CountVisibleExpandedNodesCore(ExecutableFilePickerTreeItem node, ref int count)
    {
        if (node.Kind == ExecutableFilePickerTreeItemKind.LoadingPlaceholder)
            return;

        count++;
        if (!node.IsExpanded)
            return;

        foreach (var child in node.Children)
            CountVisibleExpandedNodesCore(child, ref count);
    }

    private static void FindLastVisibleExpandedNodeCore(
        ExecutableFilePickerTreeItem node,
        ref ExecutableFilePickerTreeItem? last)
    {
        if (node.Kind == ExecutableFilePickerTreeItemKind.LoadingPlaceholder)
            return;

        last = node;
        if (!node.IsExpanded)
            return;

        foreach (var child in node.Children)
            FindLastVisibleExpandedNodeCore(child, ref last);
    }

    public static ExecutableFilePickerTreeItem? TryGetQuickAccessRoot(
        IEnumerable<ExecutableFilePickerTreeItem> roots) =>
        roots.FirstOrDefault(item => item.Kind == ExecutableFilePickerTreeItemKind.QuickAccessGroup);

    public static ExecutableFilePickerTreeItem? TryGetThisComputerRoot(
        IEnumerable<ExecutableFilePickerTreeItem> roots) =>
        roots.FirstOrDefault(item => item.Kind == ExecutableFilePickerTreeItemKind.ThisComputer);

    public static string? TryGetDefaultQuickAccessPath(ExecutableFilePickerTreeItem quickAccess)
    {
        foreach (var child in quickAccess.Children)
        {
            if (child.Kind == ExecutableFilePickerTreeItemKind.LoadingPlaceholder)
                continue;

            if (!string.IsNullOrWhiteSpace(child.FullPath) &&
                ExecutableFilePickerCatalog.IsValidDirectory(child.FullPath))
                return child.FullPath;
        }

        return null;
    }
}
