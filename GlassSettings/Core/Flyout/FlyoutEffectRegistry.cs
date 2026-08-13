namespace GlassSettings.Core.Flyout;

public static class FlyoutEffectRegistry
{
    public const string ValueName = "EffectType";

    public static uint DefaultFlyoutEffectType =>
        Core.Registry.EffectTypeInterop.DefaultFlyoutEffectType;

    public static uint DefaultRootEffectType =>
        Core.Registry.EffectTypeInterop.DefaultRootEffectType;

    /// <summary>
    /// Только локальный DWORD в regSub (GlassRegistry).
    /// PartRegistry TryReadLocal падает на корень BWM через MakeKeyTree{"sub",""} —
    /// из‑за этого gate Effect «ложно present» после Delete Tray\Xaml\EffectType.
    /// </summary>
    public static bool IsPresent(string regSub) =>
        Core.Registry.GlassRegistry.ReadDword(regSub, ValueName).HasValue;

    public static void Delete(string regSub) =>
        Core.Registry.GlassRegistry.DeleteValue(regSub, ValueName);

    /// <summary>Gate ON: записать EffectType по умолчанию, если локального ключа нет.</summary>
    public static void EnsureEnabled(string regSub)
    {
        if (!IsPresent(regSub))
            Core.Registry.GlassRegistry.WriteDword(regSub, ValueName, DefaultFlyoutEffectType);
    }

    /// <summary>
    /// Индекс для UI (Shared::EffectTypeQuery::ReadUiEffectType).
    /// Отсутствие ключа ≠ ET=0 (Disable).
    /// </summary>
    public static uint ReadUiIndex(string regSub) =>
        Core.Registry.EffectTypeInterop.ReadUiEffectType(regSub);
}
