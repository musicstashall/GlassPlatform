#include "CompatPch.h"
#include "GlassCompat.h"
#include "CrashGuard.h"
#include "HostPidHelper.h"
#include "HostShellWatchdog.h"
#include "CoreWindowHostInject.h"
#include "AttachList.h"
#include <GlassPlatformTask.h>
#include <Helpers/DwmGlassOpacity.h>
#include <OpenGlassDwmPolicy.h>
#include <PlatformPresenceMonitor.h>
#include <RootRegistryMigration.h>
#include <PlatformPresenceState.h>
#include <InjectProcessMap.h>
#include <OpenGlassHostCommand.h>
#include <ProcessBlockList.h>
#include <TrayXamlPolicyMap.h>
#include <ReloadRenderFlags.h>
#include <ForceUnloadGlassModules.h>
#include <GlassUlTrace.h>
#include <windows.h>
#include <sddl.h>
#include <string>
#include <utility>
#include <vector>

#ifndef WM_DWMCOLORIZATIONCOLORCHANGED
#define WM_DWMCOLORIZATIONCOLORCHANGED 0x0320
#endif

#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER)
#include <GlassLogClient.h>
#endif

// ---- Внешнее объявление колбэка, определённого в GlassCompat.cpp ----
extern "C" void CALLBACK GlassCompat_WinEventProc(
    HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);

namespace
{
    static HWINEVENTHOOK g_globalHook = nullptr;
    // Отдельный хук: смена переднего плана порождает событие в процессе окна с фокусом (INCONTEXT),
    // минуя UIPI «высокий хост → низкий клиент» для SendMessage/NotifyWinEvent.
    static HWINEVENTHOOK g_foregroundWinEventHook = nullptr;
    // CREATE..SHOW не покрывает UNCLOAKED (0x8018): ShellHost QS Attach на cloaked SHOW
    // видит пустое XAML-дерево; патч — на UNCLOAKED (см. WinEventHandler).
    static HWINEVENTHOOK g_uncloakWinEventHook = nullptr;

    // Хэндл именованного file mapping с PID хоста.
    // Не закрываем до ClearHostPid — объект маппинга должен оставаться
    // живым всё время работы хоста, иначе ReadHostPid() в других процессах
    // (и в самом хосте) вернёт 0, и хост инжектирует сам себя.
    static HANDLE s_hostPidMapping = nullptr;
    static HANDLE s_reloadSemaphore = nullptr;
    static HANDLE s_reloadGenMapping = nullptr;
    static HANDLE s_reloadFlagsMapping = nullptr;
    static HANDLE s_reloadScopeMapping = nullptr;
    static HANDLE s_trayXamlPolicyMapping = nullptr;
    // Manual-reset: хост выставляет в done: перед CloseHandle(mutex).
    static HANDLE s_hostShutdownEvent = nullptr;

    // Приёмник запросов на закрытие. Планировщик заданий при End task / переходе
    // на батарею сначала рассылает WM_CLOSE по top-level окнам потоков процесса и
    // только потом убивает его. Без окна хост получал сразу TerminateProcess:
    // StopEvent и HostShutdown не выставлялись никогда, и инжектированные модули
    // оставались висеть в чужих процессах.
    // HWND_MESSAGE не подходит — message-only окна в такой рассылке не участвуют.
    constexpr wchar_t kHostCloseSinkWndClass[] = L"GlassPlatformHostCloseSink";

    static HWND s_hostCloseSinkHwnd = nullptr;
    static HANDLE s_hostCloseSinkStopEvent = nullptr; // не владеем
    // WM_CLOSE / завершение сеанса: штатная остановка задачи, без удалённой
    // принудительной выгрузки в других процессах.
    static bool s_stopFromCloseMessage = false;

    static void RequestHostStopFromWindowMessage() noexcept
    {
        s_stopFromCloseMessage = true;
        if (s_hostCloseSinkStopEvent)
            SetEvent(s_hostCloseSinkStopEvent);
    }

    static LRESULT CALLBACK HostCloseSinkWndProc(
        HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept
    {
        switch (msg)
        {
        case WM_CLOSE:
            RequestHostStopFromWindowMessage();
            return 0;

        case WM_QUERYENDSESSION:
            // Выгрузку здесь не начинаем: сеанс ещё может быть отменён другим
            // приложением, и платформа осталась бы выключенной без причины.
            return TRUE;

        case WM_ENDSESSION:
            if (wParam != FALSE)
                RequestHostStopFromWindowMessage();
            return 0;

        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    /// Невидимое top-level окно нулевого размера; WS_EX_TOOLWINDOW убирает его
    /// из Alt+Tab и панели задач.
    [[nodiscard]] static HWND CreateHostCloseSinkWindow(HMODULE hSelf, HANDLE hStopEvent) noexcept
    {
        s_hostCloseSinkStopEvent = hStopEvent;

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = HostCloseSinkWndProc;
        wc.hInstance = hSelf;
        wc.lpszClassName = kHostCloseSinkWndClass;
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;

        return CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kHostCloseSinkWndClass,
            L"GlassPlatform Host",
            WS_POPUP,
            0, 0, 0, 0,
            nullptr,
            nullptr,
            hSelf,
            nullptr);
    }

    static void DestroyHostCloseSinkWindow(HMODULE hSelf) noexcept
    {
        if (s_hostCloseSinkHwnd)
        {
            DestroyWindow(s_hostCloseSinkHwnd);
            s_hostCloseSinkHwnd = nullptr;
        }
        s_hostCloseSinkStopEvent = nullptr;
        (void)UnregisterClassW(kHostCloseSinkWndClass, hSelf);
    }

    static void StopLogClientForShortLivedMain() noexcept
    {
#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER)
        Shared::Log::GlassLogClient::Instance().Stop();
#endif
    }

    static void SignalHostShutdownComplete() noexcept
    {
        if (s_hostShutdownEvent)
            SetEvent(s_hostShutdownEvent);
    }

    static void CloseHostShutdownEvent() noexcept
    {
        if (s_hostShutdownEvent)
        {
            CloseHandle(s_hostShutdownEvent);
            s_hostShutdownEvent = nullptr;
        }
    }

    [[nodiscard]] static bool CreateReloadRenderResources(SECURITY_ATTRIBUTES* pSa) noexcept
    {
        s_reloadGenMapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            pSa,
            PAGE_READWRITE,
            0,
            sizeof(DWORD),
            kReloadGenerationMappingName);

        if (!s_reloadGenMapping)
            return false;

        void* pView = MapViewOfFile(s_reloadGenMapping, FILE_MAP_WRITE, 0, 0, sizeof(DWORD));
        if (pView)
        {
            *static_cast<DWORD*>(pView) = 0;
            UnmapViewOfFile(pView);
        }

        s_reloadFlagsMapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            pSa,
            PAGE_READWRITE,
            0,
            sizeof(DWORD),
            kReloadRenderFlagsMappingName);
        if (!s_reloadFlagsMapping)
        {
            CloseHandle(s_reloadGenMapping);
            s_reloadGenMapping = nullptr;
            return false;
        }

        void* pFlagsView = MapViewOfFile(s_reloadFlagsMapping, FILE_MAP_WRITE, 0, 0, sizeof(DWORD));
        if (pFlagsView)
        {
            *static_cast<DWORD*>(pFlagsView) = kReloadRenderFlagAll;
            UnmapViewOfFile(pFlagsView);
        }

        s_reloadScopeMapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            pSa,
            PAGE_READWRITE,
            0,
            sizeof(DWORD),
            kReloadRootLiveScopeMappingName);
        if (!s_reloadScopeMapping)
        {
            CloseHandle(s_reloadFlagsMapping);
            s_reloadFlagsMapping = nullptr;
            CloseHandle(s_reloadGenMapping);
            s_reloadGenMapping = nullptr;
            return false;
        }

