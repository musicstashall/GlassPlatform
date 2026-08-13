#pragma once

#include <Windows.h>
#include <cstdint>
#include <cstdio>

namespace Shared::InjectProcessMap
{
#if defined(_WIN64)
    inline constexpr wchar_t kMappingName[] = L"Local\\GlassPlatform.InjectMap.x64";
#else
    inline constexpr wchar_t kMappingName[] = L"Local\\GlassPlatform.InjectMap.x86";
#endif
    /// Legacy имя (до dual-arch); GS читает как fallback x64.
    inline constexpr wchar_t kMappingNameLegacy[] = L"Local\\GlassPlatform.InjectMap";
    inline constexpr wchar_t kMappingNameX64[] = L"Local\\GlassPlatform.InjectMap.x64";
    inline constexpr wchar_t kMappingNameX86[] = L"Local\\GlassPlatform.InjectMap.x86";

    inline constexpr DWORD kMagic = 0x4D504947u; // 'GIPM' LE
    inline constexpr DWORD kVersion = 2u;
    inline constexpr DWORD kSlotCount = 512u;
    inline constexpr DWORD kExeNameChars = 64u;

    enum class Kind : std::uint8_t
    {
        None = 0,
        ActiveGui = 1,
        RejectedBlockList = 2,
        RejectedForbidden = 3,
    };

    enum class State : std::uint8_t
    {
        Empty = 0,
        Alive = 1,
        Unloading = 2,
        Unloaded = 3,
    };

    enum ModuleBits : std::uint32_t
    {
        ModuleCompat = 1u << 0,
        ModuleCore = 1u << 1,
        ModuleWnd = 1u << 2,
        ModuleExplorer = 1u << 3,
        ModuleTaskbar = 1u << 4,
        ModuleUICompat = 1u << 5,
    };

    /// Флаги подсистем (пишут Startup/Shutdown / Install/Uninstall владельцы).
    enum DiagFlags : std::uint32_t
    {
        DiagHooks = 1u << 0,
        DiagPaintDispatcher = 1u << 1,
        DiagModuleHandler = 1u << 2,
        DiagExplorer = 1u << 3,
        DiagTaskbar = 1u << 4,
        DiagWinEventProc = 1u << 5, // WndCompat WinEventHook tracker
        DiagCrashGuard = 1u << 6,   // GlassCompat CrashGuard
        DiagUICompat = 1u << 7,     // UICompat CoreWindow IXDE
    };

#pragma pack(push, 4)
    struct Slot
    {
        /// Seqlock: нечётное = запись; чётное = стабильно.
        volatile LONG seq;
        DWORD pid;
        Kind kind;
        std::uint8_t path;
        State state;
        std::uint8_t reserved;
        std::uint32_t moduleBits;
        std::uint32_t unloadResult;
        std::uint32_t diagFlags;
        std::uint32_t subclassWindowTotal;
        std::uint64_t subclassKindMask;
        wchar_t exeName[kExeNameChars];
    };

    struct Header
    {
        DWORD magic;
        DWORD version;
        DWORD slotCount;
        DWORD reserved;
        Slot slots[kSlotCount];
    };
#pragma pack(pop)

    static_assert(sizeof(Slot) == 164, "InjectProcessMap::Slot stride must match GS native parse");

    [[nodiscard]] bool CreateHostResources(SECURITY_ATTRIBUTES* sa) noexcept;
    void CloseHostResources() noexcept;

    [[nodiscard]] bool TryUpsert(
        DWORD pid,
        Kind kind,
        std::uint8_t path,
        State state,
        std::uint32_t moduleBits,
        const wchar_t* exeName) noexcept;

    [[nodiscard]] bool TrySetState(DWORD pid, State state) noexcept;
    [[nodiscard]] bool TrySetModuleBits(DWORD pid, std::uint32_t moduleBits) noexcept;
    /// Прочитать moduleBits Alive-слота (0 если нет). Для host Toolhelp merge.
    [[nodiscard]] bool TryGetModuleBits(DWORD pid, std::uint32_t& outBits) noexcept;
    [[nodiscard]] bool TrySetUnloadResult(DWORD pid, std::uint32_t unloadResult) noexcept;
    [[nodiscard]] bool TrySetDiagFlag(DWORD pid, std::uint32_t flag, bool active) noexcept;
    [[nodiscard]] bool TryPublishSubclassStats(
        DWORD pid,
        std::uint32_t windowTotal,
        std::uint64_t kindMask) noexcept;
    [[nodiscard]] bool TryClearPid(DWORD pid) noexcept;
    /// Очистка слота в конкретной карте (x64/x86/legacy) — для GS dual-arch.
    [[nodiscard]] bool TryClearPidFromMapping(const wchar_t* mappingName, DWORD pid) noexcept;

    [[nodiscard]] bool TryReadSlots(Slot* outSlots, DWORD maxSlots, DWORD* outCount) noexcept;
    [[nodiscard]] bool TryReadSlotsFromMapping(
        const wchar_t* mappingName,
        Slot* outSlots,
        DWORD maxSlots,
        DWORD* outCount) noexcept;

    [[nodiscard]] bool IsMapAvailable() noexcept;
    [[nodiscard]] bool IsMappingAvailable(const wchar_t* mappingName) noexcept;

    inline void FormatUnloadRequestName(wchar_t* buf, size_t cch, DWORD pid) noexcept
    {
        if (!buf || cch == 0)
            return;
        (void)swprintf_s(buf, cch, L"Local\\GlassCompat.UnloadRequest.%lu", static_cast<unsigned long>(pid));
    }

    [[nodiscard]] inline HANDLE CreateUnloadRequestEvent(SECURITY_ATTRIBUTES* sa) noexcept
    {
        wchar_t name[96]{};
        FormatUnloadRequestName(name, _countof(name), GetCurrentProcessId());
        return CreateEventW(sa, FALSE, FALSE, name);
    }

    [[nodiscard]] inline bool SignalUnloadRequest(DWORD pid) noexcept
    {
        if (pid == 0)
            return false;
        wchar_t name[96]{};
        FormatUnloadRequestName(name, _countof(name), pid);
        HANDLE h = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
        if (!h)
            return false;
        const BOOL ok = SetEvent(h);
        CloseHandle(h);
        return ok != FALSE;
    }
}
