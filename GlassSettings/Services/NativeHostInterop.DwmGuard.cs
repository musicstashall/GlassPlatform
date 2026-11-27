using System.Runtime.InteropServices;

namespace GlassSettings.Services;

/// <summary>P/Invoke к «сторожу» падений DWM (реализация — GlassShared::OpenGlassDwmGuard).</summary>
public static partial class NativeHostInterop
{
    /// <summary>Arm(...) возвращает true и когда наблюдение уже ведёт другой процесс (Setup).</summary>
    public static bool OpenGlassDwmGuardArm(uint armedBy, string ogVersion) =>
        Native_OpenGlassDwmGuard_Arm(armedBy, ogVersion ?? string.Empty);

    public static void OpenGlassDwmGuardDisarm() => Native_OpenGlassDwmGuard_Disarm();

    public static uint OpenGlassDwmGuardQueryState() => Native_OpenGlassDwmGuard_QueryState();

    public static bool OpenGlassDwmGuardIsWatching() => Native_OpenGlassDwmGuard_IsWatching();

    public static bool OpenGlassDwmGuardIsQuarantined() => Native_OpenGlassDwmGuard_IsQuarantined();

    /// <summary>Анти-триггер автозапуска: true — старт пропустить. При записи в маркере
    /// строго более новой версии OG карантин снимается автоматически (возвращается false).</summary>
    public static bool OpenGlassDwmGuardShouldSuppressOpenGlassStart(string installedOgVersion) =>
        Native_OpenGlassDwmGuard_ShouldSuppressOpenGlassStart(installedOgVersion ?? string.Empty);

    public static bool OpenGlassDwmGuardClearQuarantine() => Native_OpenGlassDwmGuard_ClearQuarantine();

    public static bool OpenGlassDwmGuardGetQuarantineInfo(out uint crashCount, out uint reason)
    {
        crashCount = 0;
        reason = 0;
        return Native_OpenGlassDwmGuard_GetQuarantineInfo(out crashCount, out reason);
    }

    [DllImport(Dll, EntryPoint = "GlassSettings_OpenGlassDwmGuard_Arm", CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_OpenGlassDwmGuard_Arm(uint armedBy, string ogVersion);

    [DllImport(Dll, EntryPoint = "GlassSettings_OpenGlassDwmGuard_Disarm")]
    private static extern void Native_OpenGlassDwmGuard_Disarm();

    [DllImport(Dll, EntryPoint = "GlassSettings_OpenGlassDwmGuard_QueryState")]
    private static extern uint Native_OpenGlassDwmGuard_QueryState();

    [DllImport(Dll, EntryPoint = "GlassSettings_OpenGlassDwmGuard_IsWatching")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_OpenGlassDwmGuard_IsWatching();

    [DllImport(Dll, EntryPoint = "GlassSettings_OpenGlassDwmGuard_IsQuarantined")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_OpenGlassDwmGuard_IsQuarantined();

    [DllImport(Dll, EntryPoint = "GlassSettings_OpenGlassDwmGuard_ShouldSuppressOpenGlassStart", CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_OpenGlassDwmGuard_ShouldSuppressOpenGlassStart(string installedOgVersion);

    [DllImport(Dll, EntryPoint = "GlassSettings_OpenGlassDwmGuard_ClearQuarantine")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_OpenGlassDwmGuard_ClearQuarantine();

    [DllImport(Dll, EntryPoint = "GlassSettings_OpenGlassDwmGuard_GetQuarantineInfo")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_OpenGlassDwmGuard_GetQuarantineInfo(
        out uint crashCountOut, out uint reasonOut);
}
