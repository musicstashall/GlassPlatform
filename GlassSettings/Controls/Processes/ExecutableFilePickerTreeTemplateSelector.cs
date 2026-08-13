using GlassSettings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls.Processes;

public sealed class ExecutableFilePickerTreeTemplateSelector : DataTemplateSelector
{
    public DataTemplate? GroupTemplate { get; set; }

    public DataTemplate? FolderTemplate { get; set; }

    public DataTemplate? DriveTemplate { get; set; }

    protected override DataTemplate? SelectTemplateCore(object item) =>
        SelectTemplateCore(item, null);

    protected override DataTemplate? SelectTemplateCore(object item, DependencyObject? container)
    {
        if (item is not ExecutableFilePickerTreeItem treeItem)
            return FolderTemplate;

        return treeItem.Kind switch
        {
            ExecutableFilePickerTreeItemKind.LoadingPlaceholder => FolderTemplate,
            ExecutableFilePickerTreeItemKind.QuickAccessGroup or ExecutableFilePickerTreeItemKind.ThisComputer
                => GroupTemplate,
            ExecutableFilePickerTreeItemKind.Drive => DriveTemplate ?? FolderTemplate,
            _ => FolderTemplate
        };
    }
}
