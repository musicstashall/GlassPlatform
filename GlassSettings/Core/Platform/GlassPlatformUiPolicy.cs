namespace GlassSettings.Core.Platform;

/// <summary>
/// GS x64 dual UI: x86 заблокирован только без задачи и без InstallPath в Wow6432Node.
/// Задача есть — управление сохраняется даже без записи установки.
/// </summary>
public static class GlassPlatformUiPolicy
{
    public static bool IsX86DualUiControlsLocked()
    {
        if (!GlassPlatformNames.ShowDualGlassPlatformUi)
            return false;

        if (GlassPlatformService.GlassAutorunTaskExists(GlassPlatformArch.X86))
            return false;

        return !GlassModulePath.IsPlatformInstalled(GlassPlatformArch.X86);
    }

    public static bool CanCreateX86AutorunTask() =>
        !IsX86DualUiControlsLocked();
}
