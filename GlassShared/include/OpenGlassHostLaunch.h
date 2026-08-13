#pragma once

namespace Shared::OpenGlassHostLaunch
{
    /// Запуск оркестрации OpenGlass (задача или служба). Только из хоста.
    [[nodiscard]] bool TryStartup() noexcept;

    /// Остановка оркестрации OpenGlass. Только из хоста.
    [[nodiscard]] bool TryShutdown() noexcept;
}
