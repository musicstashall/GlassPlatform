namespace GlassSettings.Services;

/// <summary>Переход между корневыми разделами из вложенных страниц.</summary>
public static class MainWindowNavigation
{
    public static Action<string>? NavigateSidebar { get; set; }
}
