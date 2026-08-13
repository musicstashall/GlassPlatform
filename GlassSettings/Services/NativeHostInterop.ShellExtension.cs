using System.Runtime.InteropServices;

namespace GlassSettings.Services;

public static partial class NativeHostInterop
{
    public static bool BeginShellExtensionChange(bool register) =>
        Native_BeginShellExtensionChange(register);

    public static bool IsShellExtensionOperationInProgress() =>
        Native_IsShellExtensionOperationInProgress();

    public static int GetShellExtensionLastOpMessageId() =>
        (int)Native_GetShellExtensionLastOpMessageId();

    public static void NotifyShellCachesAfterRegistrationChange() =>
        Native_NotifyShellCachesAfterRegistrationChange();

    [DllImport(Dll, EntryPoint = "GlassSettings_BeginShellExtensionChange")]
    private static extern bool Native_BeginShellExtensionChange(
        bool registerNotUnregister);

    [DllImport(Dll, EntryPoint = "GlassSettings_IsShellExtensionOperationInProgress")]
    private static extern bool Native_IsShellExtensionOperationInProgress();

    [DllImport(Dll, EntryPoint = "GlassSettings_GetShellExtensionLastOpMessageId")]
    private static extern uint Native_GetShellExtensionLastOpMessageId();

    [DllImport(Dll, EntryPoint = "GlassSettings_NotifyShellCachesAfterRegistrationChange")]
    private static extern void Native_NotifyShellCachesAfterRegistrationChange();
}