        void* pScopeView = MapViewOfFile(s_reloadScopeMapping, FILE_MAP_WRITE, 0, 0, sizeof(DWORD));
        if (pScopeView)
        {
            *static_cast<DWORD*>(pScopeView) = 0;
            UnmapViewOfFile(pScopeView);
        }

        s_reloadSemaphore = CreateSemaphoreW(pSa, 0, 65535, kReloadRenderSemaphoreName);
        if (!s_reloadSemaphore)
        {
            CloseHandle(s_reloadFlagsMapping);
            s_reloadFlagsMapping = nullptr;
            CloseHandle(s_reloadGenMapping);
            s_reloadGenMapping = nullptr;
            return false;
        }
        return true;
    }

    static void CloseReloadRenderResources() noexcept
    {
        if (s_reloadSemaphore)
        {
            CloseHandle(s_reloadSemaphore);
            s_reloadSemaphore = nullptr;
        }
        if (s_reloadFlagsMapping)
        {
            CloseHandle(s_reloadFlagsMapping);
            s_reloadFlagsMapping = nullptr;
        }
        if (s_reloadScopeMapping)
        {
            CloseHandle(s_reloadScopeMapping);
            s_reloadScopeMapping = nullptr;
        }
        if (s_reloadGenMapping)
        {
            CloseHandle(s_reloadGenMapping);
            s_reloadGenMapping = nullptr;
        }
        Shared::TrayXamlPolicyMap::CloseHost(&s_trayXamlPolicyMapping);
    }

    static void SignalReloadRender(DWORD flags) noexcept
    {
        // AppContainer UICompat: актуальный Tray\Xaml snapshot до сигнала reload.
        (void)Shared::TrayXamlPolicyMap::Publish(&s_trayXamlPolicyMapping);

        HANDLE hSem = OpenSemaphoreW(SEMAPHORE_MODIFY_STATE | SYNCHRONIZE, FALSE, kReloadRenderSemaphoreName);
        HANDLE hMap = OpenFileMappingW(FILE_MAP_WRITE, FALSE, kReloadGenerationMappingName);
        HANDLE hFlagsMap = OpenFileMappingW(FILE_MAP_WRITE, FALSE, kReloadRenderFlagsMappingName);
        if (hSem && hMap)
        {
            while (WaitForSingleObject(hSem, 0) == WAIT_OBJECT_0)
            {
            }
            if (hFlagsMap)
            {
                void* pFlagsView = MapViewOfFile(hFlagsMap, FILE_MAP_WRITE, 0, 0, sizeof(DWORD));
                if (pFlagsView)
                {
                    *static_cast<DWORD*>(pFlagsView) = flags;
                    UnmapViewOfFile(pFlagsView);
                }
            }
            void* pView = MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, sizeof(DWORD));
            if (pView)
            {
                InterlockedIncrement(reinterpret_cast<LONG*>(pView));
                UnmapViewOfFile(pView);
            }
            constexpr LONG kGuiThreadReleaseBudget = 512;
            (void)ReleaseSemaphore(hSem, kGuiThreadReleaseBudget, nullptr);
        }
        if (hFlagsMap)
            CloseHandle(hFlagsMap);
        if (hSem)
            CloseHandle(hSem);
        if (hMap)
            CloseHandle(hMap);
    }

    static void WriteHostPid(DWORD pid) noexcept
    {
        // H52: AppContainer SEH не читал NULL DACL → ReadHostPid=0 (лог cw_defer reason:1 PidMissing).
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;

        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;WD)(A;;GA;;;AC)",
                SDDL_REVISION_1,
                &sd,
                nullptr)
            || !sd)
        {
            sd = reinterpret_cast<PSECURITY_DESCRIPTOR>(
                LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH));
            if (sd)
            {
                InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION);
                SetSecurityDescriptorDacl(sd, TRUE, nullptr, FALSE);
            }
        }
        sa.lpSecurityDescriptor = sd;

        HANDLE hMap = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            sd ? &sa : nullptr,
            PAGE_READWRITE,
            0, sizeof(DWORD),
            kHostPidMappingName);

        // CreateFileMapping на существующий объект игнорирует новый SD — выставим ACL явно.
        if (hMap && sd)
        {
            PACL pDacl = nullptr;
            BOOL daclPresent = FALSE, daclDefaulted = FALSE;
            if (GetSecurityDescriptorDacl(sd, &daclPresent, &pDacl, &daclDefaulted) && daclPresent)
            {
                (void)SetNamedSecurityInfoW(
                    const_cast<LPWSTR>(kHostPidMappingName),
                    SE_KERNEL_OBJECT,
                    DACL_SECURITY_INFORMATION,
                    nullptr,
                    nullptr,
                    pDacl,
                    nullptr);
            }
        }

        if (sd) LocalFree(sd);

        if (!hMap)
            return;

        void* pView = MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, sizeof(DWORD));
        if (pView)
        {
            *static_cast<DWORD*>(pView) = pid;
            UnmapViewOfFile(pView);
        }

        // Намеренно НЕ закрываем hMap — именованный объект живёт пока жив хотя бы
        // один хэндл. Закроем в ClearHostPid при завершении хоста.
        s_hostPidMapping = hMap;
    }

    // Имя exe (нижний регистр, только имя файла) для фильтра ProcessBlockList при стартовом пинге.
    [[nodiscard]] static std::wstring QueryExeBaseNameLower(DWORD pid) noexcept
    {
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProc)
            return {};

        wchar_t path[MAX_PATH]{};
        DWORD sz = MAX_PATH;
        if (!QueryFullProcessImageNameW(hProc, 0, path, &sz))
        {
            CloseHandle(hProc);
            return {};
        }
        CloseHandle(hProc);

        std::wstring full(path);
        const size_t slash = full.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            full.erase(0, slash + 1);
        if (!full.empty())
            CharLowerBuffW(full.data(), static_cast<DWORD>(full.size()));
        return full;
    }

    // Стартовый пинг: у elevated-хоста UIPI часто блокирует SendMessageTimeout(WM_GETOBJECT) в medium-IL
    // приложениях — инжекция не происходит. PostMessage(WM_NULL) иногда проходит; надёжнее сочетать с
    // глобальным хуком EVENT_SYSTEM_FOREGROUND (см. Main /start) — колбэк приходит уже в целевом процессе.
    static void WinEventStartupPingTopLevelWindows() noexcept
    {
        struct PingContext
        {
            unsigned stimulated = 0;
            unsigned maxStimulate = 16384;
            DWORD selfPid = 0;
        } ctx;
        ctx.selfPid = GetCurrentProcessId();

        ProcessBlockList::Initialize();

        constexpr UINT kGetObjectTimeoutMs = 40;
        constexpr UINT kSmto = SMTO_ABORTIFHUNG | SMTO_NOTIMEOUTIFNOTHUNG;

        (void)EnumWindows(
            [](HWND hwnd, LPARAM lp) -> BOOL
            {
                auto* p = reinterpret_cast<PingContext*>(lp);
                if (!p || p->stimulated >= p->maxStimulate)
                    return FALSE;
                if (!IsWindow(hwnd))
                    return TRUE;
                if (!IsWindowVisible(hwnd))
                    return TRUE;
                if (IsIconic(hwnd))
                    return TRUE;

                DWORD pid = 0;
                (void)GetWindowThreadProcessId(hwnd, &pid);
                if (!pid || pid == p->selfPid)
                    return TRUE;

                const std::wstring exe = QueryExeBaseNameLower(pid);
                if (!exe.empty() && ProcessBlockList::IsBlocked(exe))
                    return TRUE;

                (void)PostMessageW(hwnd, WM_NULL, 0, 0);

                DWORD_PTR answer = 0;
                (void)SendMessageTimeoutW(
                    hwnd,
                    WM_GETOBJECT,
                    0,
                    static_cast<LPARAM>(OBJID_CLIENT),
                    kSmto,
                    kGetObjectTimeoutMs,
                    &answer);
                ++p->stimulated;
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));

#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER)
        DebugLog(DBG_IMPORTANT,
            L"[GlassCompat][Host] WinEvent startup ping: PostMessage(WM_NULL)+WM_GETOBJECT per top-level count=%u (cap=%u)",
            ctx.stimulated,
            ctx.maxStimulate);
#endif
    }

    // ── R7 (H97): публикация гейта загрузки ─────────────────────────────────
    //
    // Файл читает DllMain каждого процесса (GlassCompat/DllMain.cpp, GateAllowsAttach).
    // Смысл: если процесс «недоверенный», наш DLL не должна загружаться в него вовсе.
    // Иначе user32 держит в нём указатель на GlassCompat_WinEventProc, снять ссылку
    // снаружи нельзя, и после принудительного FreeLibrary процесс падает в чужом стеке
    // (дамп Яндекс Музыки 35444: AV 0xC0000005 внутри выгруженного GlassCompat).
    //
    // Режимы (переменная окружения ХОСТА GLASS_ATTACH_GATE):
    //   (нет) / deny — не грузимся в процессы из BlockList + AttachList=Forbidden
    //   allow        — грузимся ТОЛЬКО в перечисленные (строгий режим)
    //   0 / off      — гейт выключен (файл удаляется)
    // Дополнительные исключения без правки реестра: GLASS_ATTACH_GATE_EXTRA="a.exe;b.exe".

    std::wstring GateLowerCopy(const std::wstring& in)
    {
        std::wstring out = in;
        if (!out.empty())
            CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
        return out;
    }

    void GateAddName(std::wstring& body, const wchar_t* name)
    {
        if (!name || !*name)
            return;
        std::wstring lower = GateLowerCopy(name);
        body += L"name=";
        body += lower;
        body += L'\n';
    }

    // Имена из реестрового BlockList (HKCU\...\GlassPlatform\BlockList, DWORD != 0).
    void GateCollectRegistryBlockList(std::vector<std::wstring>& out)
    {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(
                HKEY_CURRENT_USER,
                L"Software\\StanislavKa\\GlassPlatform\\BlockList",
                0, KEY_READ, &hKey) != ERROR_SUCCESS)
            return;

        wchar_t name[256]{};
        DWORD index = 0;
        for (;; ++index)
        {
            DWORD nameSize = _countof(name);
            DWORD type = 0;
            DWORD value = 0;
            DWORD valueSize = sizeof(value);
            const LONG rc = RegEnumValueW(
                hKey, index, name, &nameSize, nullptr, &type,
                reinterpret_cast<BYTE*>(&value), &valueSize);
            if (rc == ERROR_NO_MORE_ITEMS)
                break;
            if (rc == ERROR_SUCCESS && type == REG_DWORD && value != 0 && nameSize > 0)
                out.emplace_back(name, nameSize);
        }
        RegCloseKey(hKey);
    }

    void WriteAttachGateFile() noexcept
    {
        wchar_t modeEnv[16]{};
        const DWORD modeLen =
            GetEnvironmentVariableW(L"GLASS_ATTACH_GATE", modeEnv, _countof(modeEnv));
        const bool gateOff =
            modeLen > 0 && (modeEnv[0] == L'0' || _wcsicmp(modeEnv, L"off") == 0);
        const bool allowMode = modeLen > 0 && _wcsicmp(modeEnv, L"allow") == 0;

        wchar_t localAppData[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, _countof(localAppData)) == 0)
        {
            DebugLog(DBG_WARNING, L"[GlassCompat][Host] attach-gate: no LOCALAPPDATA");
            return;
        }

        wchar_t dir[MAX_PATH * 2]{};
        if (swprintf_s(dir, L"%s\\GlassPlatform", localAppData) <= 0)
            return;
        (void)CreateDirectoryW(dir, nullptr);

        wchar_t path[MAX_PATH * 2]{};
        if (swprintf_s(path, L"%s\\attach-gate.txt", dir) <= 0)
            return;

        if (gateOff)
        {
            (void)DeleteFileW(path);
            GlassUlTrace::Logf(
                "H97", "EntryRundll.cpp:WriteAttachGateFile", "attach_gate_written",
                "\"mode\":\"off\",\"names\":0,\"removed\":1");
            return;
        }

        std::wstring body = L"v1\n";
        body += allowMode ? L"mode=allow\n" : L"mode=deny\n";
        unsigned count = 0;

        // Хост и собственные инструменты грузят DLL всегда — иначе платформа
        // не сможет ни стартовать, ни показать состояние процессов.
        const wchar_t* const kAlwaysTrusted[] = {
            L"rundll32.exe",
            L"GlassPlatform.exe",
            L"GlassSettings.exe",
            L"LogViewer.exe",
            L"explorer.exe",
        };
        const wchar_t* const kUiCompatHosts[] = {
            L"shellexperiencehost.exe",
            L"shellhost.exe",
            L"startmenuexperiencehost.exe",
            L"searchhost.exe",
            L"searchapp.exe",
        };

        std::vector<std::wstring> names;
        if (allowMode)
        {
            std::vector<std::pair<std::wstring, DWORD>> entries;
            AttachList::GetEntries(entries);
            for (const auto& [name, raw] : entries)
            {
                if (raw >= 1)
                    names.push_back(name);
            }
            for (const wchar_t* n : kAlwaysTrusted)
                names.emplace_back(n);
            for (const wchar_t* n : kUiCompatHosts)
                names.emplace_back(n);
        }
        else
        {
            for (std::size_t i = 0; i < ProcessBlockList::BuiltInCount(); ++i)
            {
                if (const wchar_t* n = ProcessBlockList::BuiltInAt(i))
                    names.emplace_back(n);
            }
            GateCollectRegistryBlockList(names);

            std::vector<std::pair<std::wstring, DWORD>> entries;
            AttachList::GetEntries(entries);
            for (const auto& [name, raw] : entries)
            {
                if (raw == 0) // Forbidden
                    names.push_back(name);
            }

            wchar_t extra[1024]{};
            if (GetEnvironmentVariableW(
                    L"GLASS_ATTACH_GATE_EXTRA", extra, _countof(extra)) > 0)
            {
                std::wstring rest(extra);
                size_t pos = 0;
                while (pos <= rest.size())
                {
                    const size_t sep = rest.find(L';', pos);
                    std::wstring one = rest.substr(
                        pos, sep == std::wstring::npos ? std::wstring::npos : sep - pos);
                    if (!one.empty())
                        names.push_back(one);
                    if (sep == std::wstring::npos)
                        break;
                    pos = sep + 1;
                }
            }
        }

        for (const std::wstring& name : names)
        {
            if (name.empty())
                continue;
            // В deny-режиме исключаем свои процессы даже если они попали в BlockList.
            if (!allowMode)
            {
                bool exempt = false;
                for (const wchar_t* trusted : kAlwaysTrusted)
                {
                    if (_wcsicmp(name.c_str(), trusted) == 0)
                    {
                        exempt = true;
                        break;
                    }
                }
                if (exempt)
                    continue;
            }
            GateAddName(body, name.c_str());
            ++count;
        }

        HANDLE hFile = CreateFileW(
            path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            DebugLog(DBG_WARNING,
                L"[GlassCompat][Host] attach-gate: create failed GLE=%u", GetLastError());
            return;
        }
        DWORD written = 0;
        const DWORD bytes = static_cast<DWORD>(body.size() * sizeof(wchar_t));
        const BOOL ok = WriteFile(hFile, body.c_str(), bytes, &written, nullptr);
        CloseHandle(hFile);

        DebugLog(DBG_IMPORTANT,
            L"[GlassCompat][Host] attach-gate: mode=%s names=%u path=%s ok=%d",
            allowMode ? L"allow" : L"deny", count, path, ok ? 1 : 0);
        GlassUlTrace::Logf(
            "H97", "EntryRundll.cpp:WriteAttachGateFile", "attach_gate_written",
            "\"mode\":\"%s\",\"names\":%u,\"ok\":%d",
            allowMode ? "allow" : "deny",
            count,
            ok ? 1 : 0);
    }

    static void ClearHostPid() noexcept
    {
        if (!s_hostPidMapping)
            return;

        void* pView = MapViewOfFile(s_hostPidMapping, FILE_MAP_WRITE, 0, 0, sizeof(DWORD));
        if (pView)
        {
            *static_cast<DWORD*>(pView) = 0;
            UnmapViewOfFile(pView);
        }
        CloseHandle(s_hostPidMapping);
        s_hostPidMapping = nullptr;
    }

    static std::wstring ToWide(const char* s) noexcept
    {
        if (!s || !*s) return {};
        int len = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
        if (len <= 0) return {};
        std::wstring w(len - 1, 0);
        MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), len);
        return w;
    }

    [[nodiscard]] static DWORD ParseForceUnloadPid(const std::wstring& cmd) noexcept
    {
        static const wchar_t* const kTags[] = { L"/forceUnload", L"-forceUnload" };
        for (const wchar_t* tag : kTags)
        {
            const wchar_t* p = wcsstr(cmd.c_str(), tag);
            if (!p)
                continue;
            p += wcslen(tag);
            while (*p == L' ' || *p == L'\t')
                ++p;
            if (*p == L'\0')
                return 0;
            return static_cast<DWORD>(wcstoul(p, nullptr, 10));
        }
        return 0;
    }

    [[nodiscard]] static bool ThemeMessageSuggestsDwmPolicyReload(UINT msg, LPARAM lParam) noexcept
    {
        if (msg == WM_THEMECHANGED)
            return true;
        if (msg != WM_SETTINGCHANGE)
            return false;
        const wchar_t* s = reinterpret_cast<const wchar_t*>(lParam);
        if (!s)
            return true;
        return _wcsicmp(s, L"ImmersiveColorSet") == 0;
    }
    [[nodiscard]] static bool ColorMessageSuggestsDwmPolicyReload(
        UINT msg,
        WPARAM wParam,
        LPARAM lParam) noexcept
    {
        if (msg == WM_DWMCOLORIZATIONCOLORCHANGED)
        {
            return !Shared::DwmGlassOpacity::IsGeneratedDwmColorizationWave(
                wParam,
                lParam);
        }
        return ThemeMessageSuggestsDwmPolicyReload(msg, lParam);
    }
} // anonymous namespace

