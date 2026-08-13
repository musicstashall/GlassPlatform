using System.Runtime.InteropServices;
using GlassSettings.Core.Platform;
using GlassSettings.Services;
using GlassSettings.Strings;

namespace GlassSettings.Services;

public static class HomeStatusSummary
{
    public static string OpenGlassLine() =>
        MapOpenGlassState(GetOpenGlassState()) switch
        {
            OpenGlassHostUiState.Running => GlassSettingsResources.Get("Status_Running"),
            OpenGlassHostUiState.Ready => GlassSettingsResources.Get("Status_Ready"),
            OpenGlassHostUiState.Disabled => GlassSettingsResources.Get("Status_Disabled"),
            _ => GlassSettingsResources.Get("Status_NoTask")
        };

    public static string GlassPlatformLine() =>
        GlassPlatformLine(GlassPlatformNames.NativeArch);

    public static string GlassPlatformLine(GlassPlatformArch arch) =>
        QueryGlassPlatformUi(arch).Line;

    public static Windows.UI.Color OpenGlassStatusColor() =>
        PlatformStatusColors.OpenGlass(MapOpenGlassState(GetOpenGlassState()));

    public static Windows.UI.Color GlassPlatformStatusColor() =>
        GlassPlatformStatusColor(GlassPlatformNames.NativeArch);

    public static Windows.UI.Color GlassPlatformStatusColor(GlassPlatformArch arch) =>
        QueryGlassPlatformUi(arch).DotColor;

    public static bool IsGlassPlatformUnloading(GlassPlatformArch arch) =>
        QueryGlassPlatformUi(arch).IsUnloading;

    /// <summary>
    /// Один snapshot life+autorun: текст/точка/блок тумблера не разъезжаются
    /// (лог H6: unloading=true при life=Idle из‑за двух QueryInjectLifecycleUi подряд).
    /// </summary>
    public readonly record struct GlassPlatformUiSnapshot(
        NativeHostInterop.InjectLifecycleUi Life,
        GlassAutorunUiState Autorun,
        bool IsUnloading,
        bool LifecycleBusy,
        string Line,
        Windows.UI.Color DotColor);

    public static GlassPlatformUiSnapshot QueryGlassPlatformUi(GlassPlatformArch arch)
    {
        var life = NativeHostInterop.QueryInjectLifecycleUi(arch);
        // Live Query: Presence snapshot для NativeArch после stop залипает на Running.
        var autorun = GlassPlatformService.QueryGlassAutorunUiState(arch);
        var stopSignaled = IsStopEventSignaled(arch);
        // Residual или Stopping из‑за StopEvent — не LifecycleBusy на старте.
        var unloading = life == NativeHostInterop.InjectLifecycleUi.Residual
            || (life == NativeHostInterop.InjectLifecycleUi.Stopping && stopSignaled);
        var lifeBusy = life is NativeHostInterop.InjectLifecycleUi.Stopping
            or NativeHostInterop.InjectLifecycleUi.Residual;

        string line;
        Windows.UI.Color dot;
        if (unloading)
        {
            line = GlassSettingsResources.Get("Status_Unloading");
            dot = PlatformStatusColors.GlassPlatformUnloading;
        }
        else
        {
            line = autorun switch
            {
                GlassAutorunUiState.Running => GlassSettingsResources.Get("Status_Running"),
                GlassAutorunUiState.Ready => GlassSettingsResources.Get("Status_Ready"),
                GlassAutorunUiState.Disabled => GlassSettingsResources.Get("Status_Disabled"),
                _ => GlassSettingsResources.Get("Status_NoTask")
            };
            dot = PlatformStatusColors.GlassPlatform(autorun);
        }

        return new GlassPlatformUiSnapshot(life, autorun, unloading, lifeBusy, line, dot);
    }

    private static bool IsStopEventSignaled(GlassPlatformArch arch)
    {
        var h = OpenEventW(0x00100000 /* SYNCHRONIZE */, false, GlassPlatformNames.StopEventFor(arch));
        if (h == IntPtr.Zero)
            return false;
        try
        {
            return WaitForSingleObject(h, 0) == 0;
        }
        finally
        {
            CloseHandle(h);
        }
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr OpenEventW(
        uint dwDesiredAccess,
        [MarshalAs(UnmanagedType.Bool)] bool bInheritHandle,
        string lpName);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr hObject);

    public static string PlatformLine()
    {
        return MapGlassState(GetGlassState()) switch
        {
            GlassAutorunUiState.Running => GlassSettingsResources.Get("Status_BwmRunning"),
            GlassAutorunUiState.Ready => GlassSettingsResources.Get("Status_BwmReady"),
            GlassAutorunUiState.Disabled => GlassSettingsResources.Get("Status_BwmDisabled"),
            _ => GlassSettingsResources.Get("Status_BwmNoTask")
        };
    }

