namespace GlassSettings.Core.Snapshot;

/// <summary>Как сопоставлять GlassSubKey с записями snapshot (partition vs точный ключ / потомки).</summary>
public enum GlassSubKeyMatchMode
{
    /// <summary>Весь partition (legacy ForGlassSubKey).</summary>
    Partition = 0,

    /// <summary>Только values непосредственно в указанных subkey (без детей).</summary>
    Exact = 1,

    /// <summary>Только потомки prefix\… (сам prefix не входит).</summary>
    Descendants = 2,

    /// <summary>
    /// Partition Tray без потомков GlassSubKey (для бара: всё Tray/Tray\Xaml, без Tray\Xaml\*).
    /// </summary>
    PartitionExceptDescendants = 3
}