// ============================================================
// Экспорт Main для Rundll32:
//   Rundll32.exe "path\GlassCompat.dll",Main /start
//   Rundll32.exe "path\GlassCompat.dll",Main /stop
//   Rundll32.exe "path\GlassCompat.dll",Main /reloadRender
//   Rundll32.exe "path\GlassCompat.dll",Main /shellStartPlatform
//   Rundll32.exe "path\GlassCompat.dll",Main /forceUnload <pid>
// ============================================================
extern "C" __declspec(dllexport)
void CALLBACK Main(HWND, HINSTANCE hInstance, LPSTR lpszCmdLine, int)
{
    std::wstring cmd = ToWide(lpszCmdLine);

    const bool isStop = (cmd.find(L"/stop")  != std::wstring::npos ||
                         cmd.find(L"-stop")  != std::wstring::npos);

    if (isStop)
    {
        StopLogClientForShortLivedMain();
        HANDLE hEvt = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
        if (hEvt)
        {
            SetEvent(hEvt);
            CloseHandle(hEvt);
        }
        return;
    }

    const bool isReloadTaskbarSwca =
        cmd.find(L"/reloadRenderTaskbarSwca") != std::wstring::npos ||
        cmd.find(L"-reloadRenderTaskbarSwca") != std::wstring::npos;
    const bool isReloadTaskbarXaml =
        cmd.find(L"/reloadRenderTaskbarXaml") != std::wstring::npos ||
        cmd.find(L"-reloadRenderTaskbarXaml") != std::wstring::npos;
    const bool isReloadTaskbarBoth =
        cmd.find(L"/reloadRenderTaskbarBoth") != std::wstring::npos ||
        cmd.find(L"-reloadRenderTaskbarBoth") != std::wstring::npos;
    const bool isReloadRenderCommit =
        cmd.find(L"/reloadRenderCommit") != std::wstring::npos ||
        cmd.find(L"-reloadRenderCommit") != std::wstring::npos;
    const bool isReloadRender =
        cmd.find(L"/reloadRender") != std::wstring::npos ||
        cmd.find(L"-reloadRender") != std::wstring::npos;

    if (isReloadTaskbarSwca)
    {
        SignalReloadRender(kReloadRenderFlagTaskbarSwca);
        StopLogClientForShortLivedMain();
        return;
    }
    if (isReloadTaskbarXaml)
    {
        SignalReloadRender(kReloadRenderFlagTaskbarXaml);
        StopLogClientForShortLivedMain();
        return;
    }
    if (isReloadTaskbarBoth)
    {
        SignalReloadRender(kReloadRenderFlagTaskbarBoth);
        StopLogClientForShortLivedMain();
        return;
    }
    if (isReloadRenderCommit)
    {
        SignalReloadRender(kReloadRenderFlagGlassSettingsCommit);
        StopLogClientForShortLivedMain();
        return;
    }
    if (isReloadRender)
    {
        SignalReloadRender(kReloadRenderFlagAll);
        StopLogClientForShortLivedMain();
        return;
    }

    const bool isShellStartPlatform = (cmd.find(L"/shellStartPlatform") != std::wstring::npos ||
                                       cmd.find(L"-shellStartPlatform") != std::wstring::npos);
    if (isShellStartPlatform)
    {
        // Один повышенный вызов из GlassShellExt: при необходимости /ENABLE и запуск задачи/хоста.
        if (GlassShared::GlassPlatformTask::IsGlassPlatformRunning())
        {
            StopLogClientForShortLivedMain();
            return;
        }

        const auto ui = GlassShared::GlassPlatformTask::QueryGlassAutorunUiState();
        if (ui == GlassShared::GlassPlatformTask::AutorunUiState::NoTask)
        {
            StopLogClientForShortLivedMain();
            return;
        }

        if (ui == GlassShared::GlassPlatformTask::AutorunUiState::Disabled)
            (void)GlassShared::GlassPlatformTask::SetGlassAutorunTaskSchedulerEnabled(true);

        GlassShared::GlassPlatformTask::StartPlatform();
        StopLogClientForShortLivedMain();
        return;
    }

    const bool isForceUnload =
        cmd.find(L"/forceUnload") != std::wstring::npos ||
        cmd.find(L"-forceUnload") != std::wstring::npos;
    if (isForceUnload)
    {
        const DWORD targetPid = ParseForceUnloadPid(cmd);
        if (targetPid != 0)
        {
            (void)Shared::ForceUnloadGlassModules::ForceUnload(targetPid);
        }
        StopLogClientForShortLivedMain();
        return;
    }

    // ---- /start ----

    // Проверяем одиночный экземпляр
    HANDLE hMutex = CreateMutexW(nullptr, FALSE, kInstanceMutexName);
    if (!hMutex) return;

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(hMutex);
        return; // уже запущен
    }

    // Публикуем PID хоста — WinEventProc в других процессах его прочитает
    // и поймёт, что в хосте инициализация не нужна
    WriteHostPid(GetCurrentProcessId());

    // Хост не проходит GlassCompat::Initialize (WinEventProc → HostProcess).
    // CrashGuard здесь, а не в DllMain: BlockList-процессы не получают UEF/VEH.
    GlassCompat::InstallProcessCrashGuard();

    Shared::OpenGlassDwmPolicy::ApplyDwmPolicyOnVisualStyleChange();
