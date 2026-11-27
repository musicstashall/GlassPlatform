#include "SharedPch.h"
#include <OpenGlassDwmGuard.h>
#include <OpenGlassDwmGuardPolicy.h>
#include <OpenGlassHostLaunch.h>
#include <OpenGlassHostState.h>
#include <TaskSchedulerCom.h>

#include <TlHelp32.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace Shared::OpenGlassDwmGuard
{
    namespace
    {
        using namespace OpenGlassDwmGuardPolicy;

        constexpr DWORD kPollFastMs = 200;
        constexpr DWORD kPollSlowMs = 500;
        constexpr ULONGLONG kFastPollMs = 8000; // первые 8 с — учащённый опрос

        // Кросс-процессные объекты ядра: единственный сторож на сессию + событие смены состояния.
        constexpr LPCWSTR kWatchMutexName = L"Local\\GlassPlatform.OpenGlassDwmGuard.Watch";
        constexpr LPCWSTR kStateEventName = L"Local\\GlassPlatform.OpenGlassDwmGuard.StateChanged";

        constexpr LPCWSTR kValueArmedAt = L"ArmedAt";
        constexpr LPCWSTR kValueArmedBy = L"ArmedBy";
        constexpr LPCWSTR kValueOsBuild = L"OsBuild";
        constexpr LPCWSTR kValueOgVersion = L"OgVersion";
        constexpr LPCWSTR kValueQuarantine = L"Quarantine";
        constexpr LPCWSTR kValueQuarantinedAt = L"QuarantinedAt";
        constexpr LPCWSTR kValueReason = L"Reason";
        constexpr LPCWSTR kValueLastResult = L"LastResult";
        constexpr LPCWSTR kValueCrashTimes = L"CrashTimes";

        struct DwmIdentity
        {
            DWORD pid = 0;
            ULONGLONG creation = 0; // FILETIME как ULARGE_INTEGER; 0 — не прочитано
            bool present = false;
        };

        std::mutex g_gate; // единственное наблюдение в процессе (GS x64/x86, Setup)
        std::atomic<DWORD> g_state{ kStateIdle };
        std::atomic<bool> g_workerRunning{ false };
        std::atomic<bool> g_stopRequested{ false };
        DwmIdentity g_baseline{};
        ULONGLONG g_armedTick = 0; // GetTickCount64 на момент Arm
        ULONGLONG g_armedAtFileTime = 0;
        std::wstring g_ogVersion;
        std::vector<unsigned long long> g_crashTimes; // FILETIME UTC
        size_t g_totalCrashes = 0;      // всех крашей за этот цикл наблюдения (не стареет)
        HANDLE g_watchMutex = nullptr;  // именованный мьютекс — снимается при выходе сторожа
        HANDLE g_stateEvent = nullptr;  // авто-сброс; взводится на каждой смене состояния

        void SignalStateChanged() noexcept
        {
            if (g_stateEvent)
                (void)SetEvent(g_stateEvent);
        }

        void ReleaseCrossProcessObjects() noexcept
        {
            // Мьютекс создаётся без первоначального владения и служит «токеном существования»:
            // выход любого владеющего процесса закрывает его дескриптор → объект уничтожается.
            if (g_watchMutex)
            {
                CloseHandle(g_watchMutex);
                g_watchMutex = nullptr;
            }
            if (g_stateEvent)
            {
                CloseHandle(g_stateEvent);
                g_stateEvent = nullptr;
            }
        }

        [[nodiscard]] ULONGLONG FileTimeNowUtc() noexcept
        {
            FILETIME ft{};
            GetSystemTimeAsFileTime(&ft);
            ULARGE_INTEGER value{};
            value.LowPart = ft.dwLowDateTime;
            value.HighPart = ft.dwHighDateTime;
            return value.QuadPart;
        }

        [[nodiscard]] DWORD QueryOsBuild() noexcept
        {
            RTL_OSVERSIONINFOW rovi{};
            rovi.dwOSVersionInfoSize = sizeof(rovi);
            using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
            static RtlGetVersionFn pRtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
                GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
            if (!pRtlGetVersion || pRtlGetVersion(&rovi) != 0)
                return 0;
            return rovi.dwBuildNumber;
        }

        [[nodiscard]] DWORD CurrentSessionId() noexcept
        {
            DWORD sid = 0;
            return ProcessIdToSessionId(GetCurrentProcessId(), &sid) ? sid : 0;
        }

        /// Снимок dwm.exe текущей сессии. false — сбой самого опроса (не краш, тик пропускаем),
        /// true — валидный ответ, в том числе «dwm.exe отсутствует».
        [[nodiscard]] bool QueryDwmIdentity(DWORD sessionId, DwmIdentity& out) noexcept
        {
            out = {};
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap == INVALID_HANDLE_VALUE)
                return false;

            PROCESSENTRY32W pe{};
            pe.dwSize = sizeof(pe);
            bool found = false;
            DWORD pid = 0;
            if (!Process32FirstW(snap, &pe))
            {
                CloseHandle(snap);
                return false; // сбой перечисления — не делаем выводов о dwm.exe
            }
            do
            {
                if (_wcsicmp(pe.szExeFile, L"dwm.exe") != 0)
                    continue;
                DWORD sid = 0;
                if (!ProcessIdToSessionId(pe.th32ProcessID, &sid))
                    continue;
                if (sid != sessionId)
                    continue;
                found = true;
                pid = pe.th32ProcessID;
                break;
            } while (Process32NextW(snap, &pe));
            CloseHandle(snap);
            if (!found)
                return true; // перечисление состоялось, dwm.exe в сессии нет

            out.pid = pid;
            out.present = true;
            if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
            {
                FILETIME created{}, exited{}, kernel{}, user{};
                if (GetProcessTimes(process, &created, &exited, &kernel, &user))
                {
                    ULARGE_INTEGER value{};
                    value.LowPart = created.dwLowDateTime;
                    value.HighPart = created.dwHighDateTime;
                    out.creation = value.QuadPart;
                }
                CloseHandle(process);
            }
            return true;
        }

        [[nodiscard]] bool OpenMarkerForRead(HKEY& out) noexcept
        {
            return RegOpenKeyExW(HKEY_CURRENT_USER, kMarkerSubKey, 0, KEY_READ, &out) == ERROR_SUCCESS;
        }

        [[nodiscard]] bool CreateMarkerForWrite(HKEY& out) noexcept
        {
            DWORD disposition = 0;
            return RegCreateKeyExW(HKEY_CURRENT_USER, kMarkerSubKey, 0, nullptr,
                       REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &out, &disposition) == ERROR_SUCCESS;
        }

        void SetDwordValue(HKEY key, LPCWSTR name, DWORD value) noexcept
        {
            (void)RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        }

        void SetQwordValue(HKEY key, LPCWSTR name, ULONGLONG value) noexcept
        {
            (void)RegSetValueExW(key, name, 0, REG_QWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        }

        [[nodiscard]] DWORD ReadDwordValue(HKEY key, LPCWSTR name, DWORD fallback) noexcept
        {
            DWORD type = 0, value = 0, size = sizeof(value);
            if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) != ERROR_SUCCESS
                || type != REG_DWORD)
                return fallback;
            return value;
        }

        [[nodiscard]] ULONGLONG ReadQwordValue(HKEY key, LPCWSTR name, ULONGLONG fallback) noexcept
        {
            DWORD type = 0;
            ULONGLONG value = 0;
            DWORD size = sizeof(value);
            if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) != ERROR_SUCCESS
                || (type != REG_QWORD && type != REG_BINARY) || size != sizeof(value))
                return fallback;
            return value;
        }

        void WriteCrashTimes(HKEY key) noexcept
        {
            const DWORD bytes = static_cast<DWORD>(g_crashTimes.size() * sizeof(ULONGLONG));
            if (bytes == 0)
            {
                (void)RegDeleteValueW(key, kValueCrashTimes);
                return;
            }
            (void)RegSetValueExW(key, kValueCrashTimes, 0, REG_BINARY,
                                 reinterpret_cast<const BYTE*>(g_crashTimes.data()), bytes);
        }

        [[nodiscard]] std::vector<unsigned long long> ReadCrashTimes(HKEY key) noexcept
        {
            DWORD type = 0, size = 0;
            if (RegQueryValueExW(key, kValueCrashTimes, nullptr, &type, nullptr, &size) != ERROR_SUCCESS
                || type != REG_BINARY || size == 0 || (size % sizeof(ULONGLONG)) != 0)
                return {};

            std::vector<unsigned long long> values(size / sizeof(ULONGLONG));
            if (RegQueryValueExW(key, kValueCrashTimes, nullptr, &type,
                                 reinterpret_cast<BYTE*>(values.data()), &size) != ERROR_SUCCESS)
                return {};
            if (values.size() > kMaxCrashTimesStored)
                values.erase(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(values.size() - kMaxCrashTimesStored));
            return values;
        }

        [[nodiscard]] std::wstring ReadStringValue(HKEY key, LPCWSTR name) noexcept
        {
            DWORD type = 0, size = 0;
            if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS
                || type != REG_SZ || size == 0 || size > 512)
                return {};
            std::wstring value(size / sizeof(wchar_t), L'\0');
            if (RegQueryValueExW(key, name, nullptr, &type,
                                 reinterpret_cast<BYTE*>(value.data()), &size) != ERROR_SUCCESS)
                return {};
            while (!value.empty() && value.back() == L'\0')
                value.pop_back();
            return value;
        }

        void WriteMarkerArmed(DWORD armedBy) noexcept
        {
            HKEY key = nullptr;
            if (!CreateMarkerForWrite(key))
            {
                OutputDebugStringW(L"[OpenGlassDwmGuard] marker write failed (Armed)\n");
                return;
            }
            SetQwordValue(key, kValueArmedAt, g_armedAtFileTime);
            SetDwordValue(key, kValueArmedBy, armedBy);
            SetDwordValue(key, kValueOsBuild, QueryOsBuild());
            if (!g_ogVersion.empty())
                (void)RegSetValueExW(key, kValueOgVersion, 0, REG_SZ,
                                     reinterpret_cast<const BYTE*>(g_ogVersion.c_str()),
                                     static_cast<DWORD>((g_ogVersion.size() + 1) * sizeof(wchar_t)));
            else
                (void)RegDeleteValueW(key, kValueOgVersion);
            SetDwordValue(key, kValueLastResult, kLastResultNone);
            WriteCrashTimes(key);
            RegCloseKey(key);
        }

        void WriteMarkerQuarantine(DWORD reason, ULONGLONG nowFileTime) noexcept
        {
            HKEY key = nullptr;
            if (!CreateMarkerForWrite(key))
            {
                OutputDebugStringW(L"[OpenGlassDwmGuard] marker write failed (Quarantine)\n");
                return;
            }
            WriteCrashTimes(key);
            SetQwordValue(key, kValueQuarantinedAt, nowFileTime);
            SetDwordValue(key, kValueReason, reason);
            SetDwordValue(key, kValueLastResult, kLastResultQuarantined);
            // Quarantine пишем последним: читатель видит целостную пару «метки краша + флаг».
            SetDwordValue(key, kValueQuarantine, 1);
            RegCloseKey(key);
        }

        void WriteMarkerLastResult(DWORD lastResult) noexcept
        {
            HKEY key = nullptr;
            if (!CreateMarkerForWrite(key))
                return;
            SetDwordValue(key, kValueLastResult, lastResult);
            RegCloseKey(key);
        }

        /// Служба → ручной запуск. Если была SERVICE_DISABLED — НЕ трогаем (это явный выбор пользователя,
        /// автозапуск и так выключен; изменение DISABLED→DEMAND не должно быть молчаливым).
        void DisableOgServiceAutostartKeepingDisabled() noexcept
        {
            SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            if (!scm)
                return;
            SC_HANDLE svc = OpenServiceW(scm, Shared::OpenGlassHostState::kOpenGlassHostServiceName,
                SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG);
            CloseServiceHandle(scm);
            if (!svc)
                return;

            DWORD configBytes = 0;
            (void)QueryServiceConfigW(svc, nullptr, 0, &configBytes);
            QUERY_SERVICE_CONFIGW* cfg = nullptr;
            if (configBytes != 0)
                cfg = static_cast<QUERY_SERVICE_CONFIGW*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, configBytes));
            if (cfg && QueryServiceConfigW(svc, cfg, configBytes, &configBytes))
            {
                if (cfg->dwStartType != SERVICE_DISABLED && cfg->dwStartType != SERVICE_DEMAND_START)
                {
                    (void)ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_DEMAND_START,
                        SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                }
            }
            if (cfg)
                HeapFree(GetProcessHeap(), 0, cfg);
            CloseServiceHandle(svc);
        }

        [[nodiscard]] bool DisableOgLegacyHostTask() noexcept
        {
            return GlassShared::RunSchtasksOnTask(L"OpenGlass Host", L"/Change /DISABLE")
                || GlassShared::RunSchtasksOnTask(L"OpenGlass Legacy Host", L"/Change /DISABLE");
        }

        void Quarantine(Action action) noexcept
        {
            const DWORD reason = action == Action::QuarantineDanger ? kReasonDangerSecondCrash
                : action == Action::QuarantineFuse ? kReasonFuseTotal
                : kReasonPairInWindow;
            const ULONGLONG nowFileTime = FileTimeNowUtc();

            {
                std::lock_guard<std::mutex> lock(g_gate);
                WriteMarkerQuarantine(reason, nowFileTime);
            }
            g_state.store(kStateQuarantined, std::memory_order_release);
            SignalStateChanged();

            wchar_t msg[192]{};
            swprintf_s(msg, L"[OpenGlassDwmGuard] dwm.exe crashed within %u s of OpenGlass start; OG stopped, autostart disabled (reason=%lu)\n",
                       kWatchWindowSeconds, reason);
            OutputDebugStringW(msg);

            // 1) Остановка оркестрации (служба или legacy-задача) — общая логика платформы.
            (void)Shared::OpenGlassHostLaunch::TryShutdown();
            // 2) Отключение автозапуска: служба → DEMAND_START (DISABLED не трогаем), legacy-задача → /DISABLE.
            DisableOgServiceAutostartKeepingDisabled();
            (void)DisableOgLegacyHostTask();

            g_workerRunning.store(false, std::memory_order_release);
            ReleaseCrossProcessObjects();
        }

        void FinishWatch(DWORD lastResult) noexcept
        {
            WriteMarkerLastResult(lastResult);
            g_state.store(kStateIdle, std::memory_order_release);
            g_workerRunning.store(false, std::memory_order_release);
            SignalStateChanged();
            ReleaseCrossProcessObjects();
        }

        void HandleCrash() noexcept
        {
            const ULONGLONG nowFileTime = FileTimeNowUtc();
            const ULONGLONG elapsedMs = GetTickCount64() - g_armedTick;
            const bool instant = elapsedMs <= static_cast<ULONGLONG>(kDangerThresholdMs);

            Action action = Action::Suspect;
            {
                std::lock_guard<std::mutex> lock(g_gate);
                AppendCrash(g_crashTimes, nowFileTime);
                ++g_totalCrashes;
                const bool wasDanger = g_state.load(std::memory_order_acquire) == kStateDanger;
                action = EvaluateCrash(wasDanger, g_crashTimes.size(), g_totalCrashes, instant);
                HKEY key = nullptr;
                if (CreateMarkerForWrite(key))
                {
                    WriteCrashTimes(key);
                    RegCloseKey(key);
                }
            }

            switch (action)
            {
            case Action::EnterDanger:
                g_state.store(kStateDanger, std::memory_order_release);
                SignalStateChanged();
                OutputDebugStringW(L"[OpenGlassDwmGuard] immediate dwm.exe crash after OG start; danger mode\n");
                break;
            case Action::Suspect:
                g_state.store(kStateSuspect, std::memory_order_release);
                SignalStateChanged();
                OutputDebugStringW(L"[OpenGlassDwmGuard] dwm.exe crash after OG start; watching\n");
                break;
            default:
                Quarantine(action);
                break;
            }
        }

        void WorkerMain() noexcept
        {
            const DWORD session = CurrentSessionId();
            const ULONGLONG t0 = GetTickCount64();
            for (;;)
            {
                if (g_stopRequested.load(std::memory_order_acquire))
                {
                    FinishWatch(kLastResultAborted);
                    return;
                }
                if (GetSystemMetrics(SM_SHUTTINGDOWN))
                {
                    FinishWatch(kLastResultShutdown);
                    return;
                }
                const ULONGLONG elapsed = GetTickCount64() - t0;
                if (elapsed >= static_cast<ULONGLONG>(kMaxWatchSeconds) * 1000ull)
                {
                    FinishWatch(kLastResultTimeout);
                    return;
                }
                if (g_state.load(std::memory_order_acquire) == kStateWatching
                    && elapsed >= static_cast<ULONGLONG>(kCleanExitSeconds) * 1000ull)
                {
                    // Ни одного краша за окно тишины — досрочный чистый выход.
                    FinishWatch(kLastResultClean);
                    return;
                }

                DwmIdentity current;
                if (QueryDwmIdentity(session, current))
                {
                    bool crash = false;
                    {
                        std::lock_guard<std::mutex> lock(g_gate);
                        if (g_baseline.present)
                        {
                            const bool pidChanged = !current.present || current.pid != g_baseline.pid;
                            // PID мог быть переиспользован после краша: то же имя ≠ тот же процесс.
                            const bool identityChanged = !pidChanged && current.present
                                && g_baseline.creation != 0 && current.creation != 0
                                && current.creation != g_baseline.creation;
                            if (pidChanged || identityChanged)
                            {
                                crash = true;
                                g_baseline = current.present ? current : DwmIdentity{};
                            }
                        }
                        else if (current.present)
                        {
                            g_baseline = current; // появился после отсутствия — тихий adopt
                        }
                    }
                    if (crash)
                        HandleCrash();
                    if (!g_workerRunning.load(std::memory_order_acquire))
                        return; // карантин выполнен внутри HandleCrash
                }

                Sleep(elapsed < kFastPollMs ? kPollFastMs : kPollSlowMs);
            }
        }
    } // namespace

    bool Arm(DWORD armedBy, const std::wstring& ogVersion) noexcept
    {
        std::lock_guard<std::mutex> lock(g_gate);
        if (g_workerRunning.load(std::memory_order_acquire))
            return true; // уже наблюдаем — повторное вооружение не нужно

        // Именованный мьютекс: единственный сторож на сессию. Фоновый процесс не нужен —
        // за сторожа отвечает тот, кто запустил OG (Setup или GlassSettings).
        // bInitialOwner=FALSE: владение не требуется, поток не имеет значения.
        if (g_watchMutex)
        {
            CloseHandle(g_watchMutex);
            g_watchMutex = nullptr;
        }
        g_watchMutex = CreateMutexW(nullptr, FALSE, kWatchMutexName);
        if (!g_watchMutex)
            OutputDebugStringW(L"[OpenGlassDwmGuard] named mutex unavailable; in-process single-watch only\n");
        else if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            // Наблюдение уже ведёт другой процесс — он и выполнит Stop/Disable и запишет маркер.
            CloseHandle(g_watchMutex);
            g_watchMutex = nullptr;
            OutputDebugStringW(L"[OpenGlassDwmGuard] watch already owned by another process\n");
            return true;
        }

        // Именованное авто-сброс-событие смены состояния (UI ждёт вместо опроса).
        if (!g_stateEvent)
            g_stateEvent = CreateEventW(nullptr, FALSE, FALSE, kStateEventName);

        const DWORD session = CurrentSessionId();
        DwmIdentity baseline{};
        if (!QueryDwmIdentity(session, baseline))
            baseline = {}; // сбой опроса: первую встречу dwm примем за базовую

        g_baseline = baseline;
        g_armedAtFileTime = FileTimeNowUtc();
        g_armedTick = GetTickCount64();
        g_ogVersion = ogVersion;
        g_totalCrashes = 0;
        g_stopRequested.store(false, std::memory_order_release);

        // История крашей переживает рестарт сессии: вторая половина «циклического»
        // сбоя может случиться уже в следующей сессии. Окно привязано к now.
        g_crashTimes.clear();
        HKEY readKey = nullptr;
        if (OpenMarkerForRead(readKey))
        {
            g_crashTimes = ReadCrashTimes(readKey);
            RegCloseKey(readKey);
        }
        DropExpired(g_crashTimes, g_armedAtFileTime);

        WriteMarkerArmed(armedBy);

        g_state.store(kStateWatching, std::memory_order_release);
        SignalStateChanged();
        g_workerRunning.store(true, std::memory_order_release);
        try
        {
            std::thread(WorkerMain).detach();
        }
        catch (...)
        {
            g_workerRunning.store(false, std::memory_order_release);
            g_state.store(kStateIdle, std::memory_order_release);
            WriteMarkerLastResult(kLastResultAborted);
            ReleaseCrossProcessObjects();
            return false;
        }
        return true;
    }

    void Disarm() noexcept
    {
        g_stopRequested.store(true, std::memory_order_release);
    }

    DWORD QueryState() noexcept
    {
        return g_state.load(std::memory_order_acquire);
    }

    bool IsWatching() noexcept
    {
        const DWORD state = g_state.load(std::memory_order_acquire);
        return state == kStateWatching || state == kStateSuspect || state == kStateDanger;
    }

    bool ReadMarker(MarkerInfo& out) noexcept
    {
        out = MarkerInfo{};
        HKEY key = nullptr;
        if (!OpenMarkerForRead(key))
            return false;

        out.armedBy = ReadDwordValue(key, kValueArmedBy, 0);
        out.osBuild = ReadDwordValue(key, kValueOsBuild, 0);
        out.quarantine = ReadDwordValue(key, kValueQuarantine, 0);
        out.reason = ReadDwordValue(key, kValueReason, 0);
        out.lastResult = ReadDwordValue(key, kValueLastResult, 0);
        out.armedAt = ReadQwordValue(key, kValueArmedAt, 0);
        out.quarantinedAt = ReadQwordValue(key, kValueQuarantinedAt, 0);
        out.ogVersion = ReadStringValue(key, kValueOgVersion);
        auto crashes = ReadCrashTimes(key);
        RegCloseKey(key);
        DropExpired(crashes, FileTimeNowUtc());
        out.crashCount = static_cast<DWORD>(crashes.size());
        return true;
    }

    bool IsQuarantined() noexcept
    {
        MarkerInfo info{};
        return ReadMarker(info) && info.quarantine != 0;
    }

    bool ShouldSuppressOpenGlassStart(const std::wstring& installedOgVersion) noexcept
    {
        MarkerInfo info{};
        if (!ReadMarker(info) || info.quarantine == 0)
            return false;
        // Строго более новая версия OG — автоматическое снятие карантина:
        // возможное исправление нестабильности, даём свежий шанс.
        if (!info.ogVersion.empty() && !installedOgVersion.empty()
            && IsVersionStrictlyGreater(installedOgVersion, info.ogVersion))
        {
            OutputDebugStringW(L"[OpenGlassDwmGuard] quarantine cleared: OpenGlass version increased\n");
            return !ClearQuarantine();
        }
        return true;
    }

    bool ClearQuarantine() noexcept
    {
        HKEY key = nullptr;
        if (!CreateMarkerForWrite(key))
            return false;
        SetDwordValue(key, kValueQuarantine, 0);
        SetDwordValue(key, kValueReason, 0);
        // QuarantinedAt и история CrashTimes сохраняются для диагностики.
        RegCloseKey(key);
        return true;
    }
}
