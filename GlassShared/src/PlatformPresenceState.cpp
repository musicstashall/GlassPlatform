#include "SharedPch.h"
#include <PlatformPresenceState.h>

#include <atomic>

namespace Shared::PlatformPresenceState
{
    namespace
    {
        static HANDLE s_mapping = nullptr;
        static HANDLE s_changedEvent = nullptr;
        static std::atomic<DWORD> s_hostRevision{ 0 };

        [[nodiscard]] Snapshot* MapSnapshotView(DWORD desiredAccess) noexcept
        {
            HANDLE hMap = s_mapping;
            if (!hMap)
                hMap = OpenFileMappingW(desiredAccess, FALSE, kPresenceStateMappingName);
            if (!hMap)
                return nullptr;

            auto* view = static_cast<Snapshot*>(MapViewOfFile(
                hMap, desiredAccess, 0, 0, sizeof(Snapshot)));
            if (hMap != s_mapping)
                CloseHandle(hMap);
            return view;
        }
    } // namespace

    bool CreateHostPresenceResources(SECURITY_ATTRIBUTES* sa) noexcept
    {
        if (s_mapping)
            return true;

        s_mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            sa,
            PAGE_READWRITE,
            0,
            sizeof(Snapshot),
            kPresenceStateMappingName);
        if (!s_mapping)
            return false;

        if (Snapshot* view = MapSnapshotView(FILE_MAP_WRITE))
        {
            *view = Snapshot{};
            UnmapViewOfFile(view);
        }

        s_changedEvent = CreateEventW(sa, FALSE, FALSE, kPresenceChangedEventName);
        if (!s_changedEvent)
        {
            CloseHandle(s_mapping);
            s_mapping = nullptr;
            return false;
        }

        return true;
    }

    void CloseHostPresenceResources() noexcept
    {
        if (s_changedEvent)
        {
            CloseHandle(s_changedEvent);
            s_changedEvent = nullptr;
        }
        if (s_mapping)
        {
            CloseHandle(s_mapping);
            s_mapping = nullptr;
        }
    }

    bool TryReadSnapshot(Snapshot& out) noexcept
    {
        Snapshot* view = MapSnapshotView(FILE_MAP_READ);
        if (!view)
            return false;
        out = *view;
        UnmapViewOfFile(view);
        return true;
    }

    bool TryWriteSnapshot(const Snapshot& in, bool signalChanged) noexcept
    {
        Snapshot* view = MapSnapshotView(FILE_MAP_WRITE);
        if (!view)
            return false;

        Snapshot next = in;
        next.revision = s_hostRevision.fetch_add(1, std::memory_order_relaxed) + 1;
        *view = next;
        UnmapViewOfFile(view);

        if (signalChanged)
        {
            HANDLE hEvt = s_changedEvent;
            if (!hEvt)
                hEvt = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, kPresenceChangedEventName);
            if (hEvt)
            {
                SetEvent(hEvt);
                if (hEvt != s_changedEvent)
                    CloseHandle(hEvt);
            }
        }

        return true;
    }

    HANDLE OpenPresenceChangedEvent() noexcept
    {
        HANDLE hEvt = s_changedEvent;
        if (hEvt)
            return hEvt;
        return OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, kPresenceChangedEventName);
    }

    void PulsePresenceChanged() noexcept
    {
        HANDLE hEvt = s_changedEvent;
        if (!hEvt)
            hEvt = OpenEventW(EVENT_MODIFY_STATE, FALSE, kPresenceChangedEventName);
        if (!hEvt)
            return;
        SetEvent(hEvt);
        if (hEvt != s_changedEvent)
            CloseHandle(hEvt);
    }
}
