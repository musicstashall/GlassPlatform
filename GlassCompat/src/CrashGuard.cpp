#include "CompatPch.h"
#include "CrashGuard.h"
#include "HostPidHelper.h"

#include <DebugLog.h>
#include <DbgHelp.h>
#include <InjectProcessMap.h>
#include <cstring>
#include <cstdio>
#include <process.h>

#pragma comment(lib, "Dbghelp.lib")

#ifndef STATUS_ACCESS_VIOLATION
#define STATUS_ACCESS_VIOLATION ((DWORD)0xC0000005L)
#endif

namespace
{
    // Настройки дампов / VEH (см. CrashGuard.h). Запись — только в kCrashRegPath.
    constexpr wchar_t kBwmRegRoot[] = L"Software\\StanislavKa\\BlackWindowManager";
    constexpr wchar_t kCrashRegPath[] = L"Software\\StanislavKa\\BlackWindowManager\\Crashes";
    // Fallback чтения: прошлая сборка писала в корень BWM; ещё старее — Glass\CrashGuard.
    constexpr wchar_t kLegacyRegPath[] = L"Software\\Glass\\CrashGuard";
    constexpr DWORD kMaxCrashes = 3;
    //10 минут — окно серии падений (FILETIME 100-ns)
    constexpr ULONGLONG kCrashWindowTicks = 10ull * 60 * 10'000'000ull;
    constexpr size_t kMaxDumpRoots = 5;
    constexpr DWORD kStowedExceptionCode = 0xC000027BUL;

    LPTOP_LEVEL_EXCEPTION_FILTER g_prevExplorerFilter = nullptr;
    PVOID g_vehFastFail = nullptr;
    bool g_vehRecordFastFail = false;
    bool g_vehRecordStowed = false;
    bool g_crashGuardInstalled = false;