    private static int GetOpenGlassState()
    {
        var snap = PlatformPresenceService.Instance.CurrentSnapshot;
        if (snap.Revision > 0)
            return snap.OpenGlassOrchestration;
        return (int)OpenGlassHostService.QueryOpenGlassHostUiState();
    }

    private static int GetGlassState() => GetGlassState(GlassPlatformNames.NativeArch);

    private static int GetGlassState(GlassPlatformArch arch)
    {
        // Dual-arch: presence snapshot хранит только native; чужую битность читаем напрямую.
        if (arch == GlassPlatformNames.NativeArch)
        {
            var snap = PlatformPresenceService.Instance.CurrentSnapshot;
            if (snap.Revision > 0)
                return snap.GlassState;
        }

        return (int)GlassPlatformService.QueryGlassAutorunUiState(arch);
    }

    private static OpenGlassHostUiState MapOpenGlassState(int value) =>
        (OpenGlassHostUiState)value;

    private static GlassAutorunUiState MapGlassState(int value) =>
        (GlassAutorunUiState)value;

    /// <summary>Строки 2–3 шапки карточки устройства: модель и «24H2 · сборка … · x64».</summary>
    public static string DeviceHeaderBody()
    {
        var model = TryReadComputerModel();
        if (string.IsNullOrWhiteSpace(model))
            model = Environment.MachineName;

        var releaseLine = BuildReleaseRuntimeLine();
        return string.IsNullOrEmpty(releaseLine) ? model : $"{model}\n{releaseLine}";
    }

    private static string BuildReleaseRuntimeLine()
    {
        var release = TryReadDisplayVersion();
        var build = TryReadBuildNumber();
        var arch = Environment.Is64BitOperatingSystem ? "x64" : "x86";

        if (string.IsNullOrEmpty(release) && string.IsNullOrEmpty(build))
            return arch;

        if (string.IsNullOrEmpty(release))
            return GlassSettingsResources.Format("Home_RuntimeArchBuild", arch, build ?? string.Empty);

        if (string.IsNullOrEmpty(build))
            return $"{release} · {arch}";

        return GlassSettingsResources.Format("Home_DeviceReleaseLine", release, build, arch);
    }

    private static string? TryReadComputerModel()
    {
        try
        {
            using var biosKey = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"HARDWARE\DESCRIPTION\System\BIOS");
            var fromBios = biosKey?.GetValue("SystemProductName") as string;
            if (!string.IsNullOrWhiteSpace(fromBios))
                return fromBios.Trim();

            using var sysKey = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\SystemInformation");
            var fromSys = sysKey?.GetValue("SystemProductName") as string;
            return string.IsNullOrWhiteSpace(fromSys) ? null : fromSys.Trim();
        }
        catch
        {
            return null;
        }
    }

    private static string? TryReadBuildNumber()
    {
        try
        {
            using var key = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion");
            return key?.GetValue("CurrentBuildNumber") as string;
        }
        catch
        {
            return null;
        }
    }

    private static string? TryReadDisplayVersion()
    {
        try
        {
            using var key = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion");
            var display = key?.GetValue("DisplayVersion") as string;
            return string.IsNullOrWhiteSpace(display) ? null : display.Trim();
        }
        catch
        {
            return null;
        }
    }

    public static string FlyoutLine()
    {
        var eff = Core.Flyout.FlyoutEffectRegistry.ReadUiIndex("");
        return GlassSettingsResources.Format("Home_FlyoutLine", Core.Flyout.FlyoutEffectNames.Get(eff));
    }

    public static string TaskbarLine()
    {
        var paintXaml = Core.Flyout.FlyoutPartRegistry.ReadEffectiveDword("Tray", "BorderDrawMode", 0) != 0;
        var mode = paintXaml
            ? GlassSettingsResources.Get("Param_ModeXaml")
            : GlassSettingsResources.Get("Param_ModeDwmSwca");
        var eff = paintXaml
            ? Core.Flyout.FlyoutXamlEffectNames.Get(
                Core.Flyout.FlyoutPartRegistry.TryReadLocalDword("Tray\\Xaml", "EffectType", out var xamlEff)
                    ? xamlEff
                    : 0u)
            : Core.Flyout.FlyoutEffectNames.Get(Core.Flyout.FlyoutEffectRegistry.ReadUiIndex("Tray"));
        return GlassSettingsResources.Format("Home_TaskbarLine", mode, eff);
    }

    public static uint TrayGradientArgb() =>
        Core.Flyout.FlyoutThemeRegistry.ReadGradientArgbForUi("Tray");
}
