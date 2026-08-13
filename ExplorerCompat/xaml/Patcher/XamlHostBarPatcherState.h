#pragma once

#include "ExplorerPch.h"

#include <windows.ui.xaml.media.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct IInspectable;

// Общее состояние патчера XAML (один набор на процесс explorer). Определения — в .cpp.

inline constexpr ULONGLONG kExplorerStartupGateMs = 450;
inline constexpr int kRepatchMaxCycles = 12;
inline constexpr wchar_t kExplorerHostBarBrushInitTransparent[] = L"Transparent";

// --- Host bar patches (WinUI SetProperty): ключ в map ---
struct HostBarPropPatchKey
{
    UINT64 element{};
    unsigned propIndex{};
    bool operator==(const HostBarPropPatchKey& o) const noexcept
    {
        return element == o.element && propIndex == o.propIndex;
    }
};

struct HostBarPropPatchKeyHash
{
    [[nodiscard]] size_t operator()(const HostBarPropPatchKey& k) const noexcept
    {
        return std::hash<UINT64>{}(k.element) ^ (std::hash<unsigned>{}(k.propIndex) << 1);
    }
};

struct TabRepatchEntry
{
    unsigned propIndex{};
    std::wstring brushTypeName;
};

struct PendingLazyHostBar
{
    UINT64 handle{};
    std::wstring type;
    std::wstring name;
    UINT32 numChildren{};
};

enum class BgPatchTarget : std::uint8_t
{
    Panel,
    Control,
    Border
};

struct ExplorerDiagBgPatch
{
    ABI::Windows::UI::Xaml::Media::IBrush* original{};
    BgPatchTarget target{};
};

extern std::atomic<ULONGLONG> g_explorerStartupGateUntilTick;
extern std::atomic<bool> g_startupGateLogPrinted;
extern std::mutex g_xamlMutex;
extern std::atomic<bool> g_disableHostBarPatching;
extern std::atomic<DWORD> g_vtcCallbackTid;
extern std::atomic<bool> g_pendingApplyHostBarChrome;
extern std::atomic<bool> g_pendingRestoreHostBarChrome;
extern std::atomic<bool> g_pendingRepatchTabElements;
extern std::atomic<bool> g_replayRestoreAfterOff;
extern std::atomic<bool> g_inFullReplayPass;
extern std::atomic<bool> g_loggedSetPropertyFailInReplay;
// DispatcherQueue конкретного XAML-острова уже гарантирует правильный UI-поток;
// не применять к нему глобальный last-VTC gate, который может быть перезаписан popup-островом.
extern thread_local bool g_hostBarRestoreOnIslandUiQueue;

extern std::unordered_map<UINT64, UINT64> g_parentOf;
extern std::unordered_map<UINT64, std::wstring> g_nameOf;
extern std::unordered_map<UINT64, std::wstring> g_typeOf;
extern std::unordered_map<UINT64, ABI::Windows::UI::Xaml::Media::IBrush*> g_originalFills;
extern std::unordered_map<UINT64, ABI::Windows::UI::Xaml::Media::IBrush*> g_originalBorderBackgrounds;

extern std::unordered_map<HostBarPropPatchKey, UINT64, HostBarPropPatchKeyHash> g_hostBarPropertyPatches;
extern std::unordered_map<HostBarPropPatchKey, IInspectable*, HostBarPropPatchKeyHash> g_hostBarOriginalInspectables;
extern std::unordered_set<UINT64> g_tabContainerHandles;
extern std::unordered_map<UINT64, std::vector<TabRepatchEntry>> g_tabRepatchTargets;
extern int g_repatchCyclesRemaining;
extern std::vector<PendingLazyHostBar> g_pendingLazyHostBars;

extern std::unordered_map<UINT64, ExplorerDiagBgPatch> g_explorerDiagBackgrounds;

extern std::unordered_set<HWND> g_patchedExplorerShellHwnds;