#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER)
    DebugLog(DBG_IMPORTANT, L"[GlassCompat][Host] DWM policy on startup");
#endif

    // Stop/HostShutdown: WD+AC (как TrayXamlPolicy). NULL DACL не даёт AppContainer
    // OpenEvent → SEH/SMEH не видят Stop → нет ExecuteShutdown/Restore/FreeSelf (H48).
    SECURITY_ATTRIBUTES saEvt{};
    saEvt.nLength = sizeof(saEvt);
    saEvt.bInheritHandle = FALSE;

    PSECURITY_DESCRIPTOR sdEvt = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;WD)(A;;GA;;;AC)",
            SDDL_REVISION_1,
            &sdEvt,
            nullptr)
        || !sdEvt)
    {
        sdEvt = reinterpret_cast<PSECURITY_DESCRIPTOR>(
            LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH));
        if (sdEvt)
        {
            InitializeSecurityDescriptor(sdEvt, SECURITY_DESCRIPTOR_REVISION);
            SetSecurityDescriptorDacl(sdEvt, TRUE, nullptr, FALSE);
        }
    }
    saEvt.lpSecurityDescriptor = sdEvt;

    // SD живёт до конца создания именованных объектов; LocalFree раньше — UAF →
    // CreateEventW(HostShutdown)/mapping часто падают, хост сразу выходит («не стартует»).
    SECURITY_ATTRIBUTES* saPtr = sdEvt ? &saEvt : nullptr;

    HANDLE hStopEvent = CreateEventW(saPtr, TRUE, FALSE, kStopEventName);
    if (!hStopEvent)
    {
        if (sdEvt) LocalFree(sdEvt);
        ClearHostPid();
        CloseHandle(hMutex);
        return;
    }

    s_hostShutdownEvent = CreateEventW(
        saPtr,
        TRUE,
        FALSE,
        GlassShared::GlassPlatformTask::kHostShutdownEventName);
    if (!s_hostShutdownEvent)
    {
        CloseHandle(hStopEvent);
        if (sdEvt) LocalFree(sdEvt);
        ClearHostPid();
        CloseHandle(hMutex);
        return;
    }

    // Dead-man switch. Хост владеет мьютексом всё время работы; если процесс убит
    // (TerminateProcess от Планировщика, краш), ядро отдаёт ожидающим WAIT_ABANDONED.
    // В отличие от OpenProcess/PID-поллинга это не даёт ложных срабатываний в
    // AppContainer (H51/H52) и не требует опроса.
    HANDLE hHostAlive = CreateMutexW(
        saPtr, TRUE, GlassShared::GlassPlatformTask::kHostAliveMutexName);
    if (hHostAlive && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        // Объект пережил прежнего хоста (клиенты держат handle) — забрать владение,
        // иначе он остался бы вечно сигнальным и все клиенты сразу решили бы, что мы мертвы.
        const DWORD claim = WaitForSingleObject(hHostAlive, 0);
        if (claim != WAIT_OBJECT_0 && claim != WAIT_ABANDONED)
        {
            CloseHandle(hHostAlive);
            hHostAlive = nullptr;
        }
    }
