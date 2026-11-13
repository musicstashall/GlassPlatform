#pragma once

namespace GlassCompat
{
    // Хост Rundll32: poll из цикла сообщений (throttle 2 с).
    // Три падения Explorer за 3 минуты или два исчезновения оболочки — один диалог.
    // Нет ответа 20 с или «Да» — остановка. «Нет» сбрасывает серию.
    void HostPollExplorerShellWatchdog() noexcept;
}
