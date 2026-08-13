namespace GlassSettings.Navigation;

public sealed class SettingsBreadcrumbItem
{
    public SettingsBreadcrumbItem(string name, Type pageType, object? parameter = null)
    {
        Name = name;
        PageType = pageType;
        Parameter = parameter;
    }

    public string Name { get; }

    public Type PageType { get; }

    public object? Parameter { get; }
}
