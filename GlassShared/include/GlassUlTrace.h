#pragma once

// GLASS-ULTRACE: наблюдение выгрузки (R1). Не меняет порядок фаз / таймауты / path=1 vs BlockList.
// Пишет NDJSON в корень репозитория. Снимать вместе с вызовами по метке GLASS-ULTRACE.

#include <windows.h>
#include <tlhelp32.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace GlassUlTrace
{
    inline constexpr char kSessionId[] = "8e6259";
    inline constexpr char kRunId[] = "r2";
    // Основной путь (машина разработчика). Если недоступен (ACL/AppContainer/другой ПК) —
    // %LOCALAPPDATA%\GlassPlatform\diag, затем %TEMP%. Фактический путь пишется в trace_init.
    inline constexpr wchar_t kLogPath[] =
        L"d:\\VS_PROJECT\\DWM\\GlassProject\\debug-8e6259.log";

    // rc: 0 = модуля нет в списке загрузчика, kRcUnknown = прочитать не удалось.
    inline constexpr unsigned kRcUnknown = 0xFFFFFFFFu;

    inline unsigned long long NowMs() noexcept
    {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER u{};
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return (u.QuadPart / 10000ULL) - 11644473600000ULL;
    }

    inline void WideToJson(char* dst, size_t cch, const wchar_t* src) noexcept
    {
        if (!dst || cch == 0)
            return;
        dst[0] = 0;
        if (!src)
            return;
        size_t j = 0;
        for (size_t i = 0; src[i] && j + 8 < cch; ++i)
        {
            const unsigned c = static_cast<unsigned>(src[i]);
            if (c == L'\\' || c == L'"')
            {
                dst[j++] = '\\';
                dst[j++] = (c == L'\\') ? '\\' : '"';
            }
            else if (c < 0x20 || c > 0x7E)
            {
                // Валидный JSON для не-ASCII: RU-имена процессов больше не "?????? ??????".
                const int r = _snprintf_s(dst + j, cch - j, _TRUNCATE, "\\u%04X", c);
                if (r <= 0)
                    break;
                j += static_cast<size_t>(r);
            }
            else
            {
                dst[j++] = static_cast<char>(c);
            }
        }
        dst[j] = 0;
    }

    inline void CurrentExeLeaf(char* dst, size_t cch) noexcept
    {
        wchar_t wexe[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, wexe, MAX_PATH))
        {
            if (dst && cch)
                dst[0] = 0;
            return;
        }
        const wchar_t* leaf = wexe;
        for (const wchar_t* p = wexe; *p; ++p)
        {
            if (*p == L'\\' || *p == L'/')
                leaf = p + 1;
        }
        WideToJson(dst, cch, leaf);
    }

    struct LdrDdagNodeUl
    {
        LIST_ENTRY Modules;
        PVOID ServiceTagList;
        ULONG LoadCount;
        ULONG LoadWhileUnloadingCount;
    };

    struct LdrEntryUl
    {
        LIST_ENTRY InLoadOrderLinks;
        LIST_ENTRY InMemoryOrderLinks;
        LIST_ENTRY InInitializationOrderLinks;
        PVOID DllBase;
        PVOID EntryPoint;
        ULONG SizeOfImage;
        struct { USHORT Length; USHORT MaximumLength; PWSTR Buffer; } FullDllName;
        struct { USHORT Length; USHORT MaximumLength; PWSTR Buffer; } BaseDllName;
        ULONG Flags;
        USHORT ObsoleteLoadCount;
        USHORT TlsIndex;
        LIST_ENTRY HashLinks;
        ULONG TimeDateStamp;
        PVOID EntryPointActivationContext;
        PVOID Lock;
        LdrDdagNodeUl* DdagNode;
    };

    struct PebLdrUl
    {
        ULONG Length;
        BOOLEAN Initialized;
        PVOID SsHandle;
        LIST_ENTRY InLoadOrderModuleList;
    };

    struct PebUl
    {
        BYTE Reserved1[2];
        BYTE BeingDebugged;
        BYTE Reserved2[1];
        PVOID Reserved3[2];
        PebLdrUl* Ldr;
    };

    // Фактический loader refcount (DdagNode->LoadCount / ObsoleteLoadCount).
    // 0 = модуля нет в списке загрузчика; kRcUnknown = прочитать не удалось
    // (важно различать: «модуль выгружен» и «rc не измерен»).
    inline unsigned QueryLoadCount(HMODULE hMod) noexcept
    {
        if (!hMod)
            return 0;
#if defined(_WIN64)
        auto* peb = reinterpret_cast<PebUl*>(__readgsqword(0x60));
#else
        auto* peb = reinterpret_cast<PebUl*>(__readfsdword(0x30));
#endif
        if (!peb || !peb->Ldr)
            return kRcUnknown;
        LIST_ENTRY* const head = &peb->Ldr->InLoadOrderModuleList;
        for (LIST_ENTRY* e = head->Flink; e && e != head; e = e->Flink)
        {
            auto* entry = CONTAINING_RECORD(e, LdrEntryUl, InLoadOrderLinks);
            if (entry->DllBase != hMod)
                continue;
            if (entry->DdagNode)
                return entry->DdagNode->LoadCount;
            return entry->ObsoleteLoadCount;
        }
        return 0;
    }

    inline unsigned QueryLoadCountByName(const wchar_t* name) noexcept
    {
        return QueryLoadCount(GetModuleHandleW(name));
    }

    inline bool ModulePresentGmh(const wchar_t* name) noexcept
    {
        return name && GetModuleHandleW(name) != nullptr;
    }

    inline bool ModulePresentToolhelp(const wchar_t* name, DWORD pid = 0) noexcept
    {
        if (!name || !*name)
            return false;
        if (pid == 0)
            pid = GetCurrentProcessId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE)
            return false;
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        bool found = false;
        if (Module32FirstW(snap, &me))
        {
            do
            {
                if (_wcsicmp(me.szModule, name) == 0)
                {
                    found = true;
                    break;
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
        return found;
    }

    inline constexpr const wchar_t* kGlassModules[] = {
        L"GlassCompat.dll",
        L"GlassCore.dll",
        L"WndCompat.dll",
        L"ExplorerCompat.dll",
        L"TaskbarCompat.dll",
        L"UICompat.dll",
    };

    // bit0=compat,1=core,2=wnd,3=explorer,4=taskbar,5=ui
    inline unsigned RemoteModuleMask(DWORD pid) noexcept
    {
        unsigned mask = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE)
            return 0;
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me))
        {
            do
            {
                for (unsigned i = 0; i < 6; ++i)
                {
                    if (_wcsicmp(me.szModule, kGlassModules[i]) == 0)
                        mask |= (1u << i);
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
        return mask;
    }

    inline void FormatModuleMask(unsigned mask, char* dst, size_t cch) noexcept
    {
        if (!dst || !cch)
            return;
        dst[0] = 0;
        const char* names[] = { "compat", "core", "wnd", "explorer", "taskbar", "ui" };
        bool first = true;
        for (unsigned i = 0; i < 6; ++i)
        {
            if ((mask & (1u << i)) == 0)
                continue;
            if (!first)
                strncat_s(dst, cch, ",", _TRUNCATE);
            strncat_s(dst, cch, names[i], _TRUNCATE);
            first = false;
        }
        if (first)
            strncat_s(dst, cch, "none", _TRUNCATE);
    }

    // Разрешение пути: env GLASS_ULTRACE_LOG → kLogPath → %LOCALAPPDATA%\GlassPlatform\diag →
    // %TEMP%\GlassPlatform-diag. Возвращает nullptr, если ни один каталог не открылся (тогда
    // остаётся только OutputDebugStringW).
    inline const wchar_t* LogPath(bool* outFromFallback) noexcept
    {
        static wchar_t s_path[MAX_PATH * 2]{};
        static LONG s_state = 0; // 0 = не разрешён, 1 = готов
        if (InterlockedCompareExchange(&s_state, 1, 0) == 0)
        {
            wchar_t candidate[MAX_PATH * 2]{};
            if (GetEnvironmentVariableW(L"GLASS_ULTRACE_LOG", candidate, _countof(candidate)) > 0)
            {
                wcscpy_s(s_path, candidate);
                if (outFromFallback)
                    *outFromFallback = true;
            }
            else
            {
                wcscpy_s(s_path, kLogPath);
                // Проверяем каталог основного пути.
                wchar_t dir[MAX_PATH * 2]{};
                wcscpy_s(dir, s_path);
                wchar_t* slash = wcsrchr(dir, L'\\');
                const wchar_t* probe = s_path;
                if (slash)
                {
                    *slash = 0;
                    probe = dir;
                }
                if (GetFileAttributesW(probe) == INVALID_FILE_ATTRIBUTES)
                {
                    wchar_t base[MAX_PATH]{};
                    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, _countof(base));
                    if (n == 0 || n >= _countof(base))
                        (void)GetTempPathW(static_cast<DWORD>(_countof(base)), base);
                    swprintf_s(s_path, L"%s\\GlassPlatform\\diag\\unload-trace.log", base);
                    CreateDirectoryW((std::wstring(base) + L"\\GlassPlatform").c_str(), nullptr);
                    CreateDirectoryW((std::wstring(base) + L"\\GlassPlatform\\diag").c_str(), nullptr);
                    if (outFromFallback)
                        *outFromFallback = true;
                }
            }
        }
        return s_path;
    }

    inline void Log(const char* hyp, const char* location, const char* message, const char* dataJson) noexcept
    {
        char exe[80]{};
        CurrentExeLeaf(exe, sizeof(exe));
        const unsigned long long ts = NowMs();
        const DWORD pid = GetCurrentProcessId();
        const DWORD tid = GetCurrentThreadId();
        char line[2200]{};
        _snprintf_s(
            line,
            _TRUNCATE,
            "{\"sessionId\":\"%s\",\"id\":\"log_%llu_%lu_%lu\",\"timestamp\":%llu,"
            "\"location\":\"%s\",\"message\":\"%s\",\"hypothesisId\":\"%s\",\"runId\":\"%s\","
            "\"data\":{\"pid\":%lu,\"tid\":%lu,\"exe\":\"%s\",%s}}\n",
            kSessionId,
            ts,
            static_cast<unsigned long>(pid),
            static_cast<unsigned long>(tid),
            ts,
            location ? location : "",
            message ? message : "",
            hyp ? hyp : "",
            kRunId,
            static_cast<unsigned long>(pid),
            static_cast<unsigned long>(tid),
            exe,
            dataJson && dataJson[0] ? dataJson : "\"_\":0");

        HANDLE h = CreateFileW(
            LogPath(nullptr),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            ::OutputDebugStringA(line);
            return;
        }
        DWORD written = 0;
        (void)WriteFile(h, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
        CloseHandle(h);
    }

    inline void Logf(
        const char* hyp,
        const char* location,
        const char* message,
        const char* fmt,
        ...) noexcept
    {
        char data[1400]{};
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf_s(data, _TRUNCATE, fmt, ap);
        va_end(ap);
        Log(hyp, location, message, data);
    }

    // Одна строка на процесс: фактический путь лога (видно, сработал ли fallback).
    inline void LogTraceInitOnce() noexcept
    {
        static LONG s_done = 0;
        if (InterlockedCompareExchange(&s_done, 1, 0) != 0)
            return;
        const wchar_t* path = LogPath(nullptr);
        char narrow[MAX_PATH * 2]{};
        WideToJson(narrow, sizeof(narrow), path);
        Logf("H82", "GlassUlTrace.h:LogTraceInit", "trace_init",
            "\"path\":\"%s\",\"run\":\"%s\"", narrow, kRunId);
    }

    // R8: `tag` отделяет замеры внутри хоста ("" = прежнее поведение) от пост-стопных
    // проверок из отдельного процесса после выхода хоста ("post2"/"post8"). Только
    // пост-стопные цифры годятся для приёмки S8: OS снимает per-process ссылку на DLL
    // ровно в момент завершения процесса-владельца хуков (наблюдение прогона 8:
    // freeself 12:52:52…56 → dll_detach 12:53:19.3–19.6, т.е. через 0.3–0.6 с после
    // выхода rundll32).
    inline void LogVerify(unsigned delaySec, const char* tag = nullptr) noexcept
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
        {
            Logf("H82", "GlassUlTrace.h:LogVerify", "verify",
                "\"delay_s\":%u,\"tag\":\"%s\",\"scan\":\"failed\",\"gle\":%lu",
                delaySec, tag ? tag : "", static_cast<unsigned long>(GetLastError()));
            return;
        }
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        unsigned hits = 0;
        // R9: свои инструменты считаем отдельно. GlassSettings держит локальный GlassCore
        // для превью и выгружает его только при собственном закрытии — если окно настроек
        // открыто после Stop, `core` там остаётся намеренно и к остаткам не относится.
        unsigned own = 0;
        if (Process32FirstW(snap, &pe))
        {
            do
            {
                // pid 0 = System Idle; CreateToolhelp32Snapshot(0,...) снимает ТЕКУЩИЙ процесс.
                if (pe.th32ProcessID == 0)
                    continue;
                // Себя (проверяющий процесс) в остатки не считаем: в нём наша DLL по определению.
                if (pe.th32ProcessID == GetCurrentProcessId())
                    continue;
                const unsigned mask = RemoteModuleMask(pe.th32ProcessID);
                if (mask == 0)
                    continue;
                const bool ownTool =
                    _wcsicmp(pe.szExeFile, L"GlassSettings.exe") == 0 ||
                    _wcsicmp(pe.szExeFile, L"LogViewer.exe") == 0;
                if (ownTool)
                    ++own;
                else
                    ++hits;
                char exe[80]{};
                WideToJson(exe, sizeof(exe), pe.szExeFile);
                char mods[80]{};
                FormatModuleMask(mask, mods, sizeof(mods));
                Logf(
                    "H82",
                    "GlassUlTrace.h:LogVerify",
                    "verify",
                    "\"delay_s\":%u,\"tag\":\"%s\",\"target_pid\":%lu,\"target_exe\":\"%s\",\"modules\":\"%s\",\"mask\":%u,\"own\":%d",
                    delaySec,
                    tag ? tag : "",
                    static_cast<unsigned long>(pe.th32ProcessID),
                    exe,
                    mods,
                    mask,
                    ownTool ? 1 : 0);
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        Logf("H82", "GlassUlTrace.h:LogVerify", "verify",
            "\"delay_s\":%u,\"tag\":\"%s\",\"hits\":%u,\"own\":%u,\"scan\":\"done\"",
            delaySec, tag ? tag : "", hits, own);
    }

    inline DWORD WINAPI VerifyThreadProc(LPVOID) noexcept
    {
        Sleep(2000);
        LogVerify(2);
        Sleep(8000);
        LogVerify(10);
        return 0;
    }

    // 0s сразу; 2s/10s на фоне. Не блокирует caller (контракт done: не двигаем).
    inline HANDLE ScheduleVerify() noexcept
    {
        LogVerify(0);
        return CreateThread(nullptr, 0, VerifyThreadProc, nullptr, 0, nullptr);
    }
}
