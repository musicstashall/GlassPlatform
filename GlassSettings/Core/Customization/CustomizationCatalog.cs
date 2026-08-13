using GlassSettings.Core.Flyout;
using GlassSettings.Core.Menu;
using GlassSettings.Core.Tooltip;
using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;

namespace GlassSettings.Core.Customization;

/// <summary>Каталог отслеживаемых параметров по разделам (единый источник для UI и Обзора).</summary>
public static class CustomizationCatalog
{
    private static readonly string[] DwmColorizationKeys =
    [
        "ColorizationColor", "ColorizationColorOverride", "ColorizationColorInactive",
        "ColorizationAfterglow", "ColorizationOpaqueBlend",
        "ColorizationOpaqueBlendPriority", "GlassOverrideAccent", "BlurDeviation",
        "GlassOpacity", "GlassOpacityInactive",
        "GlassType", "ColorizationOpacity", "ColorizationBlurBalance",
        "ColorizationBaseTransparent"
    ];

    private static readonly string[] DwmGlassKeys =
    [
        "BlurOptimization", "RoundRectRadius", "UseDirect3DRendering",
        "CaptionButtons", "CenterCaption", "TextGlowMode", "DisableModernBorders",
        "ColorizationColorCaption", "ColorizationColorCaptionMaximized",
        "ColorizationColorCaptionInactive", "ColorizationColorCaptionInactiveMaximized"
    ];

    private static readonly string[] DwmThemeKeys =
    [
        "ColorizationGlassReflectionOpacity", "ColorizationGlassReflectionOpacityMaximized",
        "ColorizationGlassReflectionOpacityInactive", "ColorizationGlassReflectionOpacityInactiveMaximized",
        "ColorizationGlassReflectionIntensity", "ColorizationGlassReflectionParallaxIntensity",
        "ColorizationGlassReflectionPolicy", "CustomThemeReflection", "CustomThemeMaterial",
        "MaterialOpacity", "CustomThemeAtlas"
    ];

    private static readonly string[] PopupKinds = ["Menu", "DropDown", "ToolTip"];
    private static readonly string[] RootSwcaSubs = ["Root\\Active", "Root\\Inactive"];

    public static IReadOnlyList<RegistryParameterRef> GetParameters(CustomizationAreaId area) => area switch
    {
        CustomizationAreaId.General => FlyoutSectionParameters("", includeEffectType: true).ToList(),
        CustomizationAreaId.Root => RootParameters(),
        CustomizationAreaId.Popup => PopupKinds
            .SelectMany(s => FlyoutSectionParameters(s, includeEffectType: true))
            .Concat([MenuCustomRenderingCoordinator.MasterParameterRef])
            .Concat(MenuRootRegistry.AllParameterRefs())
            .Concat(MenuItemRegistry.AllPopupParameterRefs())
            .Concat(TooltipRenderingRegistry.AllParameterRefs())
            .ToList(),
        CustomizationAreaId.Taskbar => FlyoutSectionParameters("Tray", includeEffectType: true)
            .Append(RegistryParameterRef.Glass("Tray", "BorderDrawMode"))
            .Append(RegistryParameterRef.Glass("Tray\\Xaml", "EffectType"))
            .Append(RegistryParameterRef.Glass("Tray\\Xaml", "CornerType"))
            .Append(RegistryParameterRef.Glass("Tray\\Xaml", "SeparateZones"))
            .Concat(FlyoutSectionParameters("Tray\\Xaml\\Popup", includeEffectType: true, includeCorners: true))
            .Concat(FlyoutSectionParameters("Tray\\Xaml\\Preview", includeEffectType: true, includeCorners: true))
            .Concat(FlyoutSectionParameters("Tray\\Xaml\\Menu", includeEffectType: true, includeCorners: true))
            .Concat(FlyoutSectionParameters("Tray\\Xaml\\Tooltip", includeEffectType: true, includeCorners: true))
            .ToList(),
        CustomizationAreaId.DwmColorization => DwmColorizationKeys.Select(RegistryParameterRef.Dwm).ToList(),
        CustomizationAreaId.DwmGlass => DwmGlassKeys.Select(RegistryParameterRef.Dwm).ToList(),
        CustomizationAreaId.DwmTheme => DwmThemeKeys.Select(RegistryParameterRef.Dwm).ToList(),
        CustomizationAreaId.Text =>
        [
            ..ThemeParameterNames.GlobalTextGlowNames
                .Select(name => RegistryParameterRef.Glass("", name)),
            RegistryParameterRef.Glass("", GlassInkAlphaCapsuleRegistry.ValueName),
        ],
        _ => []
    };