    [[nodiscard]] unsigned ClassifyGlassModule(HMODULE mod) noexcept
    {
        if (!mod)
            return 0;
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(mod, path, MAX_PATH))
            return 0;
        const wchar_t* base = path;
        for (const wchar_t* p = path; *p; ++p)
        {
            if (*p == L'\\' || *p == L'/')
                base = p + 1;
        }
        if (_wcsicmp(base, L"GlassCompat.dll") == 0)
            return 1;
        if (_wcsicmp(base, L"GlassCore.dll") == 0)
            return 2;
        if (_wcsicmp(base, L"WndCompat.dll") == 0)
            return 4;
        if (_wcsicmp(base, L"ExplorerCompat.dll") == 0)
            return 8;
        if (_wcsnicmp(base, L"Glass", 5) == 0 || _wcsnicmp(base, L"WndCompat", 9) == 0)
            return 16;
        return 0;
    }

    void LogFaultingModuleProbe(EXCEPTION_POINTERS* info, const char* where) noexcept
    {
        if (!info || !info->ExceptionRecord)
            return;

        const DWORD code = static_cast<DWORD>(info->ExceptionRecord->ExceptionCode);

        if (code != STATUS_ACCESS_VIOLATION && code != 0xC0000409UL && code != 0xC0000374UL
            && code != kStowedExceptionCode)
            return;

        void* const faultIp = info->ExceptionRecord->ExceptionAddress;
        HMODULE faultMod = nullptr;
        (void)GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(faultIp),
            &faultMod);

        wchar_t faultName[MAX_PATH]{};
        if (faultMod)
            GetModuleFileNameW(faultMod, faultName, MAX_PATH);
        else
            wcscpy_s(faultName, L"<unknown>");

        const wchar_t* faultBase = faultName;
        for (const wchar_t* p = faultName; *p; ++p)
        {
            if (*p == L'\\' || *p == L'/')
                faultBase = p + 1;
        }

        unsigned glassLoaded = 0;
        if (GetModuleHandleW(L"GlassCompat.dll"))
            glassLoaded |= 1;
        if (GetModuleHandleW(L"GlassCore.dll"))
            glassLoaded |= 2;
        if (GetModuleHandleW(L"WndCompat.dll"))
            glassLoaded |= 4;
        if (GetModuleHandleW(L"ExplorerCompat.dll"))
            glassLoaded |= 8;

        const unsigned faultGlass = ClassifyGlassModule(faultMod);

        DebugLog(
            DBG_CRITICAL,
            L"[GlassCompat][CrashGuard] FAULT probe: code=0x%08X ip=0x%p module=%s "
            L"faultGlass=0x%X glassLoaded=0x%X where=%hs",
            code,
            faultIp,
            faultBase,
            faultGlass,
            glassLoaded,
            where);
    }

    [[nodiscard]] bool ReadRegDword(HKEY hKey, const wchar_t* valueName, DWORD& out, DWORD defaultVal) noexcept
    {
        DWORD v = defaultVal;
        DWORD cb = sizeof(v);
        if (RegGetValueW(hKey, nullptr, valueName, RRF_RT_REG_DWORD, nullptr, &v, &cb) != ERROR_SUCCESS)
        {
            out = defaultVal;
            return false;
        }
        out = v;
        return true;
    }

    // Только …\Crashes и Glass\CrashGuard. Корень BlackWindowManager не трогаем: там десятки чужих DWORD
    // (меню host UI, Root…) — одноимённый EnableMiniDump=0 отключал бы дампы, не доходя до legacy.
    [[nodiscard]] bool ReadRegDwordCrashSettingsThenLegacy(
        const wchar_t* valueName, DWORD& out, DWORD defaultVal) noexcept
    {
        auto tryPath = [&](const wchar_t* path) noexcept -> bool {
            HKEY hKey;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
                return false;
            DWORD v = defaultVal;
            DWORD cb = sizeof(v);
            const LSTATUS st = RegGetValueW(hKey, nullptr, valueName, RRF_RT_REG_DWORD, nullptr, &v, &cb);
            RegCloseKey(hKey);
            if (st != ERROR_SUCCESS)
                return false;
            out = v;
            return true;
        };

        if (tryPath(kCrashRegPath))
            return true;
        if (tryPath(kLegacyRegPath))
            return true;
        out = defaultVal;
        return false;
    }

    [[nodiscard]] bool IsMiniDumpEnabled() noexcept
    {
        DWORD v = 1;
        (void)ReadRegDwordCrashSettingsThenLegacy(L"EnableMiniDump", v, 1);
        return v != 0;
    }

    [[nodiscard]] bool IsMiniDumpFullMemory() noexcept
    {
        DWORD v = 0;
        (void)ReadRegDwordCrashSettingsThenLegacy(L"MiniDumpWithFullMemory", v, 0);
        return v != 0;
    }

    void DbgDumpLine(const wchar_t* text) noexcept
    {
        OutputDebugStringW(text);
    }

    // Создаёт цепочку каталогов для уже развёрнутого полного пути (без %VAR%).
    [[nodiscard]] bool CreateDirectoryTreeExpanded(const wchar_t* expandedPath) noexcept
    {
        if (!expandedPath || !expandedPath[0])
            return false;

        wchar_t buf[MAX_PATH * 2]{};
        if (wcslen(expandedPath) >= std::size(buf))
            return false;
        wcscpy_s(buf, expandedPath);

        for (wchar_t* p = buf; *p; ++p)
        {
            if (*p == L'\\' && p != buf)
            {
                *p = 0;
                (void)CreateDirectoryW(buf, nullptr);
                *p = L'\\';
            }
        }
        (void)CreateDirectoryW(buf, nullptr);
        return true;
    }

    [[nodiscard]] bool ExpandEnvAndCreateTree(const wchar_t* tpl, wchar_t* outDir, size_t outDirChars) noexcept
    {
        if (!tpl || !outDir || outDirChars < 8)
            return false;
        outDir[0] = 0;
        wchar_t expanded[MAX_PATH * 2]{};
        const DWORD cchBuf = static_cast<DWORD>(std::size(expanded) / sizeof(expanded[0]));
        const DWORD n = ExpandEnvironmentStringsW(tpl, expanded, cchBuf);
        if (n == 0)
            return false;
        // Успех: n — длина результата **включая** нуль; если n > cchBuf, буфер мал и строка обрезана.
        if (n > cchBuf)
            return false;
        if (!expanded[0])
            return false;
        if (!CreateDirectoryTreeExpanded(expanded))
            return false;
        wcsncpy_s(outDir, outDirChars, expanded, _TRUNCATE);
        return outDir[0] != 0;
    }

    // Не полагаться только на %VAR% — в раннем DllMain часть процессов даёт пустой блок.
    [[nodiscard]] bool BuildAndCreateDefaultDumpFolder(wchar_t* out, size_t outCch) noexcept
    {
        if (!out || outCch < 32)
            return false;
        out[0] = 0;
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", out, static_cast<DWORD>(outCch));
        if (n == 0 || n >= outCch)
        {
            wchar_t profile[MAX_PATH * 2]{};
            const DWORD np = GetEnvironmentVariableW(L"USERPROFILE", profile, static_cast<DWORD>(std::size(profile)));
            if (np == 0 || np >= std::size(profile))
                return false;
            if (swprintf_s(out, outCch, L"%s\\AppData\\Local", profile) < 0)
                return false;
        }
        if (wcscat_s(out, outCch, L"\\GlassPlatform\\Crashes") != 0)
            return false;
        return CreateDirectoryTreeExpanded(out);
    }

    [[nodiscard]] bool TryReadDumpFolderFromRegistryPath(const wchar_t* regPath, wchar_t* outDir, size_t outDirChars) noexcept
    {
        if (!regPath || !outDir || outDirChars < 8)
            return false;
        outDir[0] = 0;

        HKEY hKey;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, regPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
            return false;

        wchar_t raw[1024]{};
        DWORD cb = sizeof(raw);
        DWORD type = 0;
        const LSTATUS st = RegGetValueW(
            hKey,
            nullptr,
            L"DumpFolder",
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            &type,
            raw,
            &cb);
        RegCloseKey(hKey);
        if (st != ERROR_SUCCESS || cb < sizeof(wchar_t) || raw[0] == 0)
            return false;

        const DWORD cchOut = static_cast<DWORD>(outDirChars);
        const DWORD nex = ExpandEnvironmentStringsW(raw, outDir, cchOut);
        if (nex == 0 || nex > cchOut)
            return false;
        if (!outDir[0])
            return false;

        return CreateDirectoryTreeExpanded(outDir);
    }

    // DumpFolder (REG_SZ / REG_EXPAND_SZ) — свой каталог первым приоритетом.
    [[nodiscard]] bool TryReadRegistryDumpRoot(wchar_t* outDir, size_t outDirChars) noexcept
    {
        return TryReadDumpFolderFromRegistryPath(kCrashRegPath, outDir, outDirChars) ||
            TryReadDumpFolderFromRegistryPath(kBwmRegRoot, outDir, outDirChars) ||
            TryReadDumpFolderFromRegistryPath(kLegacyRegPath, outDir, outDirChars);
    }

    [[nodiscard]] bool GetCurrentExeBaseName(wchar_t* buf, size_t bufChars) noexcept
    {
        if (!buf || bufChars < 8)
            return false;
        buf[0] = 0;
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, path, MAX_PATH))
            return false;
        const wchar_t* slash = wcsrchr(path, L'\\');
        const wchar_t* base = slash ? slash + 1 : path;
        wcsncpy_s(buf, bufChars, base, _TRUNCATE);
        for (wchar_t* p = buf; *p; ++p)
        {
            if (*p == L'<' || *p == L'>' || *p == L':' || *p == L'\"' || *p == L'/' || *p == L'\\' ||
                *p == L'|' || *p == L'?' || *p == L'*')
                *p = L'_';
        }
        if (buf[0] == 0)
            wcscpy_s(buf, bufChars, L"process");
        return true;
    }

    [[nodiscard]] bool IsCurrentProcessExplorer() noexcept
    {
        wchar_t base[MAX_PATH]{};
        if (!GetCurrentExeBaseName(base, sizeof(base) / sizeof(base[0])))
            return false;
        return _wcsicmp(base, L"explorer.exe") == 0;
    }

    [[nodiscard]] bool InvokeMiniDumpWriteDump(
        DWORD pid,
        HANDLE hFile,
        MINIDUMP_TYPE dtype,
        MINIDUMP_EXCEPTION_INFORMATION* mei) noexcept
    {
        return MiniDumpWriteDump(GetCurrentProcess(), pid, hFile, dtype, mei, nullptr, nullptr) != FALSE;
    }

    // Несколько корней и понижение типа дампа — повышают шанс записи при нехватке места / сбое dbghelp.
    void TryWriteProcessMiniDump(EXCEPTION_POINTERS* info) noexcept
    {
        if (!info)
            return;

        // Один сбой часто проходит через VEH и UEF подряд — без паузы получаются дублирующие .dmp.
        static std::atomic<unsigned long long> s_lastSuccessfulDumpMs64{};
        const unsigned long long nowMs = GetTickCount64();
        const unsigned long long lastOk = s_lastSuccessfulDumpMs64.load(std::memory_order_relaxed);
        constexpr unsigned long long kMinMsBetweenOkDumps = 2000ull;
        if (lastOk != 0 && nowMs >= lastOk && (nowMs - lastOk) < kMinMsBetweenOkDumps)
        {
            DebugLog(
                DBG_NORMAL,
                L"[GlassCompat][CrashGuard] MiniDump пропущен: после успешного дампа прошло < %llu мс",
                static_cast<unsigned long long>(kMinMsBetweenOkDumps));
            return;
        }

        wchar_t exeBase[MAX_PATH]{};
        if (!GetCurrentExeBaseName(exeBase, sizeof(exeBase) / sizeof(exeBase[0])))
            wcscpy_s(exeBase, L"process");

        const DWORD pid = GetCurrentProcessId();

        wchar_t roots[kMaxDumpRoots][MAX_PATH * 2]{};
        size_t rootCount = 0;

        if (TryReadRegistryDumpRoot(roots[rootCount], std::size(roots[0])))
            ++rootCount;

        static constexpr const wchar_t* kFallbackTemplates[] = {
            L"%LOCALAPPDATA%\\GlassPlatform\\Crashes",
            L"%TEMP%\\GlassPlatformCrashes",
        };

        for (const wchar_t* tpl : kFallbackTemplates)
        {
            if (rootCount >= kMaxDumpRoots)
                break;
            if (ExpandEnvAndCreateTree(tpl, roots[rootCount], std::size(roots[0])))
                ++rootCount;
        }

        if (rootCount == 0)
        {
            DbgDumpLine(L"[GlassCompat][CrashGuard] MiniDump: не удалось подготовить ни одного каталога для дампа\n");
            return;
        }

        const bool wantFull = IsMiniDumpFullMemory();

        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = info;

        const MINIDUMP_TYPE tier1 = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
            (wantFull ? MiniDumpWithFullMemory : MiniDumpWithoutOptionalData));
        const MINIDUMP_TYPE tier2 =
            static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        const MINIDUMP_TYPE tier3 = MiniDumpNormal;
        const MINIDUMP_TYPE attempts[] = { tier1, tier2, tier3 };

        const DWORD exCode = info->ExceptionRecord
            ? static_cast<DWORD>(info->ExceptionRecord->ExceptionCode)
            : 0;

        for (size_t ri = 0; ri < rootCount; ++ri)
        {
            // Формат как у WER LocalDumps: {exe}.{pid}.dmp (дата — в метаданных ФС).
            wchar_t path[MAX_PATH * 4]{};
            swprintf_s(
                path,
                L"%s\\%s.%u.dmp",
                roots[ri],
                exeBase,
                static_cast<unsigned>(pid));

            const int kClientPtrPasses = (exCode == kStowedExceptionCode) ? 2 : 1;

            for (int pass = 0; pass < kClientPtrPasses; ++pass)
            {
                mei.ClientPointers = (pass == 1) ? TRUE : FALSE;

                for (MINIDUMP_TYPE dtype : attempts)
                {
                    HANDLE hFile = CreateFileW(
                        path,
                        GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ,
                        nullptr,
                        CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL,
                        nullptr);
                    if (hFile == INVALID_HANDLE_VALUE)
                    {
                        wchar_t line[512]{};
                        swprintf_s(
                            line,
                            L"[GlassCompat][CrashGuard] CreateFile дампа не удалось: le=%lu path=%s\n",
                            GetLastError(),
                            path);
                        DbgDumpLine(line);
                        break;
                    }

                    const bool ok = InvokeMiniDumpWriteDump(pid, hFile, dtype, &mei);
                    const DWORD gle = ok ? 0 : GetLastError();
                    CloseHandle(hFile);

                    if (ok)
                    {
                        s_lastSuccessfulDumpMs64.store(GetTickCount64(), std::memory_order_relaxed);
                        wchar_t line[MAX_PATH * 4 + 160]{};
                        swprintf_s(
                            line,
                            L"[GlassCompat][CrashGuard] MiniDump OK: %s (dtype=%u pass=%d clientPtr=%d)\n",
                            path,
                            static_cast<unsigned>(dtype),
                            pass,
                            mei.ClientPointers ? 1 : 0);
                        DbgDumpLine(line);
                        DebugLog(
                            DBG_IMPORTANT,
                            L"[GlassCompat][CrashGuard] MiniDump записан: %s",
                            path);
                        return;
                    }

                    (void)DeleteFileW(path);

                    wchar_t err[320]{};
                    swprintf_s(
                        err,
                        L"[GlassCompat][CrashGuard] MiniDump fail: gle=%lu dtype=%u pass=%d path=%s\n",
                        gle,
                        static_cast<unsigned>(dtype),
                        pass,
                        path);
                    DbgDumpLine(err);
                }
            }
        }

        DbgDumpLine(
            L"[GlassCompat][CrashGuard] MiniDump: все каталоги и уровни типа дампа исчерпаны\n");
        DebugLog(DBG_ERROR, L"[GlassCompat][CrashGuard] MiniDump: все попытки записи неудачны (см. OutputDebugString)");
    }

    [[nodiscard]] bool RecordExplorerCrashAndIsOverThreshold() noexcept
    {
        FILETIME nowFt{};
        GetSystemTimeAsFileTime(&nowFt);
        ULONGLONG now = 0;
        memcpy(&now, &nowFt, sizeof(now));

        HKEY hKey;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kCrashRegPath, 0, nullptr, 0,
                KEY_READ | KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
            return false;

        DWORD count = 0;
        DWORD cb = sizeof(count);
        (void)RegGetValueW(hKey, nullptr, L"GlassCrashSeriesCount", RRF_RT_REG_DWORD, nullptr, &count, &cb);

        FILETIME lastFt{};
        cb = sizeof(lastFt);
        const LSTATUS lrLast = RegGetValueW(
            hKey, nullptr, L"GlassCrashSeriesLastTime", RRF_RT_REG_BINARY, nullptr, &lastFt, &cb);
        ULONGLONG last = 0;
        if (lrLast == ERROR_SUCCESS && cb == sizeof(lastFt))
            memcpy(&last, &lastFt, sizeof(last));

        if (last == 0 || (now > last && (now - last) > kCrashWindowTicks))
            count = 0;

        ++count;
        RegSetValueExW(
            hKey,
            L"GlassCrashSeriesCount",
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&count),
            sizeof(count));
        RegSetValueExW(
            hKey,
            L"GlassCrashSeriesLastTime",
            0,
            REG_BINARY,
            reinterpret_cast<const BYTE*>(&nowFt),
            sizeof(nowFt));

        RegCloseKey(hKey);

        return count >= kMaxCrashes;
    }

    struct CrashPromptThreadData
    {
        int result;
    };

    static unsigned __stdcall CrashPromptThread(void* p) noexcept
    {
        auto* d = static_cast<CrashPromptThreadData*>(p);
        d->result = MessageBoxW(
            nullptr,
            L"Проводник (explorer.exe) несколько раз завершился с ошибкой за короткий период.\n"
            L"Остановить GlassPlatform, чтобы предотвратить дальнейшие падения?",
            L"GlassPlatform",
            MB_ICONERROR | MB_YESNO | MB_SETFOREGROUND | MB_TOPMOST | MB_SYSTEMMODAL);
        return 0;
    }

    [[nodiscard]] bool IsVectoredFastFailGuardEnabled() noexcept
    {
        DWORD v = 0;
        (void)ReadRegDwordCrashSettingsThenLegacy(L"EnableVectoredFastFailGuard", v, 0);
        return v != 0;
    }

    [[nodiscard]] bool IsVehStowedExceptionsEnabled() noexcept
    {
        DWORD v = 0;
        (void)ReadRegDwordCrashSettingsThenLegacy(L"EnableVehStowedExceptions", v, 0);
        return v != 0;
    }

    // DisableExplorerVehFatalDumps = 1 — отключить VEH-дампы по умолчанию в explorer.
    [[nodiscard]] bool IsExplorerVehFatalDumpsDisabled() noexcept
    {
        DWORD v = 0;
        (void)ReadRegDwordCrashSettingsThenLegacy(L"DisableExplorerVehFatalDumps", v, 0);
        return v != 0;
    }

    static void MaybePromptStopAfterExplorerSeries() noexcept
    {
        if (!IsCurrentProcessExplorer() || !RecordExplorerCrashAndIsOverThreshold())
            return;

        CrashPromptThreadData data{ IDNO };
        uintptr_t th = _beginthreadex(nullptr, 0, CrashPromptThread, &data, 0, nullptr);
        if (th)
        {
            WaitForSingleObject(reinterpret_cast<HANDLE>(th), INFINITE);
            CloseHandle(reinterpret_cast<HANDLE>(th));
        }

        if (data.result == IDYES)
        {
            if (HANDLE hStop = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName))
            {
                (void)SetEvent(hStop);
                CloseHandle(hStop);
            }
        }
    }

    // Первый шанс: многие краши WinUI не доходят до UEF — пишем дамп здесь (explorer по умолчанию).
    LONG NTAPI GlassVectoredDumpHandler(EXCEPTION_POINTERS* info) noexcept
    {
        if (!info || !info->ExceptionRecord)
            return EXCEPTION_CONTINUE_SEARCH;

        const DWORD code = static_cast<DWORD>(info->ExceptionRecord->ExceptionCode);
        const bool explorer = IsCurrentProcessExplorer();

        // AutoIt/JIT: first-chance AV вне Glass — без DebugLog/MiniDump (VEH+лог вешали UI).
        if (!explorer && code == STATUS_ACCESS_VIOLATION)
        {
            void* const faultIp = info->ExceptionRecord->ExceptionAddress;
            HMODULE faultMod = nullptr;
            (void)GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(faultIp),
                &faultMod);
            if (ClassifyGlassModule(faultMod) == 0)
                return EXCEPTION_CONTINUE_SEARCH;
        }

        LogFaultingModuleProbe(info, "VEH");
        if (code == 0xC0000409UL)
        {
            const ULONGLONG fastFailCode = info->ExceptionRecord->NumberParameters > 0
                ? static_cast<ULONGLONG>(info->ExceptionRecord->ExceptionInformation[0])
                : 0;
            (void)fastFailCode;
        }

        bool wantDump = false;
        if (explorer && IsMiniDumpEnabled() && !IsExplorerVehFatalDumpsDisabled())
        {
            if (code == STATUS_ACCESS_VIOLATION || code == 0xC0000409UL || code == 0xC0000374UL ||
                code == kStowedExceptionCode)
                wantDump = true;
        }
        // Не-explorer: AV-дамп только Glass IP (non-Glass уже early-return). Fast-fail/heap — редко.
        if (!explorer && IsMiniDumpEnabled())
        {
            if (code == 0xC0000409UL || code == 0xC0000374UL || code == STATUS_ACCESS_VIOLATION)
                wantDump = true;
        }
        if (g_vehRecordFastFail && (code == 0xC0000409UL || code == 0xC0000374UL))
            wantDump = true;
        if (g_vehRecordStowed && code == kStowedExceptionCode)
            wantDump = true;

        if (!wantDump)
            return EXCEPTION_CONTINUE_SEARCH;

        if (IsMiniDumpEnabled())
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[GlassCompat][CrashGuard] VEH первый шанс → дамп: code=0x%08X tid=%u explorer=%d",
                code,
                static_cast<unsigned>(GetCurrentThreadId()),
                explorer ? 1 : 0);
            TryWriteProcessMiniDump(info);
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }

    LONG NTAPI ExplorerCrashGuardFilter(EXCEPTION_POINTERS* info) noexcept
    {
        LogFaultingModuleProbe(info, "UEF");
        // Сначала дамп — пока контекст гарантированно доступен; потом цепочка prev (часто не возвращается).
        if (IsMiniDumpEnabled())
            TryWriteProcessMiniDump(info);

        const LONG result = g_prevExplorerFilter
            ? g_prevExplorerFilter(info)
            : EXCEPTION_CONTINUE_SEARCH;

        MaybePromptStopAfterExplorerSeries();

        return result;
    }
    // При первой загрузке GlassCompat: ключ HKCU\…\BlackWindowManager\Crashes и каталог по умолчанию,
    // чтобы настройки не «пропадали» из виду и дампы не отключались чужим DWORD в корне BWM.
    void EnsureCrashGuardRegistryBaseline() noexcept
    {
        HKEY hKey;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kCrashRegPath, 0, nullptr, 0,
                KEY_READ | KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
            return;

        DWORD cb = 0;
        const LSTATUS stMini = RegGetValueW(
            hKey, nullptr, L"EnableMiniDump", RRF_RT_REG_DWORD, nullptr, nullptr, &cb);
        if (stMini == ERROR_FILE_NOT_FOUND)
        {
            const DWORD one = 1;
            (void)RegSetValueExW(
                hKey,
                L"EnableMiniDump",
                0,
                REG_DWORD,
                reinterpret_cast<const BYTE*>(&one),
                sizeof(one));
        }

        cb = 0;
        const LSTATUS stDump = RegGetValueW(
            hKey, nullptr, L"DumpFolder", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, nullptr, &cb);
        if (stDump == ERROR_FILE_NOT_FOUND)
        {
            static constexpr wchar_t kDefaultDumpFolderTpl[] =
                L"%LOCALAPPDATA%\\GlassPlatform\\Crashes";
            (void)RegSetValueExW(
                hKey,
                L"DumpFolder",
                0,
                REG_EXPAND_SZ,
                reinterpret_cast<const BYTE*>(kDefaultDumpFolderTpl),
                static_cast<DWORD>(sizeof(kDefaultDumpFolderTpl)));
        }

        RegCloseKey(hKey);

        wchar_t dir[MAX_PATH * 2]{};
        const bool mk =
            BuildAndCreateDefaultDumpFolder(dir, std::size(dir)) ||
            (ExpandEnvAndCreateTree(L"%LOCALAPPDATA%\\GlassPlatform\\Crashes", dir, std::size(dir)) && dir[0]);
        if (mk && dir[0])
        {
            wchar_t ods[700]{};
            swprintf_s(ods, L"[GlassCompat][CrashGuard] каталог дампов (инициализация): %s\n", dir);
            OutputDebugStringW(ods);
        }
        else
        {
            OutputDebugStringW(
                L"[GlassCompat][CrashGuard] не удалось создать каталог GlassPlatform\\Crashes (см. "
                L"LOCALAPPDATA/USERPROFILE)\n");
        }
    }
} // namespace