#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER)
    if (!hHostAlive)
    {
        DebugLog(DBG_WARNING,
            L"[GlassCompat][Host] HostAlive mutex недоступен, GLE=%u — детект смерти хоста только по PID",
            GetLastError());
    }
#endif

    if (!CreateReloadRenderResources(saPtr))
    {
        CloseHandle(hStopEvent);
        SignalHostShutdownComplete();
        CloseHostShutdownEvent();
        if (sdEvt) LocalFree(sdEvt);
        ClearHostPid();
        CloseHandle(hMutex);
        return;
    }

    // Snapshot Tray XAML policy для AppContainer UICompat (до первого Refresh inject).
    if (!Shared::TrayXamlPolicyMap::Publish(&s_trayXamlPolicyMapping))
    {
#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER)
        DebugLog(DBG_WARNING,
            L"[GlassCompat][Host] TrayXamlPolicy map unavailable, GLE=%u — UICompat fallback to registry",
            GetLastError());
#endif
    }

    if (!Shared::PlatformPresenceState::CreateHostPresenceResources(saPtr))
    {
        CloseReloadRenderResources();
        CloseHandle(hStopEvent);
        SignalHostShutdownComplete();
        CloseHostShutdownEvent();
        if (sdEvt) LocalFree(sdEvt);
        ClearHostPid();
        CloseHandle(hMutex);
        return;
    }

    if (!Shared::InjectProcessMap::CreateHostResources(saPtr))
    {
#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER)
        DebugLog(DBG_WARNING,
            L"[GlassCompat][Host] InjectProcessMap unavailable, GLE=%u — host continues without inject map",
            GetLastError());
#endif
    }

    if (!Shared::OpenGlassHostCommand::CreateHostCommandResources(saPtr))
    {
#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER)
        DebugLog(DBG_WARNING,
            L"[GlassCompat][Host] OpenGlassHostCommand IPC unavailable, GLE=%u — host continues without shell OG commands",
            GetLastError());
#endif
    }

    if (sdEvt)
    {
        LocalFree(sdEvt);
        sdEvt = nullptr;
        saEvt.lpSecurityDescriptor = nullptr;
    }

    Shared::RootRegistryMigration::MigrateLegacyRootBackDropType();
    (void)Shared::PlatformPresenceMonitor::Start();

    // Получаем HMODULE этого DLL (GlassCompat.dll)
    HMODULE hSelf = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&GlassCompat_WinEventProc),
        &hSelf);

    // R7 (H97): публикуем гейт загрузки ДО установки хуков — DllMain читает этот файл
    // при каждом INCONTEXT-attach в любом процессе, в том числе на стартовом пинге.
    AttachList::Initialize();
    WriteAttachGateFile();

    // Устанавливаем глобальный INCONTEXT WinEvent-хук для всех процессов.
    // Диапазон CREATE..SHOW: тултип (tooltips_class32) после скрытия получает WM_NCDESTROY
    // и снимает субкласс; при следующем показе часто нет нового EVENT_OBJECT_CREATE —
    // нужен EVENT_OBJECT_SHOW для повторного Tooltip_AttachIfNeeded (см. WinEventHandler).
    g_globalHook = SetWinEventHook(
        EVENT_OBJECT_CREATE,
        EVENT_OBJECT_SHOW,
        hSelf,
        GlassCompat_WinEventProc,
        0, 0,                     // все процессы, все потоки
        WINEVENT_INCONTEXT
    );
    // #region agent log
    // GLASS-ULTRACE
    GlassUlTrace::Logf(
        "H81",
        "EntryRundll.cpp:Main",
        "host_hook_install",
        "\"kind\":\"global\",\"handle\":\"%p\",\"ok\":%d",
        static_cast<void*>(g_globalHook),
        g_globalHook ? 1 : 0);
    // #endregion

    g_foregroundWinEventHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND,
        EVENT_SYSTEM_FOREGROUND,
        hSelf,
        GlassCompat_WinEventProc,
        0,
        0,
        WINEVENT_INCONTEXT);
    // #region agent log
    // GLASS-ULTRACE
    GlassUlTrace::Logf(
        "H81",
        "EntryRundll.cpp:Main",
        "host_hook_install",
        "\"kind\":\"foreground\",\"handle\":\"%p\",\"ok\":%d",
        static_cast<void*>(g_foregroundWinEventHook),
        g_foregroundWinEventHook ? 1 : 0);
    // #endregion

    g_uncloakWinEventHook = SetWinEventHook(
        EVENT_OBJECT_UNCLOAKED,
        EVENT_OBJECT_UNCLOAKED,
        hSelf,
        GlassCompat_WinEventProc,
        0,
        0,
        WINEVENT_INCONTEXT);
    // #region agent log
    // GLASS-ULTRACE
    GlassUlTrace::Logf(
        "H81",
        "EntryRundll.cpp:Main",
        "host_hook_install",
        "\"kind\":\"uncloak\",\"handle\":\"%p\",\"ok\":%d",
        static_cast<void*>(g_uncloakWinEventHook),
        g_uncloakWinEventHook ? 1 : 0);
    // #endregion

