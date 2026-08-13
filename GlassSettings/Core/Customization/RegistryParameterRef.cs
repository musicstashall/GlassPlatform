namespace GlassSettings.Core.Customization;

public enum RegistryStoreKind
{
    Glass,
    Dwm
}

/// <summary>Ссылка на один DWORD-параметр в реестре настроек.</summary>
public readonly record struct RegistryParameterRef(
    RegistryStoreKind Store,
    string? GlassSubKey,
    string ValueName)
{
    public static RegistryParameterRef Glass(string? subKey, string valueName) =>
        new(RegistryStoreKind.Glass, subKey, valueName);

    public static RegistryParameterRef Dwm(string valueName) =>
        new(RegistryStoreKind.Dwm, null, valueName);
}
