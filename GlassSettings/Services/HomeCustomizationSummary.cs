using GlassSettings.Core.Customization;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Snapshot;
using GlassSettings.Strings;

namespace GlassSettings.Services;

public sealed record HomeRowStatus(
    bool HasCustomizations,
    string Subtitle,
    string? AccentValue = null);

/// <summary>Строки Обзора и footer карточки устройства (через CustomizationTracker).</summary>
public static class HomeCustomizationSummary
{
    public static HomeRowStatus General() =>
        CustomizationTracker.Describe(CustomizationAreaId.General);

    public static HomeRowStatus Popup() =>
        CustomizationTracker.Describe(CustomizationAreaId.Popup);

    public static HomeRowStatus Taskbar() =>
        CustomizationTracker.Describe(CustomizationAreaId.Taskbar);

    public static HomeRowStatus Root() =>
        CustomizationTracker.Describe(CustomizationAreaId.Root);

    public static HomeRowStatus Colorization() =>
        CustomizationTracker.Describe(CustomizationAreaId.DwmColorization);

    public static HomeRowStatus Colorization(DwmRegistryHive hive) =>
        CustomizationTracker.DescribeDwmKeys(DwmSnapshotCatalog.GetKeys(DwmSnapshotPartition.Colorization), hive);

    public static HomeRowStatus DwmGlass() =>
        CustomizationTracker.Describe(CustomizationAreaId.DwmGlass);

    public static HomeRowStatus DwmGlass(DwmRegistryHive hive) =>
        CustomizationTracker.DescribeDwmKeys(DwmSnapshotCatalog.GetKeys(DwmSnapshotPartition.Glass), hive);

    public static HomeRowStatus DwmTheme() =>
        CustomizationTracker.Describe(CustomizationAreaId.DwmTheme);

    public static HomeRowStatus DwmTheme(DwmRegistryHive hive) =>
        CustomizationTracker.DescribeDwmKeys(DwmSnapshotCatalog.GetKeys(DwmSnapshotPartition.Theme), hive);

    public static HomeRowStatus Text() =>
        CustomizationTracker.Describe(CustomizationAreaId.Text);

    public static HomeRowStatus DwmTextures() =>
        CustomizationTracker.DescribeDwmKeys(DwmThemeScopeCatalog.TexturesNavKeys);

    public static HomeRowStatus DwmAtlas() =>
        CustomizationTracker.DescribeDwmKeys(DwmThemeScopeCatalog.AtlasKeys);

    public static string SectionCountLabel(int n) =>
        CustomizationTracker.SectionCountLabel(n);

    public static string ParameterCountLabel(int n) =>
        CustomizationTracker.ParameterCountLabel(n);

    public static int CountUnsavedHomeSections()
    {
        var session = SettingsSession.Current;
        return CustomizationCatalog.HomeSummaryAreas.Count(area =>
            session.HasUnsavedInScope(SettingsCommitScope.ForArea(area)));
    }

    /// <summary>Строка под разделителем на карточке устройства.</summary>
    public static (bool Clickable, string Prefix, string? LinkText, string? PlainText, bool AccentPlain) DeviceFooter(
        bool hasUnsavedChanges)
    {
        if (hasUnsavedChanges)
        {
            var unsavedCount = CountUnsavedHomeSections();
            return unsavedCount > 0
                ? (true,
                    GlassSettingsResources.Get("Home_FooterUnsavedPrefix"),
                    SectionCountLabel(unsavedCount),
                    null,
                    false)
                : (false,
                    GlassSettingsResources.Get("Home_FooterUnsavedPrefix"),
                    null,
                    GlassSettingsResources.Get("Home_FooterUnsavedPlain"),
                    true);
        }

        var n = CustomizationTracker.CountCustomizedAreas(CustomizationCatalog.HomeSummaryAreas);
        if (n == 0)
            return (false, GlassSettingsResources.Get("Home_FooterDefaultPrefix"), null, null, false);

        return (true, GlassSettingsResources.Get("Home_FooterCustomPrefix"), SectionCountLabel(n), null, false);
    }
}
