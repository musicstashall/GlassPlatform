namespace GlassSettings.Navigation;

/// <summary>Снимок состояния навигации для глобального стека «Назад».</summary>
public sealed class SettingsNavigationSnapshot
{
    public SettingsNavigationSnapshot(string? sidebarTag, IReadOnlyList<SettingsBreadcrumbItem> trail)
    {
        SidebarTag = sidebarTag;
        Trail = trail;
    }

    public string? SidebarTag { get; }

    public IReadOnlyList<SettingsBreadcrumbItem> Trail { get; }
}
