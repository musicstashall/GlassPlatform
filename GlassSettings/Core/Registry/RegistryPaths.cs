namespace GlassSettings.Core.Registry;

public static class RegistryPaths
{
    public const string GlassRoot = @"Software\StanislavKa\GlassPlatform";

    public const string DwmRoot = @"Software\Microsoft\Windows\DWM";

    public const string GlassSettingsPlacementKey = GlassRoot + @"\GlassSettings";

    public const string MainWindowPlacementValue = "MainWindowPlacement";

    /// <summary>Сегмент подключa DisabledList — исключается из snapshot на любом уровне BWM.</summary>
    public const string GlassDisabledListSegment = "DisabledList";

    /// <summary>Прямые дети BwmRoot без snapshot baseline / dirty / revert.</summary>
    public static readonly string[] GlassNeverSnapshotSubKeys =
    [
        "AttachList", "BlockList", "Crashes", "GlassSettings"
    ];

    /// <summary>
    /// Value-имена вне dirty/revert (runtime sample / миграция схемы).
    /// Иначе CachedSystemStroke / CornerTypeSchema держат Taskbar notice после restore.
    /// </summary>
    public static readonly string[] GlassNeverSnapshotValueNames =
    [
        "CachedSystemFillArgb",
        "CachedSystemStrokeArgb",
        "CornerTypeSchema",
    ];

    /// <summary>Подключи BWM, не входящие в GeneralRoot (values-only на корне).</summary>
    public static readonly string[] GlassExcludedFromGeneralRoot =
    [
        "AttachList", "BlockList", "Crashes", "GlassSettings", "DisabledList",
        "Menu", "DropDown", "ToolTip", "Root", "Tray"
    ];

    public static bool IsNeverSnapshotValueName(string? valueName) =>
        !string.IsNullOrEmpty(valueName)
        && GlassNeverSnapshotValueNames.Contains(valueName, StringComparer.OrdinalIgnoreCase);

    /// <summary>Относительный или полный путь содержит сегмент DisabledList.</summary>
    public static bool ContainsDisabledListSegment(string subKey)
    {
        if (string.IsNullOrEmpty(subKey))
            return false;

        var normalized = subKey.Replace('/', '\\');
        foreach (var segment in normalized.Split('\\', StringSplitOptions.RemoveEmptyEntries))
        {
            if (string.Equals(segment, GlassDisabledListSegment, StringComparison.OrdinalIgnoreCase))
                return true;
        }

        return false;
    }

    /// <summary>Относительный subKey BWM (AttachList, Menu\…) вне политики snapshot.</summary>
    public static bool IsNeverSnapshotRelativeSubKey(string? relativeSubKey)
    {
        if (string.IsNullOrEmpty(relativeSubKey))
            return false;

        var normalized = relativeSubKey.Replace('/', '\\');
        if (ContainsDisabledListSegment(normalized))
            return true;

        var first = normalized.Split('\\')[0];
        return GlassNeverSnapshotSubKeys.Contains(first, StringComparer.OrdinalIgnoreCase)
               && !normalized.Contains('\\');
    }

    public static bool IsNeverSnapshotFullSubKey(string fullSubKey)
    {
        if (string.Equals(fullSubKey, GlassSettingsPlacementKey, StringComparison.OrdinalIgnoreCase))
            return true;

        if (ContainsDisabledListSegment(fullSubKey))
            return true;

        if (!fullSubKey.StartsWith(GlassRoot + "\\", StringComparison.OrdinalIgnoreCase)
            && !string.Equals(fullSubKey, GlassRoot, StringComparison.OrdinalIgnoreCase))
            return false;

        var relative = fullSubKey.Length <= GlassRoot.Length
            ? string.Empty
            : fullSubKey[(GlassRoot.Length + 1)..];

        return IsNeverSnapshotRelativeSubKey(relative);
    }
}
