#pragma once

namespace GlassCompat
{
    // Хост Rundll32: периодический poll из цикла сообщений (внутри — throttle).
    void HostPollExplorerShellWatchdog() noexcept;
}