#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER)
    if (!g_foregroundWinEventHook)
        DebugLog(DBG_WARNING, L"[GlassCompat][Host] SetWinEventHook(EVENT_SYSTEM_FOREGROUND) failed, GLE=%u",
            GetLastError());
    if (!g_uncloakWinEventHook)
        DebugLog(DBG_WARNING, L"[GlassCompat][Host] SetWinEventHook(EVENT_OBJECT_UNCLOAKED) failed, GLE=%u",
            GetLastError());
#endif

    // Логируем полный список AttachList только в хосте — один раз при старте.
    // В инжектированных процессах этот лог не нужен (они смотрят только свою запись).
    AttachList::Initialize();
    AttachList::LogAll();

    if (g_globalHook || g_foregroundWinEventHook || g_uncloakWinEventHook)
    {
        WinEventStartupPingTopLevelWindows();
        // Broadcast WM_NULL — лёгкий пинок очередей (как при shutdown), без гарантии WinEvent.
        SendNotifyMessageW(HWND_BROADCAST, WM_NULL, 0, 0);
    }

    // AppContainer SEH/SMEH: глобальный hook (pid=0,tid=0) DLL не грузит — ACL + per-thread + LoadLibrary.
    GlassCompat::CoreWindowHostInject::GrantPeerDllsAppPackagesAccess();
    GlassCompat::CoreWindowHostInject::Refresh();

    // Окно должно быть на том же потоке, что и цикл сообщений ниже.
    s_stopFromCloseMessage = false;
    s_hostCloseSinkHwnd = CreateHostCloseSinkWindow(hSelf, hStopEvent);
