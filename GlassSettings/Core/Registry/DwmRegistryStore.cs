namespace GlassSettings.Core.Registry;

public static class DwmRegistryStore
{
    public static DwmRegistryHive Opposite(DwmRegistryHive hive) =>
        hive == DwmRegistryHive.Hkcu ? DwmRegistryHive.Hklm : DwmRegistryHive.Hkcu;

    public static bool ValueExists(DwmRegistryHive hive, string valueName) =>
        DwmRegistry.ValueExists(hive, valueName);

    /// <summary>Чтение для detail-страниц: только своя ветка (HKCU или HKLM).</summary>
    public static bool ValueExistsForPage(DwmRegistryHive hive, string valueName) =>
        DwmRegistry.ValueExists(hive, valueName);

    public static bool ExistsInAnyHive(string valueName) =>
        DwmRegistry.ValueExists(DwmRegistryHive.Hkcu, valueName)
        || DwmRegistry.ValueExists(DwmRegistryHive.Hklm, valueName);

    public static bool ExistsInParallelHive(DwmRegistryHive currentHive, string valueName) =>
        DwmRegistry.ValueExists(Opposite(currentHive), valueName);

    public static uint? ReadDword(DwmRegistryHive hive, string valueName) =>
        DwmRegistry.ReadDword(hive, valueName);

    /// <summary>Чтение для detail-страниц: только своя ветка (HKCU или HKLM).</summary>
    public static uint? ReadDwordForPage(DwmRegistryHive hive, string valueName) =>
        DwmRegistry.ReadDword(hive, valueName);

    public static string? ReadString(DwmRegistryHive hive, string valueName) =>
        DwmRegistry.ReadString(hive, valueName);

    /// <summary>Чтение для detail-страниц: только своя ветка (HKCU или HKLM).</summary>
    public static string? ReadStringForPage(DwmRegistryHive hive, string valueName) =>
        DwmRegistry.ReadString(hive, valueName);

    /// <summary>Runtime-effective значение OpenGlass: HKCU имеет приоритет над HKLM.</summary>
    public static uint? ReadEffectiveDword(string valueName) =>
        DwmRegistry.ReadDword(DwmRegistryHive.Hkcu, valueName)
        ?? DwmRegistry.ReadDword(DwmRegistryHive.Hklm, valueName);

    /// <summary>Runtime-effective значение OpenGlass: HKCU имеет приоритет над HKLM.</summary>
    public static string? ReadEffectiveString(string valueName) =>
        DwmRegistry.ReadString(DwmRegistryHive.Hkcu, valueName)
        ?? DwmRegistry.ReadString(DwmRegistryHive.Hklm, valueName);

    public static bool HasAnyPresent(DwmRegistryHive hive, IEnumerable<string> valueNames)
    {
        foreach (var name in valueNames)
        {
            if (DwmRegistry.ValueExists(hive, name))
                return true;
        }

        return false;
    }

    /// <summary>Наличие значения для detail-UI: только своя ветка.</summary>
    public static bool HasAnyPresentForPage(DwmRegistryHive hive, IEnumerable<string> valueNames) =>
        HasAnyPresent(hive, valueNames);

    /// <summary>
    /// Локального ключа в текущей ветке нет, но в противоположной ветке параметр задан
    /// (аналог FlyoutSeparateScopeUi для HKCU/HKLM).
    /// </summary>
    public static bool UsesOppositeBranch(DwmRegistryHive hive, IEnumerable<string> valueNames) =>
        !HasAnyPresent(hive, valueNames) && HasAnyPresent(Opposite(hive), valueNames);

    public static bool HasParallelValues(DwmRegistryHive currentHive, IEnumerable<string> valueNames) =>
        HasAnyPresent(Opposite(currentHive), valueNames);

    /// <summary>Есть хотя бы один ключ из набора одновременно в HKCU и HKLM.</summary>
    public static bool HasIntersectingValues(IEnumerable<string> valueNames)
    {
        foreach (var name in valueNames)
        {
            if (DwmRegistry.ValueExists(DwmRegistryHive.Hkcu, name)
                && DwmRegistry.ValueExists(DwmRegistryHive.Hklm, name))
            {
                return true;
            }
        }

        return false;
    }

    public static int CountHkcuOverrides(IEnumerable<string> valueNames)
    {
        var count = 0;
        foreach (var name in valueNames)
        {
            if (DwmRegistry.ValueExists(DwmRegistryHive.Hkcu, name)
                && DwmRegistry.ValueExists(DwmRegistryHive.Hklm, name))
            {
                count++;
            }
        }

        return count;
    }
}
