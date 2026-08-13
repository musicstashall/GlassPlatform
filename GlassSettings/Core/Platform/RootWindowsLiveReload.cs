using GlassSettings.Core.Registry;
using GlassSettings.Services;

namespace GlassSettings.Core.Platform;

/// <summary>Live-reload параметров Root-окна по реестру g_rootWindows (не flyout-канал).</summary>
public static class RootWindowsLiveReload
{
    public const int LiveRootScopeInfer = 0;
    public const int LiveRootScopeActive = 1;
    public const int LiveRootScopeInactive = 2;
    /// <summary>OR с pivot: border/gradient/corner без DWM blur reset.</summary>
    public const int LiveRootScopeCompositionOnly = 0x10000;
    /// <summary>OR с pivot: только перерисовка клиента — без border/SWCA и без RDW_FRAME (text glow).</summary>
    public const int LiveRootScopeRepaintOnly = 0x20000;

    private static int MapRegSubToScope(string? regSub) =>
        regSub switch
        {
            "Root\\Inactive" => LiveRootScopeInactive,
            "Root\\Active" => LiveRootScopeActive,
            _ => LiveRootScopeInfer,
        };

    private static int BuildLiveScope(string? regSub, bool compositionOnly, bool repaintOnly = false)
    {
        var scope = MapRegSubToScope(regSub);
        if (compositionOnly)
            scope |= LiveRootScopeCompositionOnly;
        if (repaintOnly)
            scope |= LiveRootScopeRepaintOnly;
        return scope;
    }

    /// <summary>
    /// EffectType, gradient/border color, CornerType — WM_DWMCOLORIZATIONCHANGED
    /// по g_rootWindows ∩ DidGlassApplyRootBackdrop; lParam = pivot + CompositionOnly [+ RepaintOnly].
    /// </summary>
    internal static void PostLiveRootComposition(
        string? regSub = null,
        bool compositionOnly = false,
        bool repaintOnly = false)
    {
        if (!RootBackdropModePolicy.IsFullGlassEnabled)
            return;

        NativeHostInterop.PostLiveRootColorizationToAppliedRootsIfEngineLoaded(
            BuildLiveScope(regSub, compositionOnly, repaintOnly));
    }

    public static void NotifyEffectTypeChanged(string? regSub = null) =>
        PostLiveRootComposition(regSub, compositionOnly: false);

    public static void NotifyCornerTypeChanged() =>
        PostLiveRootComposition(compositionOnly: true);

    public static void NotifyBorderColorChanged(string? regSub = null) =>
        PostLiveRootComposition(regSub, compositionOnly: true);

    public static void NotifyGradientColorChanged(string? regSub = null) =>
        PostLiveRootComposition(regSub, compositionOnly: true);

    /// <summary>Иконки контролов (Root\Control\{Kind}): только перерисовка клиентской области.</summary>
    public static void NotifyControlIconChanged() =>
        PostLiveRootComposition(compositionOnly: true, repaintOnly: true);

    /// <summary>Debounced запись alpha + invalidation chrome по tracked roots.</summary>
    public static void ScheduleDrawThemeAlphaLive(uint alpha, string regSub, string valueName) =>
        LiveReloadScheduler.ScheduleDrawThemeAlphaLive(alpha, regSub, valueName);
}