#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER)
    if (!s_hostCloseSinkHwnd)
    {
        DebugLog(DBG_WARNING,
            L"[GlassCompat][Host] close-sink окно не создано, GLE=%u — End task пойдёт через TerminateProcess",
            GetLastError());
    }
#endif

    // Остановка ловится через MsgWaitForMultipleObjects по hStopEvent.
    MSG msg;
    static ULONGLONG s_themePolicyDueMs = 0;
    static ULONGLONG s_nextCoreWindowInjectMs = 0;
    while (true)
    {
        DWORD r = MsgWaitForMultipleObjects(1, &hStopEvent, FALSE, 2000, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0)
        {
            break; // stop event
        }

        // Обрабатываем сообщения
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                goto done;
            if (ColorMessageSuggestsDwmPolicyReload(msg.message, msg.wParam, msg.lParam))
            {
                s_themePolicyDueMs = GetTickCount64() + 300;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            // Do not run another injection refresh/watchdog iteration after
            // the Scheduler close request has been processed.
            if (s_stopFromCloseMessage)
                goto done;
        }

        if (s_themePolicyDueMs != 0 && GetTickCount64() >= s_themePolicyDueMs)
        {
            Shared::OpenGlassDwmPolicy::ApplyDwmPolicyOnVisualStyleChange();
            // Publish TrayXamlPolicy + TaskbarCompat Recapture + UICompat theme brush refresh
            // (не ClearValue-Recapture на общем TaskbarXaml — см. UiThemeBrushRefresh).
            SignalReloadRender(
                kReloadRenderFlagTaskbarBoth | kReloadRenderFlagUiThemeBrushRefresh);
            s_themePolicyDueMs = 0;
#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER)
            DebugLog(DBG_IMPORTANT,
                L"[GlassCompat][Host] DWM policy + TaskbarBoth+UiThemeBrushRefresh "
                L"on visual style change");
#endif
        }

        const ULONGLONG now = GetTickCount64();
        if (now >= s_nextCoreWindowInjectMs)
        {
            GlassCompat::CoreWindowHostInject::Refresh();
            s_nextCoreWindowInjectMs = now + 3000;
        }

        GlassCompat::HostPollExplorerShellWatchdog();
    }

