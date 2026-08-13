using GlassSettings.Core.Registry;

namespace GlassSettings.Core.Settings;

public static class GlassSectionGate
{
    public static bool IsAnyPresent(string? subKey, IEnumerable<string> valueNames)
    {
        foreach (var name in valueNames)
        {
            if (GlassRegistry.ReadDword(subKey, name) is not null)
                return true;
        }
        return false;
    }

    public static void DeleteAll(string? subKey, IEnumerable<string> valueNames)
    {
        foreach (var name in valueNames)
            GlassRegistry.DeleteValue(subKey, name);
    }
}
