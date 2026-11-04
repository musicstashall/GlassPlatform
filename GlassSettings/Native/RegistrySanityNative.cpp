#include "NativeHost.h"

#include <RegistrySanity.h>

// Проверка известных параметров реестра платформы (см. Shared::RegistrySanity):
// возвращает число некорректных значений. При applyFixes значения удаляются —
// умолчания в реестр не пишутся (заведённый порядок минимализма).
extern "C" __declspec(dllexport) DWORD WINAPI GlassSettings_ValidateRegistryState(BOOL applyFixes) noexcept
{
    const auto stats = Shared::RegistrySanity::Validate(applyFixes != FALSE);
    return stats.glassIssues + stats.openGlassIssues;
}
