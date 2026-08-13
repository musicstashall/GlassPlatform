using GlassSettings.Core.Registry;
using GlassSettings.Strings;

namespace GlassSettings.Core.Dwm;

/// <summary>
/// Маркеры U/L и tooltip для DWM detail-страниц при наличии значения
/// в противоположной ветке HKCU/HKLM.
/// </summary>
public static class DwmParallelBranchUi
{
    public static void ApplySectionWarning(
        DwmRegistryHive hive,
        IEnumerable<string> keys,
        Action<bool, string?, string?> setWarning)
    {
        if (!DwmRegistryStore.HasParallelValues(hive, keys))
        {
            setWarning(false, null, null);
            return;
        }

        setWarning(true, ResolveTooltip(hive), ResolveMarker(hive));
    }

    public static string ResolveMarker(DwmRegistryHive hive) =>
        hive == DwmRegistryHive.Hklm
            ? GlassSettingsResources.Get("Dwm_ParallelDuplicateMarkerUser")
            : GlassSettingsResources.Get("Dwm_ParallelDuplicateMarkerLocal");

    public static string ResolveTooltip(DwmRegistryHive hive) =>
        hive == DwmRegistryHive.Hklm
            ? GlassSettingsResources.Get("Dwm_ParallelWarnHklm")
            : GlassSettingsResources.Get("Dwm_ParallelWarnHkcu");
}
