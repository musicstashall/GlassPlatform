#pragma once

#include <Windows.h>

#include <string>

namespace GlassShared::GlassPlatformTask
{
    enum class PlatformArch : int
    {
        X86 = 0,
        X64 = 1,
    };

    [[nodiscard]] constexpr PlatformArch NativePlatformArch() noexcept
    {
#ifdef _WIN64
        return PlatformArch::X64;
#else
        return PlatformArch::X86;
#endif
    }

#ifdef _WIN64
    constexpr wchar_t kInstanceMutexName[] = L"GlassPlatform.Instance.Mutex.x64";
    constexpr wchar_t kStopEventName[] = L"GlassPlatform.StopEvent.x64";
    constexpr wchar_t kHostShutdownEventName[] = L"GlassPlatform.HostShutdown.x64";
    constexpr wchar_t kHostAliveMutexName[] = L"GlassPlatform.HostAlive.x64";
    constexpr wchar_t kHostPidMappingName[] = L"Local\\GlassHostPid.x64";
#else
    constexpr wchar_t kInstanceMutexName[] = L"GlassPlatform.Instance.Mutex.x86";
    constexpr wchar_t kStopEventName[] = L"GlassPlatform.StopEvent.x86";
    constexpr wchar_t kHostShutdownEventName[] = L"GlassPlatform.HostShutdown.x86";
    constexpr wchar_t kHostAliveMutexName[] = L"GlassPlatform.HostAlive.x86";
    constexpr wchar_t kHostPidMappingName[] = L"Local\\GlassHostPid.x86";
#endif

    [[nodiscard]] const wchar_t* InstanceMutexName(PlatformArch arch) noexcept;
    [[nodiscard]] const wchar_t* StopEventName(PlatformArch arch) noexcept;
    [[nodiscard]] const wchar_t* HostShutdownEventName(PlatformArch arch) noexcept;
    /// Dead-man switch: хост владеет этим мьютексом всё время работы.
    /// Успешный захват в инжектированном процессе (WAIT_OBJECT_0 или WAIT_ABANDONED)
    /// означает, что хост его больше не держит, то есть мёртв — в отличие от
    /// OpenProcess/PID-поллинга, это не даёт ложных срабатываний в AppContainer.
    [[nodiscard]] const wchar_t* HostAliveMutexName(PlatformArch arch) noexcept;
    [[nodiscard]] const wchar_t* HostPidMappingName(PlatformArch arch) noexcept;

    // Страховочный таймаут WaitForPlatformStopped (на практике 1–5 с).
    constexpr DWORD kDefaultPlatformShutdownWaitMs = 30000;

    [[nodiscard]] const wchar_t* GlassAutorunTaskName() noexcept;
    [[nodiscard]] const wchar_t* GlassAutorunTaskName(PlatformArch arch) noexcept;

    enum class AutorunUiState : int
    {
        NoTask = 0,
        Disabled,
        Ready,
        Running
    };

    [[nodiscard]] bool IsGlassPlatformRunning() noexcept;
    [[nodiscard]] bool IsGlassPlatformRunning(PlatformArch arch) noexcept;

    [[nodiscard]] bool GlassAutorunTaskExists() noexcept;
    [[nodiscard]] bool GlassAutorunTaskExists(PlatformArch arch) noexcept;

    [[nodiscard]] AutorunUiState QueryGlassAutorunUiState() noexcept;
    [[nodiscard]] AutorunUiState QueryGlassAutorunUiState(PlatformArch arch) noexcept;

    void StartPlatform() noexcept;
    void StartPlatform(PlatformArch arch) noexcept;

    [[nodiscard]] bool SignalPlatformStop() noexcept;
    [[nodiscard]] bool SignalPlatformStop(PlatformArch arch) noexcept;

    void StopPlatform() noexcept;
    void StopPlatform(PlatformArch arch) noexcept;

    /// Если mutex уже нет, а Task Scheduler instance ещё Running — IRegisteredTask::Stop.
    [[nodiscard]] bool StopOrphanAutorunTaskInstance(PlatformArch arch) noexcept;

    [[nodiscard]] bool WaitForPlatformStopped(DWORD timeoutMs = kDefaultPlatformShutdownWaitMs) noexcept;
    [[nodiscard]] bool WaitForPlatformStopped(PlatformArch arch, DWORD timeoutMs = kDefaultPlatformShutdownWaitMs) noexcept;

    /// Named Event Local\GlassPlatform.LifecycleBusy.* — start/stop в полёте (Settings + ShellExt).
    [[nodiscard]] bool IsLifecycleBusy(PlatformArch arch) noexcept;
    [[nodiscard]] bool TryBeginLifecycleBusy(PlatformArch arch) noexcept;
    void EndLifecycleBusy(PlatformArch arch) noexcept;

    /// Post-stop residual GlassCompat в shell/CW/Widget той же битности (не при Running).
    [[nodiscard]] bool HasShellInjectResiduals(PlatformArch arch) noexcept;

    /// Единый lifecycle для меню / списка / Home.
    enum class InjectLifecycleUi : int
    {
        Idle = 0,
        Running = 1,
        Stopping = 2,
        Residual = 3,
    };

    /// Running=mutex; Stopping=LifecycleBusy|Stop+Running; Residual=sticky/live Module32;
    /// Idle=иначе. Title «Включить» только Idle; Enabled Idle|Running.
    [[nodiscard]] InjectLifecycleUi QueryInjectLifecycleUi(PlatformArch arch) noexcept;

    /// Скан known hosts → sticky named mapping (ShellExt / GS).
    [[nodiscard]] bool RefreshInjectResidualSticky(PlatformArch arch) noexcept;

    [[nodiscard]] bool SignalReloadRender(DWORD flags) noexcept;

    void WriteReloadRootLiveScope(LPARAM liveRootScope) noexcept;
    [[nodiscard]] LPARAM ReadReloadRootLiveScope() noexcept;

    [[nodiscard]] bool SetGlassAutorunTaskSchedulerEnabled(bool enable) noexcept;
    [[nodiscard]] bool SetGlassAutorunTaskSchedulerEnabled(PlatformArch arch, bool enable) noexcept;

    /// Задача автозапуска не зависит от питания и простоя?
    /// Дефолты schtasks /Create — DisallowStartIfOnBatteries и StopIfGoingOnBatteries
    /// в true: при переходе на батарею Планировщик завершает хост, StopEvent никто
    /// не выставляет, и инжектированные модули остаются в процессах.
    [[nodiscard]] bool IsAutorunTaskPowerIndependent(PlatformArch arch) noexcept;

    /// Перерегистрирует задачу с DisallowStartIfOnBatteries/StopIfGoingOnBatteries/
    /// AllowHardTerminate/StopOnIdleEnd = false и ExecutionTimeLimit = PT0S.
    /// Требует прав администратора (задача создана с RunLevel HighestAvailable).
    [[nodiscard]] bool ApplyAutorunTaskPowerIndependence(PlatformArch arch) noexcept;

    /// Каталог x86/x64 платформы из /TR задачи автозапуска (GlassCompat.dll). Пусто — задачи нет или TR не распознан.
    [[nodiscard]] std::wstring TryGetAutorunGlassCompatInstallRoot(PlatformArch arch) noexcept;
}
