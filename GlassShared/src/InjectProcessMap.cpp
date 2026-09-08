#include "SharedPch.h"
#include <InjectProcessMap.h>

#include <cstring>

namespace Shared::InjectProcessMap
{
    namespace
    {
        static HANDLE s_mapping = nullptr;

        [[nodiscard]] Header* MapHeaderViewNamed(const wchar_t* mappingName, DWORD desiredAccess) noexcept
        {
            if (!mappingName || !mappingName[0])
                return nullptr;

            HANDLE hMap = nullptr;
            if (s_mapping && mappingName == kMappingName)
                hMap = s_mapping;
            else if (s_mapping && wcscmp(mappingName, kMappingName) == 0)
                hMap = s_mapping;

            HANDLE opened = nullptr;
            if (!hMap)
            {
                opened = OpenFileMappingW(desiredAccess, FALSE, mappingName);
                hMap = opened;
            }
            if (!hMap)
                return nullptr;

            auto* view = static_cast<Header*>(MapViewOfFile(
                hMap, desiredAccess, 0, 0, sizeof(Header)));
            if (opened)
                CloseHandle(opened);
            return view;
        }

        [[nodiscard]] Header* MapHeaderView(DWORD desiredAccess) noexcept
        {
            return MapHeaderViewNamed(kMappingName, desiredAccess);
        }

