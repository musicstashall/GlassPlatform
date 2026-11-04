using System.Runtime.InteropServices;

namespace GlassSettings.Services;

public static partial class NativeHostInterop
{
    /// <summary>
    /// Проверка известных параметров реестра платформы (Shared::RegistrySanity):
    /// неверный тип или выход из ограниченного диапазона → значение некорректно.
    /// При applyFixes значения удаляются (умолчания в реестр не пишутся).
    /// Возвращает число некорректных значений.
    /// </summary>
    public static int ValidateRegistryState(bool applyFixes) =>
        Native_ValidateRegistryState(applyFixes);

    [DllImport(Dll, EntryPoint = "GlassSettings_ValidateRegistryState")]
    private static extern int Native_ValidateRegistryState([MarshalAs(UnmanagedType.Bool)] bool applyFixes);
}
