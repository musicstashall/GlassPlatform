using GlassSettings.Core.Registry;

namespace GlassSettings.Core.Snapshot;

/// <summary>Партиции BWM для baseline/dirty (без AttachList/BlockList).</summary>
public enum GlassRegistryPartition
{
    GeneralRoot,
    Menu,
    DropDown,
    ToolTip,
    Root,
    Tray
}

public static class GlassRegistryPartitionPaths
{
    public static string GetRelativeSubKey(GlassRegistryPartition partition) => partition switch
    {
        GlassRegistryPartition.GeneralRoot => string.Empty,
        GlassRegistryPartition.Menu => "Menu",
        GlassRegistryPartition.DropDown => "DropDown",
        GlassRegistryPartition.ToolTip => "ToolTip",
        GlassRegistryPartition.Root => "Root",
        GlassRegistryPartition.Tray => "Tray",
        _ => string.Empty
    };

    public static string GetFullPath(GlassRegistryPartition partition)
    {
        var relative = GetRelativeSubKey(partition);
        return string.IsNullOrEmpty(relative)
            ? RegistryPaths.GlassRoot
            : RegistryPaths.GlassRoot + "\\" + relative;
    }
}
