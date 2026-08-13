namespace GlassSettings.Navigation;

public enum FlyoutKind
{
    General = 0,
    Menu,
    DropDown,
    ToolTip
}

public static class FlyoutKindExtensions
{
    public static string RegSub(this FlyoutKind kind) => kind switch
    {
        FlyoutKind.Menu => "Menu",
        FlyoutKind.DropDown => "DropDown",
        FlyoutKind.ToolTip => "ToolTip",
        _ => ""
    };
}
