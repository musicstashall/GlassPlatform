#pragma once

namespace Shared::GlassShellRegistration
{
    /// Снимает ContextMenuHandlers\GlassPlatformDesktopMenu на DesktopBackground и Directory\Background
    /// и дерево HKCU\Software\Classes\CLSID\{A91C9F80-...}.
    void RemoveLegacyContextMenuHandler() noexcept;

    /// Наш handler (CLSID legacy IContextMenu) зарегистрирован хотя бы на одном из корней.
    [[nodiscard]] bool IsLegacyContextMenuHandlerPresent() noexcept;
}
