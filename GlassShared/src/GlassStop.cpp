#include "SharedPch.h"

#include <GlassStop.h>
#include <GlassUlTrace.h>
#include <InjectModuleScan.h>

#include <TlHelp32.h>

namespace Shared::GlassStop
{
    namespace
    {
        namespace GPT = GlassShared::GlassPlatformTask;
        using GPT::PlatformArch;

        constexpr unsigned kMaxResiduals = 16;

        [[nodiscard]] bool IsOwnArch(PlatformArch arch) noexcept
        {
#if defined(_WIN64)
            return arch == PlatformArch::X64;
#else
            return arch == PlatformArch::X86;
#endif
        }

        [[nodiscard]] const char* ReasonTag(Reason reason) noexcept
        {
            switch (reason)
            {
            case Reason::User: return "user";
            case Reason::Installer: return "installer";
            case Reason::Uninstaller: return "uninstaller";
            case Reason::Crash: return "crash";
            case Reason::External: return "external";
            case Reason::HostDead: return "host_dead";
            }
            return "unknown";
        }

        // Снимок: процессы, где реально лежит хоть один наш модуль.
        // Только чтение — установщик и наблюдатели используют этот же путь.
        [[nodiscard]] unsigned CollectResiduals(
            PlatformArch arch,
            DWORD* outPids,
            unsigned maxPids) noexcept
        {
            if (!IsOwnArch(arch))
                return 0;

            static Shared::InjectModuleScan::Result s_rows[Shared::InjectModuleScan::kMaxResults];
            DWORD count = 0;
            if (!Shared::InjectModuleScan::Scan(nullptr, 0, s_rows,
                    Shared::InjectModuleScan::kMaxResults, &count))
                return 0;

            const DWORD self = GetCurrentProcessId();
            unsigned found = 0;
            for (DWORD i = 0; i < count; ++i)
            {
                if (s_rows[i].pid == 0 || s_rows[i].pid == self)
                    continue;
                if (s_rows[i].moduleBits == 0)
                    continue;
                if (outPids && found < maxPids)
                    outPids[found] = s_rows[i].pid;
                ++found;
            }
            return found;
        }

        // WM_NULL во все окна и все потоки процесса. Именно это делает хост живым
        // (PostWmNullToTrackedHwnds / PostWmNullAllThreads), и только это разрешено
        // делать снаружи после R6/R7.
        void NudgeProcess(DWORD pid, unsigned& windows, unsigned& threads) noexcept
        {
            struct Ctx
            {
                DWORD pid;
                unsigned posted;
            } ctx{ pid, 0 };

            (void)EnumWindows(
                [](HWND hwnd, LPARAM lp) -> BOOL
                {
                    auto* c = reinterpret_cast<Ctx*>(lp);
                    DWORD wpid = 0;
                    (void)GetWindowThreadProcessId(hwnd, &wpid);
                    if (wpid == c->pid && PostMessageW(hwnd, WM_NULL, 0, 0))
                        ++c->posted;
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&ctx));
            windows = ctx.posted;

            threads = 0;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE)
                return;
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            if (Thread32First(snap, &te))
            {
                do
                {
                    if (te.th32OwnerProcessID != pid)
                        continue;
                    if (PostThreadMessageW(te.th32ThreadID, WM_NULL, 0, 0))
                        ++threads;
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
        }
    }

    const wchar_t* ReasonName(Reason reason) noexcept
    {
        switch (reason)
        {
        case Reason::User: return L"user";
        case Reason::Installer: return L"installer";
        case Reason::Uninstaller: return L"uninstaller";
        case Reason::Crash: return L"crash";
        case Reason::External: return L"external";
        case Reason::HostDead: return L"host_dead";
        }
        return L"unknown";
    }

    unsigned SnapshotResiduals(
        PlatformArch arch, DWORD* outPids, unsigned maxPids) noexcept
    {
        return CollectResiduals(arch, outPids, maxPids);
    }

    unsigned WakeResidualProcesses(
        PlatformArch arch, DWORD* outPids, unsigned maxPids) noexcept
    {
        DWORD pids[32]{};
        const unsigned total = CollectResiduals(arch, pids, _countof(pids));
        if (total == 0)
            return 0;

        const unsigned batch = total < _countof(pids) ? total : _countof(pids);
        unsigned nudged = 0;
        for (unsigned i = 0; i < batch; ++i)
        {
            unsigned windows = 0;
            unsigned threads = 0;
            NudgeProcess(pids[i], windows, threads);
            if (windows > 0 || threads > 0)
            {
                if (outPids && nudged < maxPids)
                    outPids[nudged] = pids[i];
                ++nudged;
            }
            GlassUlTrace::Logf(
                "H102",
                "GlassStop.cpp:WakeResidualProcesses",
                "stop_wake_pid",
                "\"target_pid\":%lu,\"windows\":%u,\"threads\":%u",
                static_cast<unsigned long>(pids[i]),
                windows,
                threads);
        }

        GlassUlTrace::Logf(
            "H102",
            "GlassStop.cpp:WakeResidualProcesses",
            "stop_wake_end",
            "\"scanned\":%u,\"nudged\":%u",
            total,
            nudged);
        return nudged;
    }

