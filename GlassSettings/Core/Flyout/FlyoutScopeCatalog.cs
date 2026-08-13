using GlassSettings.Core.Customization;
using GlassSettings.Core.Menu;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Root;
using GlassSettings.Core.Tooltip;

namespace GlassSettings.Core.Flyout;

/// <summary>Параметры flyout по группам — для unsaved-точек на отдельных спойлерах.</summary>
public static class FlyoutScopeCatalog
{
    public static IReadOnlyList<RegistryParameterRef> GetParameters(string regSub, FlyoutParameterGroup group) =>
        group switch
        {
            FlyoutParameterGroup.Effect =>
                [RegistryParameterRef.Glass(regSub, "EffectType")],
            FlyoutParameterGroup.Gradient =>
                ThemeParameterNames.GradientNames
                    .Select(name => RegistryParameterRef.Glass(regSub, name))
                    .ToList(),
            FlyoutParameterGroup.Border =>
                ThemeParameterNames.BorderNames
                    .Select(name => RegistryParameterRef.Glass(regSub, name))
                    .ToList(),
            // Не включать BorderDrawMode сюда: переключение DWM/XAML ложно зажигало «Цвет бордюров».
            FlyoutParameterGroup.Zones =>
                [RegistryParameterRef.Glass("Tray\\Xaml", "SeparateZones")],
            // Только сам переключатель DWM/XAML — не параметры внутри секций.
            FlyoutParameterGroup.PaintPath =>
                [RegistryParameterRef.Glass("Tray", "BorderDrawMode")],
            FlyoutParameterGroup.Corners =>
                FlyoutCornerRegistry.ValueNames
                    .Select(name => RegistryParameterRef.Glass(regSub, name))
                    .ToList(),
            FlyoutParameterGroup.Animation =>
                FlyoutAnimRegistry.ValueNames
                    .Select(name => RegistryParameterRef.Glass(FlyoutAnimRegistry.AnimSubKey(regSub), name))
                    .ToList(),
            FlyoutParameterGroup.DrawTheme =>
                [RegistryParameterRef.Glass(regSub, "GlassDrawThemeConstantAlpha")],
            FlyoutParameterGroup.MenuItem =>
                MenuItemRegistry.TryKindFromRegSub(regSub, out var menuItemKind)
                    ? MenuItemRegistry.GetParameterRefs(menuItemKind).ToList()
                    : [],
            FlyoutParameterGroup.MenuCustomRenderingMaster =>
                regSub == MenuCustomRenderingCoordinator.MenuScopeSubKey
                    ? [MenuCustomRenderingCoordinator.MasterParameterRef]
                    : [],
            FlyoutParameterGroup.MenuRoot =>
                regSub == MenuRootRegistry.MenuScopeSubKey
                    ? MenuRootRegistry.AllParameterRefs().ToList()
                    : [],
            FlyoutParameterGroup.TooltipRendering =>
                regSub.Equals(TooltipRenderingRegistry.RegSub, StringComparison.OrdinalIgnoreCase)
                    ? TooltipRenderingRegistry.AllParameterRefs().ToList()
                    : [],
            FlyoutParameterGroup.RootControlIcon =>
                RootControlIconRegistry.TryKindFromRegSub(regSub, out var rootIconKind)
                    ? RootControlIconRegistry.GetParameterRefs(rootIconKind).ToList()
                    : regSub.Equals(RootControlIconRegistry.ControlScopeSubKey, StringComparison.OrdinalIgnoreCase)
                        ? RootControlIconRegistry.AllParameterRefs().ToList()
                        : [],
            _ => []
        };

    public static bool IsFlyoutSectionActive(string regSub) =>
        CustomizationCatalog.IsFlyoutSectionActive(regSub);
}
