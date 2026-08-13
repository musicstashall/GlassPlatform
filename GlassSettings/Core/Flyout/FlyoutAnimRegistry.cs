using GlassSettings.Core.Registry;
using GlassSettings.Core.Settings;

namespace GlassSettings.Core.Flyout;

public static class FlyoutAnimRegistry
{
    public static string AnimSubKey(string regSub) =>
        string.IsNullOrEmpty(regSub) ? "Animation" : $"{regSub}\\Animation";

    public static readonly string[] ValueNames =
    [
        "EnableFluentAnimation",
        "PopInStyle",
        "FadeOutTime",
        "PopInTime",
        "FadeInTime",
        "StartRatio",
        "ImmediateInterrupting"
    ];

    /// <summary>Видимые в UI параметры (без EnableFluentAnimation).</summary>
    public static readonly string[] VisibleValueNames =
    [
        "PopInStyle",
        "FadeOutTime",
        "PopInTime",
        "FadeInTime",
        "StartRatio",
        "ImmediateInterrupting"
    ];

    // Паритет QueryFlyoutAnimationContext (GlassCore Settings.cpp).
    public const uint DefaultPopInStyle = 0;
    public const uint DefaultFadeOutTime = 350;
    public const uint DefaultPopInTime = 250;
    public const uint DefaultFadeInTime = 87;
    public const uint DefaultStartRatio = 50;
    public const uint DefaultImmediateInterrupting = 0;

    public static uint ReadAnim(string regSub, string valueName, uint defaultValue) =>
        GlassRegistry.ReadDwordFb(AnimSubKey(regSub), valueName, defaultValue);

    public static uint ReadAnimForUi(string regSub, string valueName) =>
        ReadAnim(regSub, valueName, DefaultFor(valueName));

    public static void WriteAnim(string regSub, string valueName, uint value) =>
        GlassRegistry.WriteDword(AnimSubKey(regSub), valueName, value);

    public static void DeleteAll(string regSub) =>
        GlassRegistry.DeleteSubKeyTree(AnimSubKey(regSub));

    public static bool IsAnyPresent(string regSub) =>
        GlassSectionGate.IsAnyPresent(AnimSubKey(regSub), ValueNames);

    /// <summary>Удаляет переопределения видимых параметров; EnableFluentAnimation не трогает.</summary>
    public static void ResetVisibleParametersToDefault(string regSub)
    {
        foreach (var name in VisibleValueNames)
            GlassRegistry.DeleteValue(AnimSubKey(regSub), name);
    }

    /// <summary>Кнопка «По умолчанию» disabled, если ни один видимый ключ не переопределён.</summary>
    public static bool ShouldDisableDefaultButton(string regSub)
    {
        foreach (var name in VisibleValueNames)
        {
            if (GlassRegistry.ReadDword(AnimSubKey(regSub), name).HasValue)
                return false;
        }

        return true;
    }

    private static uint DefaultFor(string valueName) => valueName switch
    {
        "PopInStyle" => DefaultPopInStyle,
        "FadeOutTime" => DefaultFadeOutTime,
        "PopInTime" => DefaultPopInTime,
        "FadeInTime" => DefaultFadeInTime,
        "StartRatio" => DefaultStartRatio,
        "ImmediateInterrupting" => DefaultImmediateInterrupting,
        _ => 0
    };
}