bool GlassCompat::IsProcessCrashGuardInstalled() noexcept
{
    return g_crashGuardInstalled;
}

void GlassCompat::InstallProcessCrashGuard() noexcept
{
    if (g_crashGuardInstalled)
        return;

    (void)Shared::InjectProcessMap::TrySetDiagLifecycle(
        GetCurrentProcessId(),
        Shared::InjectProcessMap::DiagLifecycleCrashGuard,
        true);

    EnsureCrashGuardRegistryBaseline();

    g_prevExplorerFilter = SetUnhandledExceptionFilter(ExplorerCrashGuardFilter);

    g_vehRecordFastFail = IsVectoredFastFailGuardEnabled();
    g_vehRecordStowed = IsVehStowedExceptionsEnabled();
    const bool explorerDefaultVeh =
        IsCurrentProcessExplorer() && IsMiniDumpEnabled() && !IsExplorerVehFatalDumpsDisabled();
    // Всегда ставим VEH: probe модуля модуля + first-chance AV dump для не-explorer (HWiNFO).
    if (!g_vehFastFail)
    {
        g_vehFastFail = AddVectoredExceptionHandler(1, GlassVectoredDumpHandler);
        wchar_t ods[320]{};
        swprintf_s(
            ods,
            L"[GlassCompat][CrashGuard] VEH дамп: установлен (explorer.def=%d EnableVectoredFastFail=%d "
            L"EnableVehStowed=%d always=1)\n",
            explorerDefaultVeh ? 1 : 0,
            g_vehRecordFastFail ? 1 : 0,
            g_vehRecordStowed ? 1 : 0);
        OutputDebugStringW(ods);
    }

    g_crashGuardInstalled = true;
    (void)Shared::InjectProcessMap::TrySetDiagLifecycle(
        GetCurrentProcessId(),
        Shared::InjectProcessMap::DiagLifecycleCrashGuard,
        false);
    (void)Shared::InjectProcessMap::TrySetDiagFlag(
        GetCurrentProcessId(),
        Shared::InjectProcessMap::DiagCrashGuard,
        true);
}

