#pragma once

#include <Windows.h>
#include <cstdint>

#include <GlassPlatformTask.h>

// ============================================================================
// Shared::GlassStop — единственный механизм остановки платформы.
//
// Все инициаторы (GlassSettings, GlassShellExt, GlassSetup, GlassUnins,
// CrashGuard, HostShellWatchdog) вызывают Request(), а не SetEvent/StopTask напрямую;
// LogViewer по-прежнему только сигналит (ему не нужно ждать).
//
// Обоснование и устройство: GlassShared/docs/stop-unification.md.
//
// Инварианты (не менять без runtime-доказательства):
//  * порядок фаз и семантика path=1/2/3 в GlassCompat не меняются
//    (GlassCompat/docs/GlassCompat-unload.md);
//  * никакого FreeLibrary в чужих процессах: снять INCONTEXT-ссылку user32 может
//    только сам процесс/ОС (политика R6/R7, GlassCompat/docs/GlassCompat-unload.md);
//  * «дожим» = просьба + пинок + ожидание, а не принудительная выгрузка;
//  * остановка хоста любым способом (в т.ч. TerminateProcess планировщиком) —
//    штатный путь: инжектированные процессы уходят по abandoned HostAlive.
// ============================================================================

namespace Shared::GlassStop
{
    using GlassShared::GlassPlatformTask::PlatformArch;

    enum class Reason : int
    {
        User = 0,        // меню рабочего стола, кнопка в GlassSettings, LogViewer
        Installer = 1,   // GlassSetup
        Uninstaller = 2, // GlassUnins
        Crash = 3,       // CrashGuard / HostShellWatchdog
        External = 4,    // задача/хост завершены извне (End в планировщике)
        HostDead = 5,    // зафиксирована смерть хоста без StopEvent
    };

    struct Options
    {
        Reason reason = Reason::User;
        /// Ожидание штатного завершения (мутекс платформы исчез).
        DWORD gracefulMs = GlassShared::GlassPlatformTask::kDefaultPlatformShutdownWaitMs;
        /// Ожидание после HostShutdown: хост уже отпустил мьютекс, дожидаемся остатка.
        DWORD hostGoneMs = 5000;
        /// Разрешить IRegisteredTask::Stop как fallback, если хост не ушёл сам.
        bool allowTaskStop = false;
        /// Разбудить процессы с остатками (WM_NULL окнам и потокам). Никогда не грузит
        /// наши DLL и не вызывает FreeLibrary.
        bool wakeResiduals = false;
        // Принудительной выгрузки в чужих процессах координатор не умеет: политика R6/R7,
        // raw FreeLibrary извне запрещён. Опции для неё нет намеренно.
    };

    struct Result
    {
        bool hostGone = false;          // мьютекс платформы исчез
        bool hostKilledByTask = false;  // пришлось останавливать задачу
        unsigned residualProcesses = 0; // процессы с нашими модулями после ожидания
        DWORD elapsedMs = 0;
        /// "" | "dead_probe" | "signal" | "host" | "busy" | "already_stopped"
        const wchar_t* failedPhase = L"";
        /// До 16 pid остатков — для лога и страницы «закрыть процессы».
        unsigned residualPidCount = 0;
        DWORD residualPids[16]{};
    };

    /// Полная остановка: сигнал → ожидание → (опционально) Stop задачи → снимок остатков.
    [[nodiscard]] Result Request(PlatformArch arch, const Options& options) noexcept;

    /// То же для текущей архитектуры с Reason::User.
    [[nodiscard]] Result Request() noexcept;

    /// Снимок процессов с нашими модулями (только чтение, без вмешательства).
    /// Возвращает число найденных процессов, до maxPids pid'ов пишет в outPids.
    [[nodiscard]] unsigned SnapshotResiduals(
        PlatformArch arch, DWORD* outPids, unsigned maxPids) noexcept;

    /// W2 (§2в документа): разбудить процессы с остатками — PostMessage(WM_NULL) всем их
    /// окнам и PostThreadMessage(WM_NULL) всем их потокам. Нужно, когда хост убит и
    /// «пинкать» больше некому: у «тихого» процесса иначе остаётся per-thread
    /// hook-ссылка до следующего обращения к user32.
    /// Ничего не загружает и не выгружает. Возвращает число процессов, которым помогли.
    // Wake a PID already obtained from a verified holder scan. Architecture-
    // neutral queue messages only; includes the caller when it holds modules.
    [[nodiscard]] unsigned WakeProcessQueues(DWORD pid) noexcept;

    [[nodiscard]] unsigned WakeResidualProcesses(
        PlatformArch arch, DWORD* outPids, unsigned maxPids) noexcept;
}
