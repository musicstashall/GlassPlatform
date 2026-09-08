using System.Diagnostics;

namespace GlassSettings.Services;

/// <summary>
/// Трасса registry watch ([RegWatch] в Output / Debug). <see cref="Enabled"/> = false — без вывода.
/// События settle: ArmExternalSyncSettle, CancelExternalSyncSettle, ExternalSyncSettleTick; skip — slider/suppressed/clean.
/// SemanticDirtyDetails — hive, value name, snapshot vs registry.
/// Skip user-origin: Evaluate / ExternalSyncSettleTick / ApplyExternalRegistrySyncFromWatch / Shell — user-origin DWM dirty.
/// </summary>
internal static class RegistryWatchTrace
{
    /// <summary>true — включить трассу для отладки external-sync.</summary>
    private static readonly bool Enabled = false;

    public static void Write(string message)
    {
#if DEBUG
        if (!Enabled)
            return;

        Debug.WriteLine($"[RegWatch] {message}");
#endif
    }

    /// <summary>Semantic-dirty DWM: ключ, параметр, snap→reg.</summary>
    public static void WriteSemanticDirtyDetails(SettingsSession session, string context)
    {
#if DEBUG
        if (!Enabled)
            return;

        var lines = session.DescribeRegistryWatchSemanticDirty();
        if (lines.Count == 0)
        {
            Write($"{context}: SemanticDirty (none)");
            return;
        }

        Write($"{context}: SemanticDirty {lines.Count} value(s)");
        foreach (var line in lines)
            Write($"  {line}");
#endif
    }
}