done:
    DestroyHostCloseSinkWindow(hSelf);
#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER)
    Shared::Log::GlassLogClient::Instance().Stop();
#endif
    {
        const bool hadAnyHook =
            (g_foregroundWinEventHook != nullptr
             || g_globalHook != nullptr
             || g_uncloakWinEventHook != nullptr);
        unsigned unhooked = 0;
        if (g_foregroundWinEventHook)
        {
            const BOOL okFg = UnhookWinEvent(g_foregroundWinEventHook);
            // #region agent log
            // GLASS-ULTRACE
            GlassUlTrace::Logf(
                "H81",
                "EntryRundll.cpp:Main",
                "host_hook_unhook",
                "\"kind\":\"foreground\",\"handle\":\"%p\",\"ok\":%d",
                static_cast<void*>(g_foregroundWinEventHook),
                okFg ? 1 : 0);
            // #endregion
            g_foregroundWinEventHook = nullptr;
            ++unhooked;
        }
        if (g_globalHook)
        {
            const BOOL okGl = UnhookWinEvent(g_globalHook);
            // #region agent log
            // GLASS-ULTRACE
            GlassUlTrace::Logf(
                "H81",
                "EntryRundll.cpp:Main",
                "host_hook_unhook",
                "\"kind\":\"global\",\"handle\":\"%p\",\"ok\":%d",
                static_cast<void*>(g_globalHook),
                okGl ? 1 : 0);
            // #endregion
            g_globalHook = nullptr;
            ++unhooked;
        }
        if (g_uncloakWinEventHook)
        {
            const BOOL okUc = UnhookWinEvent(g_uncloakWinEventHook);
            // #region agent log
            // GLASS-ULTRACE
            GlassUlTrace::Logf(
                "H81",
                "EntryRundll.cpp:Main",
                "host_hook_unhook",
                "\"kind\":\"uncloak\",\"handle\":\"%p\",\"ok\":%d,\"total\":%u",
                static_cast<void*>(g_uncloakWinEventHook),
                okUc ? 1 : 0,
                unhooked + 1);
            // #endregion
            g_uncloakWinEventHook = nullptr;
            ++unhooked;
        }
        GlassCompat::CoreWindowHostInject::Shutdown(!s_stopFromCloseMessage);
        if (hadAnyHook && !s_stopFromCloseMessage)
        {
            // Для обычного сигнала платформы сохраняется прежняя обработка.
            // #region agent log
            // GLASS-ULTRACE
            GlassUlTrace::Logf(
                "H88",
                "EntryRundll.cpp:Main",
                "host_broadcast",
                "\"unhooked\":%u",
                unhooked);
            // #endregion
            (void)SendNotifyMessageW(HWND_BROADCAST, WM_NULL, 0, 0);
            Sleep(300);
        }
        // Клиенты могут самостоятельно завершиться после снятия хуков.
        // #region agent log
        // GLASS-ULTRACE
        GlassUlTrace::Logf(
            "H88",
            "EntryRundll.cpp:Main",
            "host_shutdown_signal",
            "\"unhooked\":%u,\"from_close\":%d",
            unhooked,
            s_stopFromCloseMessage ? 1 : 0);
        // #endregion
        SignalHostShutdownComplete();
        HANDLE hVerify = nullptr;
        if (s_stopFromCloseMessage)
        {
            // Планировщик попросил закрыть задачу: только локальная очистка.
            // Остатки проверяет отдельный этап мониторинга Setup/Unins.
            GlassCompat::CoreWindowHostInject::FinishTaskStop();
        }
        else
        {
            GlassCompat::CoreWindowHostInject::ForceUnloadResiduals();
            // #region agent log
            // GLASS-ULTRACE
            hVerify = GlassUlTrace::ScheduleVerify();
            // #endregion
        }
        if (hVerify)
            CloseHandle(hVerify);
    }

    Shared::PlatformPresenceMonitor::Stop();
    Shared::OpenGlassHostCommand::CloseHostCommandResources();
    Shared::InjectProcessMap::CloseHostResources();
    Shared::PlatformPresenceState::CloseHostPresenceResources();
    ClearHostPid();
    CloseReloadRenderResources();
    CloseHandle(hStopEvent);
    // H71: не CloseHostShutdownEvent здесь. Named event должен жить, пока жив процесс хоста:
    // path=1 OpenEvent после COMPLETE (H70) иначе даёт WAIT_FAILED → FreeSelf при живом OS-pin.
    // Handle закроется при выходе rundll32. Early-abort пути по-прежнему Close явно.
    //
    // hHostAlive тоже не закрываем и не освобождаем: мьютекс должен стать abandoned
    // ровно в момент смерти потока хоста — это и есть сигнал для клиентов.
    CloseHandle(hMutex);
}
