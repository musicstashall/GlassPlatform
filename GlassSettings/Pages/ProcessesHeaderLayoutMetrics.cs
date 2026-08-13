using GlassSettings.Core.Layout;

namespace GlassSettings.Pages;

/// <summary>
/// Метрики шапки «Процессы»: пороги заданы для окна, в коде — ширина контента (без NavPane).
/// </summary>
internal static class ProcessesHeaderLayoutMetrics
{
    public const double SearchMaxWidth = 366;
    public const double SearchFixedWidth = 320;
    public const double SearchHeight = 32;
    public const double ViewSwitcherWidth = 126;
    public const double ViewSwitcherHeight = 44;
    public const double MinSearchViewGap = 92;
    public const double HeaderRowSpacing = 12;

    /// <summary>Окно &lt; 1290 → три строки шапки.</summary>
    public const double WindowBreakpointThreeRows = 1290;

    /// <summary>Окно &lt; 900 → четыре строки (фильтр и сортировка отдельно).</summary>
    public const double WindowBreakpointFourRows = 900;

    public static double ContentBreakpointThreeRows =>
        WindowBreakpointThreeRows - SettingsWindowLayout.PaneLength;

    public static double ContentBreakpointFourRows =>
        WindowBreakpointFourRows - SettingsWindowLayout.PaneLength;

    public static double ResolveSearchWidth(double contentWidth)
    {
        var gapWithMax = contentWidth - SearchMaxWidth - ViewSwitcherWidth;
        if (gapWithMax < MinSearchViewGap)
            return SearchFixedWidth;

        return SearchMaxWidth;
    }
}
