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

    /// <summary>
    /// Базовые AttachList и BlockList текущего пользователя, если ключ отсутствует
    /// или не содержит именованных значений. Уже заполненный список не меняется.
    /// Код: 0 ничего, 1 Attach, 2 Block, 3 оба, -1 сбой записи.
    /// </summary>
    public static int SeedDefaultProcessLists() =>
        Native_SeedDefaultProcessLists();

    [DllImport(Dll, EntryPoint = "GlassSettings_ValidateRegistryState")]
    private static extern int Native_ValidateRegistryState([MarshalAs(UnmanagedType.Bool)] bool applyFixes);

    [DllImport(Dll, EntryPoint = "GlassSettings_SeedDefaultProcessLists")]
    private static extern int Native_SeedDefaultProcessLists();
}