    Result Request(PlatformArch arch, const Options& options) noexcept
    {
        const ULONGLONG t0 = GetTickCount64();
        Result result{};
        const char* reason = ReasonTag(options.reason);

        GlassUlTrace::Logf(
            "H102",
            "GlassStop.cpp:Request",
            "stop_request",
            "\"reason\":\"%s\",\"arch\":%d,\"graceful_ms\":%lu,\"host_gone_ms\":%lu,"
            "\"allow_task_stop\":%d,\"wake_residuals\":%d,\"force_unload\":%d",
            reason,
            static_cast<int>(arch),
            static_cast<unsigned long>(options.gracefulMs),
            static_cast<unsigned long>(options.hostGoneMs),
            options.allowTaskStop ? 1 : 0,
            options.wakeResiduals ? 1 : 0,
            options.allowForceUnload ? 1 : 0);

        // Политика R6/R7: принудительная выгрузка в чужих процессах запрещена.
        if (options.allowForceUnload)
        {
            GlassUlTrace::Logf(
                "H102", "GlassStop.cpp:Request", "stop_phase",
                "\"reason\":\"%s\",\"phase\":\"policy\",\"note\":\"force_unload_ignored\"",
                reason);
        }

        // (5.1) Reason::HostDead — хост уже умер (abandoned HostAlive / TerminateProcess).
        // По этому пути НЕ сигналим и НЕ трогаем задачу: будить и добивать нечего, у
        // инжектированных процессов свои watcher'ы (path=2/3). Единственная полезная
        // работа — снимок остатков и, если просили, WM_NULL-«пинок» (5.5).
        if (options.reason == Reason::HostDead)
        {
            result.residualPidCount = CollectResiduals(
                arch, result.residualPids, kMaxResiduals);
            result.residualProcesses = result.residualPidCount;
            if (options.wakeResiduals && result.residualPidCount > 0)
            {
                (void)WakeResidualProcesses(arch, nullptr, 0);
                result.residualPidCount = CollectResiduals(
                    arch, result.residualPids, kMaxResiduals);
                result.residualProcesses = result.residualPidCount;
            }
            result.hostGone = !GPT::IsGlassPlatformRunning(arch);
            result.failedPhase = L"already_stopped";
            result.elapsedMs = static_cast<DWORD>(GetTickCount64() - t0);
            GlassUlTrace::Logf(
                "H102", "GlassStop.cpp:Request", "stop_result",
                "\"reason\":\"%s\",\"phase\":\"host_dead_probe\",\"host_gone\":%d,"
                "\"residuals\":%u,\"ms\":%lu",
                reason,
                result.hostGone ? 1 : 0,
                result.residualProcesses,
                static_cast<unsigned long>(result.elapsedMs));
            for (unsigned i = 0; i < result.residualPidCount && i < kMaxResiduals; ++i)
            {
                GlassUlTrace::Logf(
                    "H102", "GlassStop.cpp:Request", "stop_residual",
                    "\"reason\":\"%s\",\"target_pid\":%lu",
                    reason,
                    static_cast<unsigned long>(result.residualPids[i]));
            }
            return result;
        }

        // (0) Платформа уже не работает: остатки всё равно измеряем — их показывает
        // установщик и страница «закрыть процессы».
        if (!GPT::IsGlassPlatformRunning(arch))
        {
            result.hostGone = true;
            result.failedPhase = L"already_stopped";
            result.residualPidCount = CollectResiduals(
                arch, result.residualPids, kMaxResiduals);
            result.residualProcesses = result.residualPidCount;
            if (options.wakeResiduals)
                (void)WakeResidualProcesses(arch, nullptr, 0);
            result.elapsedMs = static_cast<DWORD>(GetTickCount64() - t0);
            GlassUlTrace::Logf(
                "H102", "GlassStop.cpp:Request", "stop_result",
                "\"reason\":\"%s\",\"phase\":\"already_stopped\",\"host_gone\":1,"
                "\"residuals\":%u,\"ms\":%lu",
                reason,
                result.residualProcesses,
                static_cast<unsigned long>(result.elapsedMs));
            return result;
        }

        // (1) Сигнал. Порядок «сигнал → busy» сохранён: LifecycleBusy при busy+Running
        // без Stop auto-clear'ится, поэтому стоп должен уйти первым.
        if (!GPT::SignalPlatformStop(arch))
        {
            result.failedPhase = L"signal";
            result.elapsedMs = static_cast<DWORD>(GetTickCount64() - t0);
            GlassUlTrace::Logf(
                "H102", "GlassStop.cpp:Request", "stop_phase",
                "\"reason\":\"%s\",\"phase\":\"signal\",\"ok\":0", reason);
            return result;
        }

        // (2) Busy только на время сигнала и сразу отпускаем.
        // R12: держать busy всё время ожидания нельзя. Инициатор (GlassShellExt в explorer
        // или в DllHost) сам инжектирован и выгружается вместе с остальными; если его
        // поток умрёт на ожидании, busy останется залипшим навсегда (прогон 12: stop_request
        // из DllHost 6284 → stop_phase busy → и ни wait_host, ни stop_result).
        const bool busyAcquired = GPT::TryBeginLifecycleBusy(arch);
        if (busyAcquired)
            GPT::EndLifecycleBusy(arch);
        GlassUlTrace::Logf(
            "H102", "GlassStop.cpp:Request", "stop_phase",
            "\"reason\":\"%s\",\"phase\":\"busy\",\"acquired\":%d,\"held\":0",
            reason,
            busyAcquired ? 1 : 0);

        // (2b) Немедленный «пинок» остаткам: WM_NULL окнам и потокам. Делаем это ДО
        // ожидания, потому что инициатор может исчезнуть вместе со своим процессом
        // (ShellExt в explorer). Позже, в фазе 5, пинок повторим для тех, кто появился
        // или не отреагировал.
        if (options.wakeResiduals)
            (void)WakeResidualProcesses(arch, nullptr, 0);

        // (3) Ожидание штатного завершения: хост снимает хуки, рассылает broadcast,
        // дожимает остатки и отпускает мьютекс платформы (gracefulMs=0 — не ждём).
        result.hostGone =
            GPT::WaitForPlatformStopped(arch, options.gracefulMs);
        GlassUlTrace::Logf(
            "H102", "GlassStop.cpp:Request", "stop_phase",
            "\"reason\":\"%s\",\"phase\":\"wait_host\",\"gone\":%d,\"ms\":%lu",
            reason,
            result.hostGone ? 1 : 0,
            static_cast<unsigned long>(GetTickCount64() - t0));

        // (4) Хост не ушёл и разрешено — останавливаем задачу (это и есть «доработанный»
        // путь ручной остановки: после него процессы уходят по abandoned HostAlive).
        if (!result.hostGone && options.allowTaskStop)
        {
            result.hostKilledByTask = GPT::StopOrphanAutorunTaskInstance(arch);
            GlassUlTrace::Logf(
                "H102", "GlassStop.cpp:Request", "stop_phase",
                "\"reason\":\"%s\",\"phase\":\"task_stop\",\"ok\":%d",
                reason,
                result.hostKilledByTask ? 1 : 0);

            const DWORD afterKill =
                options.hostGoneMs > 0 ? options.hostGoneMs : 3000;
            result.hostGone = GPT::WaitForPlatformStopped(arch, afterKill);
            GlassUlTrace::Logf(
                "H102", "GlassStop.cpp:Request", "stop_phase",
                "\"reason\":\"%s\",\"phase\":\"after_task_stop\",\"gone\":%d,\"ms\":%lu",
                reason,
                result.hostGone ? 1 : 0,
                static_cast<unsigned long>(GetTickCount64() - t0));
        }

        // (5) Остатки: сначала снимок, затем (если просили) побуждение — WM_NULL окнам
        // и потокам. Принудительной выгрузки нет ни при каком значении опций.
        result.residualPidCount = CollectResiduals(
            arch, result.residualPids, kMaxResiduals);
        result.residualProcesses = result.residualPidCount;

        if (options.wakeResiduals && result.residualProcesses > 0)
        {
            (void)WakeResidualProcesses(arch, nullptr, 0);
            result.residualPidCount = CollectResiduals(
                arch, result.residualPids, kMaxResiduals);
            result.residualProcesses = result.residualPidCount;
            GlassUlTrace::Logf(
                "H102", "GlassStop.cpp:Request", "stop_phase",
                "\"reason\":\"%s\",\"phase\":\"wake_residuals\",\"left\":%u",
                reason,
                result.residualProcesses);
        }

        // (6) Состояние для UI/присутствия: sticky пересчитываем. Busy уже снят в фазе 2.
        (void)GPT::RefreshInjectResidualSticky(arch);

        if (!result.hostGone && result.failedPhase[0] == L'\0')
            result.failedPhase = L"host";
        else if (result.hostGone)
            result.failedPhase = L"";

        result.elapsedMs = static_cast<DWORD>(GetTickCount64() - t0);
        GlassUlTrace::Logf(
            "H102", "GlassStop.cpp:Request", "stop_result",
            "\"reason\":\"%s\",\"phase\":\"%s\",\"host_gone\":%d,\"task_stop\":%d,"
            "\"residuals\":%u,\"ms\":%lu",
            reason,
            *result.failedPhase ? "partial" : "done",
            result.hostGone ? 1 : 0,
            result.hostKilledByTask ? 1 : 0,
            result.residualProcesses,
            static_cast<unsigned long>(result.elapsedMs));

        for (unsigned i = 0; i < result.residualPidCount && i < kMaxResiduals; ++i)
        {
            GlassUlTrace::Logf(
                "H102", "GlassStop.cpp:Request", "stop_residual",
                "\"reason\":\"%s\",\"target_pid\":%lu",
                reason,
                static_cast<unsigned long>(result.residualPids[i]));
        }
        return result;
    }

    Result Request() noexcept
    {
        Options options{};
        options.reason = Reason::User;
        return Request(GPT::NativePlatformArch(), options);
    }
}
