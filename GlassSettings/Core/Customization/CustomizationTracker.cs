using GlassSettings.Core.Registry;
using GlassSettings.Services;
using GlassSettings.Strings;

namespace GlassSettings.Core.Customization;

/// <summary>
/// Подсистема учёта кастомизации: наличие значения в реестре = изменение относительно отсутствия ключа.
/// </summary>
public static class CustomizationTracker
{
    public sealed record PresentParameter(RegistryParameterRef Ref, uint Value);

    public static bool HasCustomization(CustomizationAreaId area) =>
        CountParameters(area) > 0;

    public static int CountParameters(CustomizationAreaId area) =>
        ListPresent(area).Count;

    public static int CountCustomizedAreas(IEnumerable<CustomizationAreaId> areas) =>
        areas.Count(HasCustomization);

    public static IReadOnlyList<PresentParameter> ListPresent(CustomizationAreaId area)
    {
        var result = new List<PresentParameter>();
        foreach (var parameter in CustomizationCatalog.GetParameters(area))
        {
            if (TryRead(parameter) is { } value)
                result.Add(new PresentParameter(parameter, value));
        }

        return result;
    }

    public static int CountPresentDwmKeys(IEnumerable<string> keys, DwmRegistryHive? hive = null)
    {
        var count = 0;
        foreach (var key in keys)
        {
            var present = hive is { } selectedHive
                ? (DwmRegistryStore.ReadDword(selectedHive, key) is not null
                   || DwmRegistryStore.ValueExists(selectedHive, key))
                : DwmRegistryStore.ExistsInAnyHive(key);

            if (present)
                count++;
        }

        return count;
    }

    public static HomeRowStatus DescribeDwmKeys(IEnumerable<string> keys, DwmRegistryHive? hive = null)
    {
        var count = CountPresentDwmKeys(keys, hive);
        if (count == 0)
            return new HomeRowStatus(false, GlassSettingsResources.Get("Custom_Default"));

        return new HomeRowStatus(true, ParameterCountLabel(count));
    }

    public static HomeRowStatus Describe(CustomizationAreaId area)
    {
        var present = ListPresent(area);
        if (present.Count == 0)
            return new HomeRowStatus(false, GlassSettingsResources.Get("Custom_Default"));

        if (area == CustomizationAreaId.General)
            return DescribeGeneral(present);

        return new HomeRowStatus(true, ParameterCountLabel(present.Count));
    }

    private static HomeRowStatus DescribeGeneral(IReadOnlyList<PresentParameter> present)
    {
        if (present.Count == 1)
        {
            var item = present[0];
            if (item.Ref.ValueName == "EffectType")
            {
                return new HomeRowStatus(
                    true,
                    GlassSettingsResources.Get("Custom_GeneralPrefix"),
                    CustomizationValueFormatter.Format(item.Ref, item.Value));
            }
        }

        return new HomeRowStatus(true, ParameterCountLabel(present.Count));
    }

    public static string ParameterCountLabel(int count) =>
        GlassSettingsResources.CountForm(count, "Custom_ParamChanged_One", "Custom_ParamChanged_Few", "Custom_ParamChanged_Many");

    public static string SectionCountLabel(int count) =>
        GlassSettingsResources.CountForm(count, "Custom_Section_One", "Custom_Section_Few", "Custom_Section_Many");

    private static uint? TryRead(RegistryParameterRef parameter) => parameter.Store switch
    {
        RegistryStoreKind.Glass when GlassRegistry.ReadDword(parameter.GlassSubKey, parameter.ValueName) is { } dword =>
            dword,
        RegistryStoreKind.Glass when GlassRegistry.ReadBinary(parameter.GlassSubKey, parameter.ValueName) is not null =>
            1u,
        RegistryStoreKind.Dwm => DwmRegistryStore.ReadEffectiveDword(parameter.ValueName)
                                 ?? (DwmRegistryStore.ExistsInAnyHive(parameter.ValueName) ? 1u : null),
        _ => null
    };
}
