#pragma once

#include <cstddef>

namespace Shared::GlassShellRegistration
{
    /// Классическая (Win32 IContextMenu) регистрация нашего handler для текущего пользователя:
    /// HKCU\Software\Classes\CLSID\{A91C9F80-...}\InprocServer32 = modulePath (ThreadingModel=Apartment) и
    /// DesktopBackground\ShellEx\ContextMenuHandlers\GlassPlatformDesktopMenu = CLSID.
    /// Единственная точка записи этих ключей: regsvr32 (DllRegisterServer, Win10) и GlassSettings.Native
    /// (Win11 — запасной путь, когда sparse-пакет зарегистрировать нельзя). Без SHChangeNotify.
    [[nodiscard]] bool RegisterLegacyContextMenuHandler(const wchar_t* modulePath) noexcept;

    /// Снимает ContextMenuHandlers\GlassPlatformDesktopMenu на DesktopBackground и Directory\Background
    /// и дерево HKCU\Software\Classes\CLSID\{A91C9F80-...}.
    void RemoveLegacyContextMenuHandler() noexcept;

    /// Снимает устаревшую HKCU-регистрацию IExplorerCommand (verbs toggle/config на DesktopBackground и
    /// Directory\Background, CLSID toggle/config): на Win11 эти пункты объявляет только sparse-манифест.
    void RemoveStaleExplorerCommandRegistration() noexcept;

    /// Наш handler (CLSID legacy IContextMenu) зарегистрирован хотя бы на одном из корней.
    [[nodiscard]] bool IsLegacyContextMenuHandlerPresent() noexcept;

    /// Путь модуля из HKCU InprocServer32 нашего CLSID (как записан; без раскрытия переменных окружения).
    /// false — значение отсутствует или пусто.
    [[nodiscard]] bool TryGetLegacyContextMenuHandlerModule(wchar_t* buf, size_t cchChars) noexcept;
}
