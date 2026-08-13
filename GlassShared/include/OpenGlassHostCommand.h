#pragma once

#include <Windows.h>

namespace Shared::OpenGlassHostCommand
{
    inline constexpr wchar_t kCommandMappingName[] = L"Local\\GlassPlatform.OpenGlassHostCommand";
    inline constexpr wchar_t kCommandWakeEventName[] = L"GlassPlatform.OpenGlassHostCommandWake";

    enum class Command : DWORD
    {
        None = 0,
        Start = 1,
        Stop = 2,
    };

    struct Slot
    {
        volatile DWORD requestId = 0;
        volatile DWORD command = 0;
        volatile DWORD processingId = 0;
        volatile DWORD ackRequestId = 0;
        volatile DWORD success = 0;
        DWORD clientPid = 0;
    };

    [[nodiscard]] bool CreateHostCommandResources(SECURITY_ATTRIBUTES* sa) noexcept;
    void CloseHostCommandResources() noexcept;

    [[nodiscard]] HANDLE OpenCommandWakeEvent() noexcept;

    /// Клиент (GlassShellExt): только при запущенной платформе.
    [[nodiscard]] bool PostCommand(Command cmd) noexcept;

    /// Только PlatformPresenceMonitor в хосте GlassCompat.
    void ProcessPending() noexcept;
}
