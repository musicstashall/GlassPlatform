#pragma once
#include <windows.h>

namespace WndCompat
{
    enum class Win32ChromeRestoreScope
    {
        LiveNoBackdrop,
        ShutdownDetach,
    };

    /// Live OFF: снять glass-визуалы root на потоке владельца (subclass остаётся).
    void RestoreRootWin32Chrome(HWND hRoot, Win32ChromeRestoreScope scope) noexcept;

    /// Обход tracked roots (marshal на owner thread).
    void RestoreTrackedRootsWin32Chrome(Win32ChromeRestoreScope scope) noexcept;

    /// Диспетчер контролов: Apply* при LiveNoBackdrop, *_DetachAll при ShutdownDetach.
    void RestoreTrackedControlsWin32Chrome(Win32ChromeRestoreScope scope) noexcept;

    /// Classic menu bar NC при live OFF.
    void RestoreMenuBarWin32Chrome(Win32ChromeRestoreScope scope) noexcept;

    /// Top-level: roots + menu + controls (или полный shutdown detach cascade).
    void RestoreWin32Chrome(Win32ChromeRestoreScope scope) noexcept;

    /// Уведомить ExplorerCompat/GlassCompat о смене Root backdrop (live ON/OFF).
    void NotifyExplorerRootBackdropPolicyChanged(bool allowed) noexcept;
}
