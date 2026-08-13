namespace GlassSettings.Services;

/// <summary>Сегмент BreadcrumbBar в file picker.</summary>
public sealed class ExecutableFilePickerBreadcrumbItem
{
    public ExecutableFilePickerBreadcrumbItem(string name, string fullPath)
    {
        Name = name;
        FullPath = fullPath;
    }

    public string Name { get; }

    public string FullPath { get; }
}
