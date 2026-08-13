using GlassSettings.Core.Registry;

namespace GlassSettings.Services;

/// <summary>Исключения для диалога выбора запущенного процесса.</summary>
public static class ProcessPickerExclusions
{
    public static bool IsExcluded(string exeName)
    {
        if (string.IsNullOrWhiteSpace(exeName))
            return true;

        var normalized = AttachListRegistry.NormalizeExeName(exeName);
        if (BlockListRegistry.IsBlocked(normalized))
            return true;

        foreach (var entry in AttachListRegistry.GetEntries())
        {
            if (!string.Equals(entry.ExeName, normalized, StringComparison.OrdinalIgnoreCase))
                continue;

            return entry.Mode == AttachProcessMode.Forbidden;
        }

        return false;
    }
}
