namespace GlassSettings.Core.Platform;

public enum GlassPlatformArch
{
    X86 = 0,
    X64 = 1,
}

public static class GlassPlatformNames
{
    public const string AutorunTaskNameX64 = "GlassPlatform.Win32 Autorun Task (x64)";
    public const string AutorunTaskNameX86 = "GlassPlatform.Win32 Autorun Task (x86)";

    public static string InstanceMutexFor(GlassPlatformArch arch) =>
        arch == GlassPlatformArch.X64
            ? "GlassPlatform.Instance.Mutex.x64"
            : "GlassPlatform.Instance.Mutex.x86";

    public static string InstanceMutex => InstanceMutexFor(NativeArch);

    /// <summary>StopEvent хоста (без Local\) — выгрузка vs LifecycleBusy на старте.</summary>
    public static string StopEventFor(GlassPlatformArch arch) =>
        arch == GlassPlatformArch.X64
            ? "GlassPlatform.StopEvent.x64"
            : "GlassPlatform.StopEvent.x86";

    public const string ReloadRenderSemaphore = "GlassPlatform.ReloadRenderSemaphore";

    public const string ReloadGenerationMapping = "Local\\GlassPlatform.ReloadGeneration";

    public const string ReloadRenderFlagsMapping = "Local\\GlassPlatform.ReloadRenderFlags";

    public const string ReloadRootLiveScopeMapping = "Local\\GlassPlatform.ReloadRootLiveScope";

    public static GlassPlatformArch NativeArch =>
        Environment.Is64BitProcess ? GlassPlatformArch.X64 : GlassPlatformArch.X86;

    public static string GlassAutorunTaskName => GlassAutorunTaskNameFor(NativeArch);

    public static string GlassAutorunTaskNameFor(GlassPlatformArch arch) =>
        arch == GlassPlatformArch.X64 ? AutorunTaskNameX64 : AutorunTaskNameX86;

    /// <summary>Dual UI: x64 Settings на OS x64.</summary>
    public static bool ShowDualGlassPlatformUi =>
        Environment.Is64BitOperatingSystem && Environment.Is64BitProcess;
}
