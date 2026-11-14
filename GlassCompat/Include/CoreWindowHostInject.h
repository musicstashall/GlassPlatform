#pragma once

#include <Windows.h>

namespace GlassCompat::CoreWindowHostInject
{
    /// Выдать ALL_APPLICATION_PACKAGES RX на peer-DLL (нужно для AppContainer SEH/SMEH).
    void GrantPeerDllsAppPackagesAccess() noexcept;

    /// Per-thread INCONTEXT хуки + optional LoadLibrary в SEH/ShellHost/SMEH.
    /// Вызывать из хост-цикла (не из injected).
    void Refresh() noexcept;

    /// Снять все per-target хуки до SignalHostShutdown. При остановке задачи
    /// не менять настройки чужих процессов и не посылать им дополнительные wake.
    void Shutdown(bool wakeProcesses) noexcept;

    /// Освободить ресурсы и собственный PLM-debug при обычной остановке задачи,
    /// не выполняя удалённую выгрузку модулей.
    void FinishTaskStop() noexcept;

    /// Для других сценариев остановки: дополнительная обработка остатков.
    void ForceUnloadResiduals() noexcept;

    /// Host OOC SHOW CoreWindow → post GUI attach (не CreateRemoteThread).
    void NotifyRemoteCoreWindowShow(DWORD pid, HWND hwnd) noexcept;
}