void GlassCompat::ReassertProcessCrashGuard() noexcept
{
    if (!g_crashGuardInstalled)
        return;

    // После DllMain другие DLL (CRT/рантайм/инжекты) часто вызывают SetUnhandledExceptionFilter
    // и снимают нас с вершины цепочки — тогда дампы при необработанном исключении не пишутся.
    LPTOP_LEVEL_EXCEPTION_FILTER displaced = SetUnhandledExceptionFilter(ExplorerCrashGuardFilter);
    if (displaced != ExplorerCrashGuardFilter && displaced != nullptr)
        g_prevExplorerFilter = displaced;

    if (displaced != nullptr && displaced != ExplorerCrashGuardFilter)
    {
        DebugLog(
            DBG_IMPORTANT,
            L"[GlassCompat][CrashGuard] UEF снова установлен поверх; предыдущий верхний фильтр=0x%p",
            reinterpret_cast<void*>(displaced));
    }
    else
    {
        DebugLog(DBG_NORMAL, L"[GlassCompat][CrashGuard] UEF подтверждён (мы уже были верхним или цепочка пуста)");
    }

    (void)Shared::InjectProcessMap::TrySetDiagFlag(
        GetCurrentProcessId(),
        Shared::InjectProcessMap::DiagCrashGuard,
        true);
}

