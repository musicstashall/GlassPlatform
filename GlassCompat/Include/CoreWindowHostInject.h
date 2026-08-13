#pragma once

#include <Windows.h>

namespace GlassCompat::CoreWindowHostInject
{
    /// Выдать ALL_APPLICATION_PACKAGES RX на peer-DLL (нужно для AppContainer SEH/SMEH).
    void GrantPeerDllsAppPackagesAccess() noexcept;

    /// Per-thread INCONTEXT хуки + optional LoadLibrary в SEH/ShellHost/SMEH.
    /// Вызывать из хост-цикла (не из injected).
    void Refresh() noexcept;

    /// Снять все per-target хуки при stop хоста (до SignalHostShutdown).
    void Shutdown() noexcept;

    /// После HostShutdown: если SEH/SMEH ещё держат GlassCompat — remote FreeLibrary.
    void ForceUnloadResiduals() noexcept;

    /// Host OOC SHOW CoreWindow → post GUI attach (не CreateRemoteThread).
    void NotifyRemoteCoreWindowShow(DWORD pid, HWND hwnd) noexcept;
}
