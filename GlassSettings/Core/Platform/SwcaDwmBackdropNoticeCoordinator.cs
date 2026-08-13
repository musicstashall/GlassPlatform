using GlassSettings.Core.Customization;
using GlassSettings.Core.Flyout;
using GlassSettings.Services;
using GlassSettings.Strings;

namespace GlassSettings.Core.Platform;

/// <summary>Баннер ET 6–8 (DWM Acrylic/Mica) при SWCA backdrop.</summary>
public static class SwcaDwmBackdropNoticeCoordinator
{
    public const uint DwmBackdropEffectTypeMin = 6;
    public const uint DwmBackdropEffectTypeMax = 8;

    private const string RootRegSub = "Root";
    private const string RootActiveRegSub = "Root\\Active";
    private const string RootInactiveRegSub = "Root\\Inactive";
    private const string FlyoutBwmRootRegSub = "";

    private static readonly (string RegSub, string KindLabelResourceKey)[] PopupFlyoutKinds =
    [
        ("Menu", "Notice_SwcaDwmBackdropPopupKindMenu"),
        ("DropDown", "Notice_SwcaDwmBackdropPopupKindDropDown"),
        ("ToolTip", "Notice_SwcaDwmBackdropPopupKindToolTip"),
    ];

    private static bool _presenceHooked;
    private static bool _lastSwcaActive;

    public static event EventHandler? Changed;

    public static void AttachPresenceListener()
    {
        if (_presenceHooked)
            return;

        _presenceHooked = true;
        _lastSwcaActive = RootBackdropModePolicy.IsSwcaBackdropActive();
        PlatformPresenceService.Instance.PresenceChanged += OnPresenceChanged;
    }

    public static bool IsDwmSystemBackdropEffectType(uint effectType) =>
        effectType >= DwmBackdropEffectTypeMin && effectType <= DwmBackdropEffectTypeMax;

    public static bool ShouldShowGradientTintLimitNotice(string regSub) =>
        RootBackdropModePolicy.IsSwcaBackdropActive()
        && IsDwmSystemBackdropEffectType(FlyoutEffectRegistry.ReadUiIndex(regSub));

    public static bool ShouldShowRootGradientTintLimitNotice()
    {
        if (!RootBackdropModePolicy.IsSwcaBackdropActive())
            return false;

        return RegSubEffectiveHasDwmBackdropEffect(RootRegSub)
               || RegSubEffectiveHasDwmBackdropEffect(RootActiveRegSub)
               || RegSubEffectiveHasDwmBackdropEffect(RootInactiveRegSub);
    }

    public static string FormatRootGradientTintLimitNoticeMessage()
    {
        var scopePhrase = ResolveRootDwmBackdropScopePhrase();
        return string.Format(
            GlassSettingsResources.Get("Notice_SwcaDwmBackdropBodyRootFormat"),
            scopePhrase);
    }

    public static bool ShouldShowPopupGradientTintLimitNotice()
    {
        if (!RootBackdropModePolicy.IsSwcaBackdropActive())
            return false;

        foreach (var (regSub, _) in PopupFlyoutKinds)
        {
            if (PopupKindHasDwmBackdropLimit(regSub))
                return true;
        }

        return FlyoutEffectRegistry.IsPresent(FlyoutBwmRootRegSub)
               && RegSubEffectiveHasDwmBackdropEffect(FlyoutBwmRootRegSub);
    }

    public static string FormatPopupGradientTintLimitNoticeMessage()
    {
        var kindLabels = ResolvePopupDwmBackdropKindLabels();
        var scopePhrase = string.Format(
            GlassSettingsResources.Get("Notice_SwcaDwmBackdropScopePopupForKinds"),
            kindLabels);
        return string.Format(
            GlassSettingsResources.Get("Notice_SwcaDwmBackdropBodyRootFormat"),
            scopePhrase);
    }

    public static void NotifyEffectOrScopeChanged() => Changed?.Invoke(null, EventArgs.Empty);

    private static bool RegSubEffectiveHasDwmBackdropEffect(string regSub) =>
        IsDwmSystemBackdropEffectType(FlyoutEffectRegistry.ReadUiIndex(regSub));

    private static bool PopupKindHasDwmBackdropLimit(string kindRegSub)
    {
        if (FlyoutEffectRegistry.IsPresent(kindRegSub))
            return RegSubEffectiveHasDwmBackdropEffect(kindRegSub);

        if (FlyoutEffectRegistry.IsPresent(FlyoutBwmRootRegSub))
            return RegSubEffectiveHasDwmBackdropEffect(FlyoutBwmRootRegSub);

        return false;
    }

    private static string ResolvePopupDwmBackdropKindLabels()
    {
        var labels = new List<string>();
        foreach (var (regSub, labelKey) in PopupFlyoutKinds)
        {
            if (PopupKindHasDwmBackdropLimit(regSub))
                labels.Add(GlassSettingsResources.Get(labelKey));
        }

        return string.Join(", ", labels);
    }

    private static string ResolveRootDwmBackdropScopePhrase()
    {
        if (!CustomizationCatalog.IsRootPerWindowStateScope())
        {
            return GlassSettingsResources.Get("Notice_SwcaDwmBackdropScopeBoth");
        }

        var rootHas = RegSubEffectiveHasDwmBackdropEffect(RootRegSub);
        var activeHas = RegSubEffectiveHasDwmBackdropEffect(RootActiveRegSub);
        var inactiveHas = RegSubEffectiveHasDwmBackdropEffect(RootInactiveRegSub);

        if (rootHas || (activeHas && inactiveHas))
            return GlassSettingsResources.Get("Notice_SwcaDwmBackdropScopeBoth");

        if (activeHas)
            return GlassSettingsResources.Get("Notice_SwcaDwmBackdropScopeActive");

        return GlassSettingsResources.Get("Notice_SwcaDwmBackdropScopeInactive");
    }

    private static void OnPresenceChanged(object? sender, EventArgs e)
    {
        var now = RootBackdropModePolicy.IsSwcaBackdropActive();
        if (now == _lastSwcaActive)
            return;

        _lastSwcaActive = now;
        Changed?.Invoke(null, EventArgs.Empty);
    }
}
