namespace GlassSettings.Core.Layout;

public enum SettingsLayoutMode
{
    Narrow,
    Medium,
    Wide
}

public static class SettingsWindowLayout
{
    public const int DefaultW = 800;
    public const int DefaultH = 600;
    public const int MinW = 600;
    public const int MinH = 460;
    public const double BreakpointPane = 815;
    public const double BreakpointCenter = 1360;
    public const double PaneLength = 320;
    public const double ContentMaxWidth = 1064;
    public const double TitleSearchWidthDefault = 360;

    public static SettingsLayoutMode ModeFromWidth(double width) =>
        width > BreakpointCenter ? SettingsLayoutMode.Wide :
        width > BreakpointPane ? SettingsLayoutMode.Medium :
        SettingsLayoutMode.Narrow;
}
