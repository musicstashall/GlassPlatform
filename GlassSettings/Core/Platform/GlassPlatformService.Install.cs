using GlassSettings.Services;

namespace GlassSettings.Core.Platform;

public static partial class GlassPlatformService
{
    /// <summary>Создаёт задачу автозапуска Glass (native arch).</summary>
    public static bool TryInstallGlassAutorunTask() =>
        TryInstallGlassAutorunTask(GlassPlatformNames.NativeArch);

    public static bool TryInstallGlassAutorunTask(GlassPlatformArch arch)
    {
        var dll = ResolveGlassCompatDllPath(arch);
        if (dll is null) return false;

        var rundll = GlassModulePath.GetRundll32Path(arch);
        var taskName = GlassPlatformNames.GlassAutorunTaskNameFor(arch);
        var tr = $"\"{rundll}\" \"{dll}\",Main /start";
        var args = $"/Create /TN \"{taskName}\" /TR \"{tr}\" /SC ONLOGON /RL HIGHEST /F";
        var (code, _) = RunSchtasks(args);
        if (code != 0 || !GlassAutorunTaskExists(arch))
            return false;

        // schtasks /Create не умеет выставлять флаги питания: задача создаётся с
        // StopIfGoingOnBatteries=true и убивается при переходе на батарею.
        _ = TryEnsureAutorunTaskPowerIndependent(arch);
        return true;
    }

    /// <summary>
    /// Снимает зависимость задачи от питания и простоя (батарея, idle, лимит выполнения).
    /// Перерегистрация требует прав администратора.
    /// </summary>
    public static bool TryEnsureAutorunTaskPowerIndependent(GlassPlatformArch arch)
    {
        if (!GlassAutorunTaskExists(arch))
            return false;
        if (NativeHostInterop.IsAutorunTaskPowerIndependent(arch))
            return true;
        return NativeHostInterop.ApplyAutorunTaskPowerIndependence(arch);
    }

    /// <summary>Остановка платформы и удаление задачи (паритет RemoveGlassAutorunTaskOnWorker).</summary>
    public static bool TryRemoveGlassAutorunTaskWithStop() =>
        TryRemoveGlassAutorunTaskWithStop(GlassPlatformNames.NativeArch);

    public static bool TryRemoveGlassAutorunTaskWithStop(GlassPlatformArch arch)
    {
        NativeHostInterop.SignalPlatformStop(arch);
        NativeHostInterop.WaitForPlatformStopped(arch, DefaultPlatformShutdownWaitMs);
        _ = RunSchtasks($"/End /TN \"{GlassPlatformNames.GlassAutorunTaskNameFor(arch)}\"");
        return RemoveGlassAutorunTask(arch);
    }

    public static bool TryEnsureAutorunTaskEnabled() =>
        TryEnsureAutorunTaskEnabled(GlassPlatformNames.NativeArch);

    public static bool TryEnsureAutorunTaskEnabled(GlassPlatformArch arch)
    {
        if (!GlassAutorunTaskExists(arch))
            return TryInstallGlassAutorunTask(arch);
        _ = TryEnsureAutorunTaskPowerIndependent(arch);
        return SetGlassAutorunTaskSchedulerEnabled(arch, true);
    }

    /// <summary>
    /// Перезаписать /TR задачи текущим Resolve (корень GS x86 ≠ чужой Sys32 от прошлой установки).
    /// </summary>
    public static bool TrySyncAutorunTaskActionPath(GlassPlatformArch arch)
    {
        if (ResolveGlassCompatDllPath(arch) is null)
            return false;
        return TryInstallGlassAutorunTask(arch);
    }

    private static string? ResolveGlassCompatDllPath(GlassPlatformArch arch) =>
        GlassModulePath.Resolve("GlassCompat.dll", arch);
}