    public static IReadOnlyList<CustomizationAreaId> SessionDirtyAreas =>
    [
        ..HomeSummaryAreas,
        CustomizationAreaId.Text
    ];

    public static IReadOnlyList<CustomizationAreaId> HomeSummaryAreas =>
    [
        CustomizationAreaId.Root,
        CustomizationAreaId.Popup,
        CustomizationAreaId.Taskbar,
        CustomizationAreaId.DwmColorization,
        CustomizationAreaId.DwmGlass,
        CustomizationAreaId.DwmTheme
    ];

    /// <summary>Есть ли в подразделе flyout (Menu, DropDown, …) хотя бы один ключ в реестре.</summary>
    public static bool IsFlyoutSectionActive(string regSub)
    {
        foreach (var parameter in FlyoutSectionParameters(regSub, includeEffectType: true))
        {
            if (parameter.Store != RegistryStoreKind.Glass)
                continue;

            if (GlassRegistry.ReadDword(parameter.GlassSubKey, parameter.ValueName) is not null)
                return true;
        }

        return false;
    }

    /// <summary>Есть ли SWCA-ключи в Root\Inactive.</summary>
    public static bool IsRootInactiveSectionActive() =>
        IsFlyoutSectionActive("Root\\Inactive");

    /// <summary>Root\SeparateActiveInactive или эвристика по Inactive.</summary>
    public static bool IsRootPerWindowStateScope()
    {
        var flag = GlassRegistry.ReadDword("Root", "SeparateActiveInactive");
        if (flag is 0)
            return false;
        if (flag is 1)
            return true;
        return IsRootInactiveSectionActive();
    }

    /// <summary>Отслеживаемые BWM-параметры подраздела flyout (Menu, DropDown, …).</summary>
    public static IReadOnlyList<RegistryParameterRef> GetFlyoutSectionTrackedParameters(string regSub) =>
        FlyoutSectionParameters(regSub, includeEffectType: true)
            .Where(static p => p.Store == RegistryStoreKind.Glass)
            .ToList();

    /// <summary>Удалить весь подраздел flyout (выключение gate на hub): Menu, Menu\Animation, …</summary>
    public static void RemoveFlyoutSection(string regSub) =>
        GlassRegistry.DeleteSubKeyTree(regSub);

    private static List<RegistryParameterRef> RootParameters()
    {
        var list = new List<RegistryParameterRef>
        {
            RegistryParameterRef.Glass("Root", "NoBackdrop"),
        };

        if (WindowsOsHelper.IsWindows11OrGreater())
            list.Add(RegistryParameterRef.Glass("Root", "CornerType"));

        foreach (var sub in RootSwcaSubs)
            list.AddRange(FlyoutSectionParameters(sub, includeEffectType: true, includeCorners: false));

        // Root\Control\{Ribbon|ListView|TreeView}: удаление фона иконок (Дополнительно).
        list.AddRange(Root.RootControlIconRegistry.AllParameterRefs());

        return list;
    }

    private static IEnumerable<RegistryParameterRef> FlyoutSectionParameters(
        string regSub,
        bool includeEffectType,
        bool includeCorners = true)
    {
        if (includeEffectType)
            yield return RegistryParameterRef.Glass(regSub, "EffectType");

        foreach (var name in ThemeParameterNames.GradientNames)
            yield return RegistryParameterRef.Glass(regSub, name);

        foreach (var name in ThemeParameterNames.BorderNames)
            yield return RegistryParameterRef.Glass(regSub, name);

        if (includeCorners)
        {
            foreach (var name in FlyoutCornerRegistry.ValueNames)
                yield return RegistryParameterRef.Glass(regSub, name);
        }

        foreach (var name in FlyoutAnimRegistry.ValueNames)
            yield return RegistryParameterRef.Glass(FlyoutAnimRegistry.AnimSubKey(regSub), name);

        if (regSub.StartsWith("Root\\", StringComparison.Ordinal))
            yield return RegistryParameterRef.Glass(regSub, "GlassDrawThemeConstantAlpha");
    }
}