        void BeginSlotWrite(Slot& slot) noexcept
        {
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&slot.seq));
            MemoryBarrier();
        }

        void EndSlotWrite(Slot& slot) noexcept
        {
            MemoryBarrier();
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&slot.seq));
        }

        [[nodiscard]] bool ReadSlotStable(const Slot& src, Slot& dst) noexcept
        {
            for (int attempt = 0; attempt < 8; ++attempt)
            {
                const LONG s1 = src.seq;
                if ((s1 & 1) != 0)
                    continue;
                MemoryBarrier();
                dst = src;
                MemoryBarrier();
                const LONG s2 = src.seq;
                if (s1 == s2 && (s2 & 1) == 0)
                    return true;
            }
            return false;
        }

        void CopyExeName(wchar_t* dest, const wchar_t* src) noexcept
        {
            if (!dest)
                return;
            if (!src || !src[0])
            {
                dest[0] = L'\0';
                return;
            }
            wcsncpy_s(dest, kExeNameChars, src, _TRUNCATE);
        }

        [[nodiscard]] Slot* FindSlotByPid(Header* header, DWORD pid) noexcept
        {
            if (!header || pid == 0)
                return nullptr;
            for (DWORD i = 0; i < kSlotCount; ++i)
            {
                if (static_cast<DWORD>(InterlockedCompareExchange(
                        reinterpret_cast<volatile LONG*>(&header->slots[i].pid),
                        0,
                        0)) == pid)
                    return &header->slots[i];
            }
            return nullptr;
        }

        [[nodiscard]] Slot* ClaimFreeSlot(Header* header, DWORD pid) noexcept
        {
            if (!header || pid == 0)
                return nullptr;
            for (DWORD i = 0; i < kSlotCount; ++i)
            {
                Slot& slot = header->slots[i];
                if (InterlockedCompareExchange(
                        reinterpret_cast<volatile LONG*>(&slot.pid),
                        static_cast<LONG>(pid),
                        0) == 0)
                    return &slot;
            }
            return nullptr;
        }

        [[nodiscard]] bool ReadSlotsFromHeader(
            Header* header,
            Slot* outSlots,
            DWORD maxSlots,
            DWORD* outCount) noexcept
        {
            if (!header || !outSlots || maxSlots == 0 || !outCount)
                return false;
            *outCount = 0;

            if (header->magic != kMagic || header->version != kVersion)
                return false;

            DWORD written = 0;
            for (DWORD i = 0; i < kSlotCount && written < maxSlots; ++i)
            {
                Slot stable{};
                if (!ReadSlotStable(header->slots[i], stable))
                    continue;
                if (stable.pid == 0 || stable.state == State::Empty)
                    continue;
                outSlots[written++] = stable;
            }
            *outCount = written;
            return true;
        }
    } // namespace

    bool CreateHostResources(SECURITY_ATTRIBUTES* sa) noexcept
    {
        if (s_mapping)
            return true;

        s_mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            sa,
            PAGE_READWRITE,
            0,
            sizeof(Header),
            kMappingName);
        if (!s_mapping)
            return false;

        if (Header* view = MapHeaderView(FILE_MAP_WRITE))
        {
            ZeroMemory(view, sizeof(Header));
            view->magic = kMagic;
            view->version = kVersion;
            view->slotCount = kSlotCount;
            UnmapViewOfFile(view);
        }
        return true;
    }

    void CloseHostResources() noexcept
    {
        if (s_mapping)
        {
            CloseHandle(s_mapping);
            s_mapping = nullptr;
        }
    }

    bool TryUpsert(
        DWORD pid,
        Kind kind,
        std::uint8_t path,
        State state,
        std::uint32_t moduleBits,
        const wchar_t* exeName) noexcept
    {
        if (pid == 0)
            return false;

        Header* header = MapHeaderView(FILE_MAP_WRITE);
        if (!header)
            return false;

        if (header->magic != kMagic || header->version != kVersion)
        {
            UnmapViewOfFile(header);
            return false;
        }

        Slot* slot = FindSlotByPid(header, pid);
        if (!slot)
            slot = ClaimFreeSlot(header, pid);
        if (!slot)
        {
            UnmapViewOfFile(header);
            return false;
        }

        BeginSlotWrite(*slot);
        slot->kind = kind;
        slot->path = path;
        slot->state = state;
        slot->moduleBits = moduleBits;
        CopyExeName(slot->exeName, exeName);
        EndSlotWrite(*slot);

        UnmapViewOfFile(header);
        return true;
    }

    bool TrySetState(DWORD pid, State state) noexcept
    {
        Header* header = MapHeaderView(FILE_MAP_WRITE);
        if (!header)
            return false;

        Slot* slot = FindSlotByPid(header, pid);
        if (!slot)
        {
            UnmapViewOfFile(header);
            return false;
        }

        BeginSlotWrite(*slot);
        slot->state = state;
        EndSlotWrite(*slot);
        UnmapViewOfFile(header);
        return true;
    }

    bool TrySetModuleBits(DWORD pid, std::uint32_t moduleBits) noexcept
    {
        Header* header = MapHeaderView(FILE_MAP_WRITE);
        if (!header)
            return false;

        Slot* slot = FindSlotByPid(header, pid);
        if (!slot)
        {
            UnmapViewOfFile(header);
            return false;
        }

        BeginSlotWrite(*slot);
        slot->moduleBits = moduleBits;
        EndSlotWrite(*slot);
        UnmapViewOfFile(header);
        return true;
    }

    bool TryGetModuleBits(DWORD pid, std::uint32_t& outBits) noexcept
    {
        outBits = 0;
        if (pid == 0)
            return false;

        Header* header = MapHeaderView(FILE_MAP_READ);
        if (!header)
            return false;

        Slot* slot = FindSlotByPid(header, pid);
        if (!slot || slot->pid != pid)
        {
            UnmapViewOfFile(header);
            return false;
        }

        outBits = slot->moduleBits;
        UnmapViewOfFile(header);
        return true;
    }

    bool TrySetUnloadResult(DWORD pid, std::uint32_t unloadResult) noexcept
    {
        Header* header = MapHeaderView(FILE_MAP_WRITE);
        if (!header)
            return false;

        Slot* slot = FindSlotByPid(header, pid);
        if (!slot)
        {
            UnmapViewOfFile(header);
            return false;
        }

        BeginSlotWrite(*slot);
        slot->unloadResult = unloadResult;
        EndSlotWrite(*slot);
        UnmapViewOfFile(header);
        return true;
    }

    bool TrySetDiagFlag(DWORD pid, std::uint32_t flag, bool active) noexcept
    {
        if (pid == 0 || flag == 0)
            return false;

        Header* header = MapHeaderView(FILE_MAP_WRITE);
        if (!header)
            return false;

        Slot* slot = FindSlotByPid(header, pid);
        if (!slot)
        {
            UnmapViewOfFile(header);
            return false;
        }

        BeginSlotWrite(*slot);
        if (active)
            slot->diagFlags |= flag;
        else
            slot->diagFlags &= ~flag;
        EndSlotWrite(*slot);
        UnmapViewOfFile(header);
        return true;
    }

    bool TrySetDiagLifecycle(DWORD pid, std::uint32_t lifecycleBit, bool active) noexcept
    {
        if (pid == 0 || lifecycleBit == 0)
            return false;

        Header* header = MapHeaderView(FILE_MAP_WRITE);
        if (!header)
            return false;

        Slot* slot = FindSlotByPid(header, pid);
        if (!slot)
        {
            UnmapViewOfFile(header);
            return false;
        }

        BeginSlotWrite(*slot);
        if (active)
            slot->diagLifecycle |= lifecycleBit;
        else
            slot->diagLifecycle &= ~lifecycleBit;
        EndSlotWrite(*slot);
        UnmapViewOfFile(header);
        return true;
    }

    bool TrySetShutdownPhase(DWORD pid, std::uint32_t phase) noexcept
    {
        if (pid == 0)
            return false;

        Header* header = MapHeaderView(FILE_MAP_WRITE);
        if (!header)
            return false;

        Slot* slot = FindSlotByPid(header, pid);
        if (!slot)
        {
            UnmapViewOfFile(header);
            return false;
        }

        BeginSlotWrite(*slot);
        slot->shutdownPhase = phase;
        EndSlotWrite(*slot);
        UnmapViewOfFile(header);
        return true;
    }

    bool TryPublishSubclassStats(
        DWORD pid,
        std::uint32_t windowTotal,
        std::uint64_t kindMask) noexcept
    {
        if (pid == 0)
            return false;

        Header* header = MapHeaderView(FILE_MAP_WRITE);
        if (!header)
            return false;

        Slot* slot = FindSlotByPid(header, pid);
        if (!slot)
        {
            UnmapViewOfFile(header);
            return false;
        }

        BeginSlotWrite(*slot);
        slot->subclassWindowTotal = windowTotal;
        slot->subclassKindMask = kindMask;
        EndSlotWrite(*slot);
        UnmapViewOfFile(header);
        return true;
    }

    bool TryClearPidFromMapping(const wchar_t* mappingName, DWORD pid) noexcept
    {
        if (pid == 0 || !mappingName || !mappingName[0])
            return false;

        Header* header = MapHeaderViewNamed(mappingName, FILE_MAP_WRITE);
        if (!header)
            return false;

        Slot* slot = FindSlotByPid(header, pid);
        if (!slot)
        {
            UnmapViewOfFile(header);
            return false;
        }

        BeginSlotWrite(*slot);
        slot->kind = Kind::None;
        slot->path = 0;
        slot->state = State::Empty;
        slot->moduleBits = 0;
        slot->unloadResult = 0;
        slot->diagFlags = 0;
        slot->diagLifecycle = 0;
        slot->subclassWindowTotal = 0;
        slot->subclassKindMask = 0;
        slot->exeName[0] = L'\0';
        slot->shutdownPhase = 0;
        slot->reserved2 = 0;
        MemoryBarrier();
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&slot->pid), 0);
        EndSlotWrite(*slot);

        UnmapViewOfFile(header);
        return true;
    }

    bool TryClearPid(DWORD pid) noexcept
    {
        return TryClearPidFromMapping(kMappingName, pid);
    }

    bool TryReadSlots(Slot* outSlots, DWORD maxSlots, DWORD* outCount) noexcept
    {
        return TryReadSlotsFromMapping(kMappingName, outSlots, maxSlots, outCount);
    }

    bool TryReadSlotsFromMapping(
        const wchar_t* mappingName,
        Slot* outSlots,
        DWORD maxSlots,
        DWORD* outCount) noexcept
    {
        if (!outSlots || maxSlots == 0 || !outCount)
            return false;
        *outCount = 0;

        Header* header = MapHeaderViewNamed(mappingName, FILE_MAP_READ);
        if (!header)
            return false;

        const bool ok = ReadSlotsFromHeader(header, outSlots, maxSlots, outCount);
        UnmapViewOfFile(header);
        return ok;
    }

    bool IsMapAvailable() noexcept
    {
        return IsMappingAvailable(kMappingName);
    }

    bool IsMappingAvailable(const wchar_t* mappingName) noexcept
    {
        Header* header = MapHeaderViewNamed(mappingName, FILE_MAP_READ);
        if (!header)
            return false;
        const bool ok = header->magic == kMagic && header->version == kVersion;
        UnmapViewOfFile(header);
        return ok;
    }
}