void GlassCompat::UninstallProcessCrashGuard() noexcept
{
    if (!g_crashGuardInstalled)
        return;

    if (g_vehFastFail)
    {
        (void)RemoveVectoredExceptionHandler(g_vehFastFail);
        g_vehFastFail = nullptr;
    }
    g_vehRecordFastFail = false;
    g_vehRecordStowed = false;

    // SetUnhandledExceptionFilter возвращает прежний верхний фильтр.
    // Если это не наш — чужой UEF встал позже: сразу вернуть его и не
    // затирать чужую цепочку сохранённым prev при выгрузке.
    const LPTOP_LEVEL_EXCEPTION_FILTER current =
        SetUnhandledExceptionFilter(g_prevExplorerFilter);
    if (current != ExplorerCrashGuardFilter)
    {
        (void)SetUnhandledExceptionFilter(current);
        DebugLog(
            DBG_IMPORTANT,
            L"[GlassCompat][CrashGuard] UEF не наш (0x%p) — оставляем чужой фильтр",
            reinterpret_cast<void*>(current));
    }

    g_prevExplorerFilter = nullptr;
    g_crashGuardInstalled = false;
    (void)Shared::InjectProcessMap::TrySetDiagFlag(
        GetCurrentProcessId(),
        Shared::InjectProcessMap::DiagCrashGuard,
        false);
}
