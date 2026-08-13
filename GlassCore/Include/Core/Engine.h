#pragma once
#include <windows.h>

namespace GlassCore::Engine
{
    bool Initialize(HMODULE module);
    /// SEH-обёртка: AV → AbortPartialInit, без повторных попыток (g_initFailed).
    bool InitializeSealed(HMODULE module) noexcept;
    /// SEH после частичного Initialize: снять Install, не ставить g_initialized.
    void AbortPartialInit() noexcept;
    [[nodiscard]] bool IsReady() noexcept;
    [[nodiscard]] bool InitFailed() noexcept;
    void Shutdown();
    /// Host UI (GlassSettings): процесс завершается — без полного teardown подсистем.
    void HostExitShutdown() noexcept;
    void Tick();
}
