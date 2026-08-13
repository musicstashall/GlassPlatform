#include "SharedPch.h"
#include <OpenGlassHostCommand.h>
#include <OpenGlassHostLaunch.h>
#include <GlassPlatformTask.h>

namespace Shared::OpenGlassHostCommand
{
    namespace
    {
        HANDLE g_mapping = nullptr;
        HANDLE g_wakeEvent = nullptr;

        [[nodiscard]] Slot* MapSlot(DWORD desiredAccess) noexcept
        {
            HANDLE hMap = g_mapping;
            if (!hMap)
                hMap = OpenFileMappingW(desiredAccess, FALSE, kCommandMappingName);
            if (!hMap)
                return nullptr;

            auto* view = static_cast<Slot*>(MapViewOfFile(hMap, desiredAccess, 0, 0, sizeof(Slot)));
            if (hMap != g_mapping)
                CloseHandle(hMap);
            return view;
        }
    } // namespace

    bool CreateHostCommandResources(SECURITY_ATTRIBUTES* sa) noexcept
    {
        if (g_mapping)
            return true;

        g_mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            sa,
            PAGE_READWRITE,
            0,
            sizeof(Slot),
            kCommandMappingName);
        if (!g_mapping)
            return false;

        if (Slot* slot = MapSlot(FILE_MAP_WRITE))
        {
            *slot = Slot{};
            UnmapViewOfFile(slot);
        }

        g_wakeEvent = CreateEventW(sa, FALSE, FALSE, kCommandWakeEventName);
        if (!g_wakeEvent)
        {
            CloseHandle(g_mapping);
            g_mapping = nullptr;
            return false;
        }

        return true;
    }

    void CloseHostCommandResources() noexcept
    {
        // Exchange до CloseHandle: monitor мог уже закрыть dup; не double-close.
        HANDLE wake = g_wakeEvent;
        g_wakeEvent = nullptr;
        if (wake)
            CloseHandle(wake);

        HANDLE map = g_mapping;
        g_mapping = nullptr;
        if (map)
            CloseHandle(map);
    }

    HANDLE OpenCommandWakeEvent() noexcept
    {
        // Владение handle у вызывающего (wil::unique_handle / CloseHandle).
        // Нельзя возвращать g_wakeEvent напрямую — PlatformPresenceMonitor
        // закрывал host handle → CloseHostCommandResources: invalid handle.
        if (g_wakeEvent)
        {
            HANDLE dup = nullptr;
            if (DuplicateHandle(
                    GetCurrentProcess(),
                    g_wakeEvent,
                    GetCurrentProcess(),
                    &dup,
                    0,
                    FALSE,
                    DUPLICATE_SAME_ACCESS))
            {
                return dup;
            }
            return nullptr;
        }
        return OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, kCommandWakeEventName);
    }

    bool PostCommand(Command cmd) noexcept
    {
        if (cmd == Command::None)
            return false;
        if (!GlassShared::GlassPlatformTask::IsGlassPlatformRunning())
            return false;

        Slot* slot = MapSlot(FILE_MAP_WRITE);
        if (!slot)
            return false;

        (void)InterlockedIncrement(reinterpret_cast<volatile LONG*>(&slot->requestId));
        slot->clientPid = GetCurrentProcessId();
        InterlockedExchange(
            reinterpret_cast<volatile LONG*>(&slot->command),
            static_cast<LONG>(cmd));

        UnmapViewOfFile(slot);

        HANDLE hEvt = OpenCommandWakeEvent();
        if (hEvt)
        {
            SetEvent(hEvt);
            CloseHandle(hEvt);
        }

        return true;
    }

    void ProcessPending() noexcept
    {
        Slot* slot = MapSlot(FILE_MAP_WRITE);
        if (!slot)
            return;

        const DWORD cmdRaw = slot->command;
        if (cmdRaw == static_cast<DWORD>(Command::None))
        {
            UnmapViewOfFile(slot);
            return;
        }

        const DWORD id = slot->requestId;
        if (id == 0 || id == slot->ackRequestId)
        {
            UnmapViewOfFile(slot);
            return;
        }

        if (InterlockedCompareExchange(
                reinterpret_cast<volatile LONG*>(&slot->processingId),
                static_cast<LONG>(id),
                0) != 0)
        {
            UnmapViewOfFile(slot);
            return;
        }

        const Command cmd = static_cast<Command>(
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&slot->command), 0));

        UnmapViewOfFile(slot);

        bool ok = false;
        if (cmd == Command::Start)
            ok = OpenGlassHostLaunch::TryStartup();
        else if (cmd == Command::Stop)
            ok = OpenGlassHostLaunch::TryShutdown();

        slot = MapSlot(FILE_MAP_WRITE);
        if (slot)
        {
            slot->success = ok ? 1u : 0u;
            slot->ackRequestId = id;
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&slot->processingId), 0);
            UnmapViewOfFile(slot);
        }
    }
}
