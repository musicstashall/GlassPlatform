#pragma once

#include <Windows.h>

namespace Shared::PlatformPresenceMonitor
{
    [[nodiscard]] bool Start() noexcept;
    void Stop() noexcept;
    [[nodiscard]] bool IsRunning() noexcept;

    /// Немедленный пересчёт snapshot (после смены NoBackdrop и т.п.).
    void RequestRefresh() noexcept;

    /// Сразу публикует rootBackdropMode в mapping (без ожидания poll).
    void PublishRootBackdropMode(DWORD mode) noexcept;

    namespace ComPush
    {
        void TryRegisterScmPush() noexcept;
    }
}
