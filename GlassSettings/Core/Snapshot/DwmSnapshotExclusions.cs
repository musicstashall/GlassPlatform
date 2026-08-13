namespace GlassSettings.Core.Snapshot;

/// <summary>
/// Ключи HKCU\DWM, исключённые из snapshot (Windows / персонализация).
/// Синхронизировать с docs/guides/GlassSettings-customization-tracking.md.
/// </summary>
public static class DwmSnapshotExclusions
{
    public static IReadOnlyList<string> DwmNeverSnapshotValues { get; } =
    [
        "AccentColor",
        "AlwaysHibernateThumbnails",
        "ColorizationColor",
        "ColorizationGlassAttribute",
        "ColorPrevalence",
        "Composition",
        "EnableAeroPeek",
        "EnableWindowColorization"
    ];

    public static bool IsExcludedDwmValue(string valueName) =>
        DwmNeverSnapshotValues.Contains(valueName, StringComparer.OrdinalIgnoreCase);
}
