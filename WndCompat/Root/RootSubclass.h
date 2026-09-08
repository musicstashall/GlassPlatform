#pragma once
#include "WndPch.h"
#include <GlassMessages.h>

// Диагностика: в WinEventHandler case WindowRole::Root дорожка проводника зовёт
// DispatchRootAttachToOwnerThread вместо DispatchExplorerRootAttachToOwnerThread.
// 1 — и Root_Attach пропускает shell-root (не игнорирует), см. RootSubclass.cpp.
#ifndef GLASS_DIAG_EXPLORER_USE_ROOT_SUBCLASS
#define GLASS_DIAG_EXPLORER_USE_ROOT_SUBCLASS 0
#endif

namespace WndCompat
{

    // Цепочка без смешивания:
    //   Classifier → FileExplorerShellRoot → DispatchExplorerRootAttachToOwnerThread → ExplorerRoot::Attach
    //   Classifier → иной Root → DispatchRootAttachToOwnerThread → Root_Attach → RootSubclassProc
    // Root_Attach не вызывает ExplorerRoot::Attach; для shell-проводника — только ExplorerRoot::Attach
    // (кроме GLASS_DIAG_EXPLORER_USE_ROOT_SUBCLASS == 1).
    void Root_Attach(HWND hRoot);

    // Панель задач (Shell_*TrayWnd): маршалинг на поток окна → TaskbarHost_Attach (без RootSubclassProc).
    void Tray_Attach(HWND hRoot) noexcept;

    // Снять сабкласс с конкретного root-окна
    void Root_Detach(HWND hRoot);

    // Снять сабклассы со всех root-окон (Shutdown)
    void Root_DetachAll();

    // Принудительное снятие subclass с окна (EnumWindows fallback перед FreeLibrary)
    void Root_ForceDetachSubclass(HWND hWnd) noexcept;

    bool WINAPI Root_IsAttached(HWND hRoot) noexcept;

    void Root_CopyTrackedRoots(std::vector<HWND>& out) noexcept;

    /// Обход g_rootWindows при смене RootBackdropMode (регистрация в GlassCore).
    void WINAPI ApplyBackdropPolicyToTrackedRoots() noexcept;

    /// Включение backdrop: зеркало post-attach визуалов Root_Attach (без повторного OnRootHooksAttach).
    void ApplyRootGlassVisualsOnBackdropEnabled(HWND hRoot) noexcept;

    /// Мягкий NONE / re-enable на потоке владельца root (в т.ч. Explorer shell-root).
    void ApplyRootBackdropReapplyOnOwnerThread(HWND hRoot) noexcept;

    /// Marshal WM_GLASS_REAPPLY_ROOT_BACKDROP на поток владельца (live OFF sweep).
    void MarshalApplyRootBackdropReapplyForHwnd(HWND hRoot) noexcept;

    /// Live-reload backdrop: PostMessage только по реестру (без тяжёлого ReapplyOpenRootBackdrops).
    enum RootLiveBackdropScope : DWORD
    {
        RootLiveBackdropScopeAllTracked     = 0,
        RootLiveBackdropScopeAppliedOnly    = 1,
    };

    void PostLiveRootBackdropReapply(RootLiveBackdropScope scope) noexcept;

    /// Live Root composition: WM_DWMCOLORIZATIONCHANGED (wParam=0, lParam=scope) по applied roots.
    void PostLiveRootColorizationToAppliedRoots(LPARAM liveRootScope = kGlassLiveRootScopeInfer) noexcept;

    /// После Initial scan: одноразовая полная перерисовка tracked roots при первом backdrop-sweep.
    void NotifyInitialRootScanCompleted() noexcept;
}
