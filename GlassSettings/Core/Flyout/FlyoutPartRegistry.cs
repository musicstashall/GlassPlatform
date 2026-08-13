using GlassSettings.Core.Registry;
using GlassSettings.Core.Settings;

namespace GlassSettings.Core.Flyout;

/// <summary>Flyout/part (Menu, Tray, Root\\Active, …): UI-чтение через Shared::PartRegistryQuery.</summary>
public static class FlyoutPartRegistry
{
    public static bool TryReadLocalDword(string regSub, string valueName, out uint value) =>
        PartRegistryInterop.TryReadLocalDword(regSub, valueName, out value);

    public static uint ReadEffectiveDword(string regSub, string valueName, uint defaultValue) =>
        PartRegistryInterop.ReadEffectiveDword(regSub, valueName, defaultValue);

    public static uint ReadCornerTypeForUi(string regSub) =>
        PartRegistryInterop.ReadUiCornerType(regSub);

    public static bool ReadNoBorderColorForUi(string regSub) =>
        PartRegistryInterop.ReadUiNoBorderColor(regSub);

    public static bool IsCornerTypePresent(string regSub) =>
        TryReadLocalDword(regSub, "CornerType", out _);

    public static bool IsDropShadowPresent(string regSub) =>
        TryReadLocalDword(regSub, "EnableDropShadow", out _);
}
