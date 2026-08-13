#pragma once

#include <InjectionMode.h>
#include <vector>
#include <windows.h>

namespace WndCompat::BackdropPolicyRescan
{
    struct WindowScanStats
    {
        unsigned topDirect = 0;
        unsigned topPosted = 0;
        unsigned childDirect = 0;
        unsigned childPosted = 0;
    };

    struct WindowScanContext
    {
        DWORD pid = 0;
        DWORD scanTid = 0;
        Shared::InjectionMode mode = Shared::InjectionMode::Full;
        bool verbose = false;
        /// Initial scan: только top-level (Shell_TrayWnd и др.). Полный subtree + SendMessageTimeout
        /// на каждом child блокировал DoInitialize → g_initDone=false → нет live WinEvent → нет TaskbarCompat.
        bool topLevelOnly = false;
        const wchar_t* phase = L"scan";
        WindowScanStats stats{};
    };

    /// Initial scan + catch-up: рекурсивный synthetic EVENT_OBJECT_CREATE под hSubtreeRoot.
    void ScanSubtreeRecursive(HWND hSubtreeRoot, WindowScanContext& ctx) noexcept;

    /// EnumWindows по процессу + ScanSubtreeRecursive (замена flat EnumChildWindows).
    void RunProcessWindowScan(WindowScanContext& ctx) noexcept;

    /// Маршалинг synthetic CREATE на поток владельца (Root → WM_GLASS_ATTACH_ROOT, иначе WM_GLASS_SYNTHETIC_CREATE).
    void MarshalSyntheticCreateAttach(HWND hWnd) noexcept;

    /// Рекурсивный обход GW_CHILD/GW_HWNDNEXT с MarshalSyntheticCreateAttach на каждом узле.
    void DispatchSyntheticCreateAttachRecursive(HWND hRoot) noexcept;

    /// Live ON: rescan roots + controls + WinUi hosts.
    void RescanAllMissingGlassHostsForBackdropEnable() noexcept;

    /// После Root_Attach / ExplorerRoot::Attach — catch-up дочерних HWND на потоке владельца.
    void ScheduleSubtreeRescanAfterRootAttach(HWND hRoot) noexcept;

    /// Live ON: catch-up controls под tracked roots.
    void RescanMissingControlsUnderTrackedRoots(const std::vector<HWND>& roots) noexcept;
}
