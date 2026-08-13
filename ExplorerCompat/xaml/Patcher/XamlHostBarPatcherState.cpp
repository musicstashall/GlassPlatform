#include "ExplorerPch.h"
#include "xaml/Patcher/XamlHostBarPatcherState.h"
#include <RootPolicyQuery.h>

// Определения глобального состояния XamlHostBarPatcher (единственная TU для этих символов).

std::atomic<ULONGLONG> g_explorerStartupGateUntilTick{ 0 };
std::atomic<bool> g_startupGateLogPrinted{ false };
std::mutex g_xamlMutex;
std::atomic<bool> g_disableHostBarPatching{ false };
std::atomic<DWORD> g_vtcCallbackTid{ 0 };
std::atomic<bool> g_pendingApplyHostBarChrome{ false };
std::atomic<bool> g_pendingRestoreHostBarChrome{ false };
std::atomic<bool> g_pendingRepatchTabElements{ false };
std::atomic<bool> g_replayRestoreAfterOff{ false };
std::atomic<bool> g_inFullReplayPass{ false };
std::atomic<bool> g_loggedSetPropertyFailInReplay{ false };
thread_local bool g_hostBarRestoreOnIslandUiQueue = false;

std::unordered_map<UINT64, UINT64> g_parentOf;
std::unordered_map<UINT64, std::wstring> g_nameOf;
std::unordered_map<UINT64, std::wstring> g_typeOf;
std::unordered_map<UINT64, ABI::Windows::UI::Xaml::Media::IBrush*> g_originalFills;
std::unordered_map<UINT64, ABI::Windows::UI::Xaml::Media::IBrush*> g_originalBorderBackgrounds;

std::unordered_map<HostBarPropPatchKey, UINT64, HostBarPropPatchKeyHash> g_hostBarPropertyPatches;
std::unordered_map<HostBarPropPatchKey, IInspectable*, HostBarPropPatchKeyHash> g_hostBarOriginalInspectables;
std::unordered_set<UINT64> g_tabContainerHandles;
std::unordered_map<UINT64, std::vector<TabRepatchEntry>> g_tabRepatchTargets;
int g_repatchCyclesRemaining = 0;
std::vector<PendingLazyHostBar> g_pendingLazyHostBars;

std::unordered_map<UINT64, ExplorerDiagBgPatch> g_explorerDiagBackgrounds;

// Shell-root (CabinetWClass/ExploreWClass), получившие XAML/GDI chrome apply в этой сессии.
std::unordered_set<HWND> g_patchedExplorerShellHwnds;

namespace XamlHostBarPatcher
{
    bool IsExplorerXamlPatchingAllowed() noexcept
    {
        if (!Shared::RootPolicyQuery::IsRootGlassRenderingEnabled())
        {
            return false;
        }
        if (g_disableHostBarPatching.load(std::memory_order_acquire))
        {
            return false;
        }
        return true;
    }
}
