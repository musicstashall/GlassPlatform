using GlassSettings.Core.Platform;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;

namespace GlassSettings.Services;

/// <summary>Иконка папки (asset) для hosted file picker; диски — SymbolIcon в XAML.</summary>
internal static class ShellFolderIconResolver
{
    private const string FolderIconRelativePath = "Assets\\folder.png";

    private static ImageSource? _folderIcon;

    public static ImageSource? TryGetFolderIcon()
    {
        EnsureFolderIcon();
        return _folderIcon;
    }

    public static void ApplyTreeItemIcon(ExecutableFilePickerTreeItem item)
    {
        item.Icon = item.Kind switch
        {
            ExecutableFilePickerTreeItemKind.Folder => TryGetFolderIcon(),
            _ => null
        };
    }

    public static void ApplyIconsRecursive(ExecutableFilePickerTreeItem item)
    {
        if (item.Kind != ExecutableFilePickerTreeItemKind.LoadingPlaceholder)
            ApplyTreeItemIcon(item);

        foreach (var child in item.Children)
            ApplyIconsRecursive(child);
    }

    private static void EnsureFolderIcon()
    {
        if (_folderIcon is not null)
            return;

        var path = Path.GetFullPath(Path.Combine(PlatformPaths.AppBase, FolderIconRelativePath));
        if (File.Exists(path))
        {
            try
            {
                _folderIcon = new BitmapImage(AppIconSourceFactory.ToFileUri(path));
                return;
            }
            catch
            {
                // fallback ниже
            }
        }

        try
        {
            _folderIcon = new BitmapImage(new Uri("ms-appx:///Assets/folder.png"));
        }
        catch
        {
            _folderIcon = null;
        }
    }
}
