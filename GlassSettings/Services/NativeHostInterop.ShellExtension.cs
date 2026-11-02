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

    /// <summary>0 — пункта меню нет, 1 — классическая регистрация (Win11: «Показать дополнительные
    /// параметры»), 2 — компактное меню Windows 11 (sparse-пакет).</summary>
    public static int GetShellMenuMode() =>
        (int)Native_GetShellMenuModeV1();

    /// <summary>Доверие подписанту GlassShellSparse.msix для текущего пользователя (только чтение):
    /// 0 неизвестно/Win10, 1 доверен, 2 не доверен, 3 msix отсутствует, 4 msix без подписи.</summary>
    public static int GetShellSignerTrust(out string signerSubject)
    {
        var buffer = new char[512];
        var trust = (int)Native_GetShellSignerTrustV1(buffer, (uint)buffer.Length);
        signerSubject = new string(buffer).TrimEnd('\0');
        return trust;
    }

    [DllImport(Dll, EntryPoint = "GlassSettings_BeginShellExtensionChange")]
    private static extern bool Native_BeginShellExtensionChange(
        bool registerNotUnregister);

    [DllImport(Dll, EntryPoint = "GlassSettings_IsShellExtensionOperationInProgress")]
    private static extern bool Native_IsShellExtensionOperationInProgress();

    [DllImport(Dll, EntryPoint = "GlassSettings_GetShellExtensionLastOpMessageId")]
    private static extern uint Native_GetShellExtensionLastOpMessageId();

    [DllImport(Dll, EntryPoint = "GlassSettings_NotifyShellCachesAfterRegistrationChange")]
    private static extern void Native_NotifyShellCachesAfterRegistrationChange();

    [DllImport(Dll, EntryPoint = "GlassSettings_GetShellMenuModeV1")]
    private static extern uint Native_GetShellMenuModeV1();

    [DllImport(Dll, EntryPoint = "GlassSettings_GetShellSignerTrustV1", CharSet = CharSet.Unicode)]
    private static extern uint Native_GetShellSignerTrustV1(
        [Out] char[] subjectBuffer,
        uint cchSubject);
}
