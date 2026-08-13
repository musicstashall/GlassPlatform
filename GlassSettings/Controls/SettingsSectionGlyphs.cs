namespace GlassSettings.Controls;

/// <summary>
/// Системные глифы Segoe MDL2 для секционных иконок карточек GlassSettings.
/// </summary>
public static class SettingsSectionGlyphs
{
    public const string General = "\uE713";
    public const string Dwm = "\uE771";
    public const string Glass = "\uE943";
    public const string Gradient = "\uE790";
    public const string Border = "\uE7B9";
    public const string Corners = "\uE8FC";
    public const string Animation = "\uE768";
    public const string EffectType = "\uE943";
    public const string Opacity = "\uE7B3";
    public const string Caption = "\uE8D2";
    public const string Image = "\uE91B";
    public const string Taskbar = "\uE7C4";
    public const string Menu = "\uE8A7";
    public const string DropDown = "\uE70D";
    public const string Tooltip = "\uE946";
    public const string ShellExtension = "\uE8A7";
    public const string Processes = "\uE9F5";
    public const string Root = "\uE8A7";
    public const string Popup = "\uE8A7";
    public const string Text = "\uE8D2";
    public const string SystemFonts = "\uE8D2";
    public const string TextSmoothing = "\uE8D2";
    public const string Theme = "\uE790";
    public const string Colorization = "\uE771";
    public const string GlassColor = "\uE771";
    public const string CaptionColor = "\uE8D2";
    public const string DrawTheme = "\uE7B3";
    public const string Symbols = "\uE896";

    public enum SectionKind
    {
        General,
        Gradient,
        Border,
        Corners,
        Animation,
        EffectType,
        Opacity,
        GlassColor,
        Caption,
        CaptionColor,
        CaptionColorMaximized,
        Image,
        DrawTheme,
        ShellExtension
    }

    public static string ForKind(SectionKind kind) => kind switch
    {
        SectionKind.General => General,
        SectionKind.Gradient => Gradient,
        SectionKind.Border => Border,
        SectionKind.Corners => Corners,
        SectionKind.Animation => Animation,
        SectionKind.EffectType => EffectType,
        SectionKind.Opacity => Opacity,
        SectionKind.GlassColor => GlassColor,
        SectionKind.Caption => Caption,
        SectionKind.CaptionColor => CaptionColor,
        SectionKind.CaptionColorMaximized => CaptionColor,
        SectionKind.Image => Image,
        SectionKind.DrawTheme => DrawTheme,
        SectionKind.ShellExtension => ShellExtension,
        _ => string.Empty
    };

    public static string TryForNavigateId(string id) => id.ToLowerInvariant() switch
    {
        "general" => General,
        "root" => Root,
        "popup" => Popup,
        "taskbar" => Taskbar,
        "text" => Text,
        "menuxaml" => Menu,
        "menu" => Menu,
        "dropdown" => DropDown,
        "tooltip" => Tooltip,
        "colorization" or "colorizationl" => Colorization,
        "glass" or "glassl" or "glass-x86" or "glass-x64" => Glass,
        "theme" or "themel" => Theme,
        "openglass" => Dwm,
        _ => string.Empty
    };
}
