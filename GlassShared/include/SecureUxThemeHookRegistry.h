#pragma once
#include <windows.h>

namespace Shared::SecureUxThemeHook {
// Установка/снятие IFEO winlogon и COM InProcServer32 для SecureUxTheme MSI.
// Чужие токены VerifierDlls сохраняются; файлы в System32 ставит MSI.
[[nodiscard]] bool Install() noexcept;
[[nodiscard]] bool Uninstall() noexcept;
}
