using System.Runtime.InteropServices;
using System.Text;

namespace GlassSettings.Services;

public static partial class NativeHostInterop
{
    [StructLayout(LayoutKind.Sequential)]
    public struct PresenceSnapshot
    {
        public uint Revision;
        public int GlassState;
        public int OpenGlassOrchestration;
        public int OpenGlassPipelinePhase;
        public uint RootBackdropMode;
        public uint OpenGlassPipelineActive;
        public uint OpenGlassHostProcessRunning;
        public int OpenGlassBackend;
        public int GlassAutorunState;
        public int InjectLifecycleUi;
        public uint StopEventSignaled;
        // --- dual-arch Glass ---
        public uint GlassTaskExistsX64;
        public uint GlassTaskExistsX86;
        public int InjectLifecycleUiX64;
        public int InjectLifecycleUiX86;
        public int GlassAutorunStateX64;
        public int GlassAutorunStateX86;
        // --- OG x64 only ---
        public uint OgLegacyTaskExists;
        public uint OgCurrentTaskExists;
    }

    public static bool TryReadPresenceSnapshot(out PresenceSnapshot snapshot)
    {
        snapshot = default;
        return Native_TryReadPresenceSnapshot(ref snapshot);
    }

    public static IntPtr OpenPresenceChangedEvent() => Native_OpenPresenceChangedEvent();

    public static bool IsOpenGlassNotificationWindowPresent() =>
        Native_IsOpenGlassNotificationWindowPresent();

    public static bool IsOpenGlassBackdropPipelineActive() =>
        Native_IsOpenGlassBackdropPipelineActive();

    public static bool ShouldApplyDwmCompositionPolicy() =>
        Native_ShouldApplyDwmCompositionPolicy();

    public static bool IsOpenGlassHostProcessRunning() =>
        Native_IsOpenGlassHostProcessRunning();

    public static bool OpenGlassOrchestrationInstalled() =>
        Native_OpenGlassOrchestrationInstalled();

    public static bool TryGetOpenGlassServiceBinPath(out string? binPath)
    {
        binPath = null;
        var buffer = new StringBuilder(1024);
        if (!Native_TryGetOpenGlassServiceBinPath(buffer, (uint)buffer.Capacity))
            return false;

        binPath = buffer.ToString();
        return !string.IsNullOrWhiteSpace(binPath);
    }

    public static bool IsOpenGlassServiceRunning() =>
        Native_IsOpenGlassServiceRunning();

    public static bool IsOpenGlassServiceAutostartEnabled() =>
        Native_IsOpenGlassServiceAutostartEnabled();

    public static bool SetOpenGlassServiceAutostartEnabled(bool enable) =>
        Native_SetOpenGlassServiceAutostartEnabled(enable);

    public static bool InstallOpenGlassService(string exePath) =>
        Native_InstallOpenGlassService(exePath);

    public static bool StartOpenGlassService() => Native_StartOpenGlassService();

    public static bool StopOpenGlassService() => Native_StopOpenGlassService();

    public static bool RemoveOpenGlassService() => Native_RemoveOpenGlassService();

    public static void RequestPlatformPresenceRefresh() => Native_RequestPlatformPresenceRefresh();

    public static void NotifyRootNoBackdropChanged(uint noBackdrop) =>
        Native_NotifyRootNoBackdropChanged(noBackdrop);

    [DllImport(Dll, EntryPoint = "GlassSettings_TryReadPresenceSnapshot")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_TryReadPresenceSnapshot(ref PresenceSnapshot snapshot);

    [DllImport(Dll, EntryPoint = "GlassSettings_OpenPresenceChangedEvent")]
    private static extern IntPtr Native_OpenPresenceChangedEvent();

    [DllImport(Dll, EntryPoint = "GlassSettings_IsOpenGlassNotificationWindowPresent")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_IsOpenGlassNotificationWindowPresent();

    [DllImport(Dll, EntryPoint = "GlassSettings_IsOpenGlassBackdropPipelineActive")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_IsOpenGlassBackdropPipelineActive();

    [DllImport(Dll, EntryPoint = "GlassSettings_ShouldApplyDwmCompositionPolicy")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_ShouldApplyDwmCompositionPolicy();

    [DllImport(Dll, EntryPoint = "GlassSettings_IsOpenGlassHostProcessRunning")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_IsOpenGlassHostProcessRunning();

    [DllImport(Dll, EntryPoint = "GlassSettings_OpenGlassOrchestrationInstalled")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_OpenGlassOrchestrationInstalled();

    [DllImport(Dll, EntryPoint = "GlassSettings_TryGetOpenGlassServiceBinPath", CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_TryGetOpenGlassServiceBinPath(StringBuilder buffer, uint cchBuffer);

    [DllImport(Dll, EntryPoint = "GlassSettings_IsOpenGlassServiceRunning")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_IsOpenGlassServiceRunning();

    [DllImport(Dll, EntryPoint = "GlassSettings_IsOpenGlassServiceAutostartEnabled")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_IsOpenGlassServiceAutostartEnabled();

    [DllImport(Dll, EntryPoint = "GlassSettings_SetOpenGlassServiceAutostartEnabled")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_SetOpenGlassServiceAutostartEnabled(
        [MarshalAs(UnmanagedType.Bool)] bool enable);

    [DllImport(Dll, EntryPoint = "GlassSettings_InstallOpenGlassService", CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_InstallOpenGlassService(string exePath);

    [DllImport(Dll, EntryPoint = "GlassSettings_StartOpenGlassService")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_StartOpenGlassService();

    [DllImport(Dll, EntryPoint = "GlassSettings_StopOpenGlassService")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_StopOpenGlassService();

    [DllImport(Dll, EntryPoint = "GlassSettings_RemoveOpenGlassService")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_RemoveOpenGlassService();

    [DllImport(Dll, EntryPoint = "GlassSettings_RequestPlatformPresenceRefresh")]
    private static extern void Native_RequestPlatformPresenceRefresh();

    [DllImport(Dll, EntryPoint = "GlassSettings_NotifyRootNoBackdropChanged")]
    private static extern void Native_NotifyRootNoBackdropChanged(uint noBackdrop);
}
