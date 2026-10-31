// DllMain.cpp (минимальный)
#include "CompatPch.h"
#include "GlassCompat.h"
#include <CrashGuard.h>
#include <Windows.h>
#include <wchar.h>
#include <GlassUlTrace.h>

#if defined(_DEBUG)
#include <crtdbg.h>
#include <cstdlib>
#endif

// #region agent log
// GLASS-ULTRACE (R5): сколько прожил модуль в процессе. Нужно для short-lived процессов
// (taskhostw живёт десятки мс): по alive_ms видно, успел ли наш watcher вообще начать работу.
static ULONGLONG g_ultraceAttachTick = 0;
// #endregion

// ── R7 (H97): гейт загрузки ─────────────────────────────────────────────────
//
// user32 грузит INCONTEXT-hook DLL (глобальный SetWinEventHook из хоста) в КАЖДЫЙ процесс
// с окнами и держит в нём указатель на GlassCompat_WinEventProc. Ссылку на модуль в таком
// процессе снимает ОС, и только тогда, когда процесс сам обработает снятие хука. Снять её
// снаружи (CreateRemoteThread+FreeLibrary) нельзя без use-after-unmap: дамп
// `Яндекс Музыка.exe.35444` — AV 0xC0000005 записью по 0x8 внутри уже выгруженного
// GlassCompat (стек вызова — user32). Поэтому единственный безопасный способ не иметь
// проблем в «чужих» процессах — не загружаться в них вовсе.
//
// Хост (EntryRundll.cpp) на старте пишет `%LOCALAPPDATA%\GlassPlatform\attach-gate.txt`:
//
//     v1
//     mode=deny            // deny: перечисленные процессы НЕ получают DLL
//     name=яндекс музыка.exe
//     ...
//
// (`mode=allow` — наоборот, грузимся только в перечисленные; `mode=off` или отсутствие
// файла — гейт выключен). Хост берёт имена из ProcessBlockList (встроенный + реестр),
// AttachList=Forbidden и GLASS_ATTACH_GATE_EXTRA.
//
// Файл, а не реестр: в DllMain нельзя дёргать advapi32 (загрузка под loader lock).
// Читаем только kernel32 (CreateFileW/ReadFile) + статический буфер, без аллокаций CRT.
namespace
{
    constexpr wchar_t kGateRelPath[] = L"GlassPlatform\\attach-gate.txt";
    constexpr DWORD   kGateMaxChars = 16384;

    enum class GateMode : int
    {
        Off = 0,   // файла нет / mode=off — ведём себя как раньше
        Allow = 1, // грузимся только в перечисленные процессы
        Deny = 2,  // не грузимся в перечисленные процессы
    };

    wchar_t GateLower(wchar_t c) noexcept
    {
        return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
    }

    bool GateNameEquals(const wchar_t* a, const wchar_t* b) noexcept
    {
        while (*a && *b)
        {
            if (GateLower(*a) != GateLower(*b))
                return false;
            ++a;
            ++b;
        }
        return *a == 0 && *b == 0;
    }

    bool GateBuildPath(wchar_t* out, DWORD cch) noexcept
    {
        wchar_t base[MAX_PATH]{};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, _countof(base));
        if (n == 0 || n >= _countof(base))
            return false;
        return swprintf_s(out, cch, L"%s\\%s", base, kGateRelPath) > 0;
    }

    /// true — процессу разрешено грузить DLL.
    bool GateAllowsAttach(const wchar_t* exeName, GateMode& modeOut, wchar_t* modeText,
        DWORD modeTextCch) noexcept
    {
        modeOut = GateMode::Off;
        wcsncpy_s(modeText, modeTextCch, L"missing", _TRUNCATE);

        wchar_t path[MAX_PATH * 2]{};
        if (!GateBuildPath(path, _countof(path)))
            return true;

        HANDLE hFile = CreateFileW(
            path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
            return true;

        static wchar_t s_buf[kGateMaxChars];
        DWORD read = 0;
        const BOOL readOk = ReadFile(
            hFile, s_buf, static_cast<DWORD>(sizeof(s_buf) - sizeof(wchar_t)), &read, nullptr);
        CloseHandle(hFile);
        if (!readOk || read == 0)
            return true;

        const DWORD chars = read / sizeof(wchar_t);
        s_buf[chars] = 0;

        bool allow = true;
        const wchar_t* line = s_buf;
        while (line && *line)
        {
            const wchar_t* end = line;
            while (*end && *end != L'\n')
                ++end;

            if (end > line && end[-1] == L'\r')
                --end;

            const wchar_t* name = nullptr;
            if (end - line > 5 && wcsncmp(line, L"mode=", 5) == 0)
            {
                const wchar_t* v = line + 5;
                const bool isAllow = (end - v == 5 && wcsncmp(v, L"allow", 5) == 0);
                const bool isDeny = (end - v == 4 && wcsncmp(v, L"deny", 4) == 0);
                const bool isOff = (end - v == 3 && wcsncmp(v, L"off", 3) == 0);
                if (isAllow)
                {
                    modeOut = GateMode::Allow;
                    allow = false; // пока имя не встретится
                    wcsncpy_s(modeText, modeTextCch, L"allow", _TRUNCATE);
                }
                else if (isDeny)
                {
                    modeOut = GateMode::Deny;
                    allow = true;
                    wcsncpy_s(modeText, modeTextCch, L"deny", _TRUNCATE);
                }
                else if (isOff)
                {
                    modeOut = GateMode::Off;
                    allow = true;
                    wcsncpy_s(modeText, modeTextCch, L"off", _TRUNCATE);
                }
            }
            else if (end - line > 5 && wcsncmp(line, L"name=", 5) == 0)
            {
                name = line + 5;
            }

            if (name && exeName)
            {
                wchar_t candidate[128]{};
                const size_t nameLen = static_cast<size_t>(end - name);
                if (nameLen > 0 && nameLen < _countof(candidate))
                {
                    for (size_t i = 0; i < nameLen; ++i)
                        candidate[i] = GateLower(name[i]);
                    if (GateNameEquals(candidate, exeName))
                        allow = (modeOut == GateMode::Allow);
                }
            }

            line = (*end == L'\n') ? end + 1 : end;
        }

        return allow;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        // CrashGuard не ставим здесь: INCONTEXT грузит DLL и в BlockList/Forbidden
        // (taskhostw и др.), где нужна только выгрузка path=2/3. UEF/VEH — в
        // long-lived хосте (/start) и в GlassCompat::Initialize для Normal.
#if defined(_DEBUG)
        // Debug CRT по умолчанию показывает модальный abort()/assert-диалог.
        // При reboot это блокирует выключение в conhost/msedgewebview2.
        // Сообщение остаётся в OutputDebugString; WER не трогаем.
        (void)_set_abort_behavior(0, _WRITE_ABORT_MSG);
        (void)_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
        (void)_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
        (void)_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
#endif
        g_ultraceAttachTick = GetTickCount64();
        // #region agent log
        // GLASS-ULTRACE
        GlassUlTrace::Logf(
            "H82",
            "DllMain.cpp:DllMain",
            "dll_attach",
            "\"base\":\"%p\",\"reason\":\"attach\",\"rc\":%u",
            static_cast<void*>(hModule),
            GlassUlTrace::QueryLoadCount(hModule));
        // #endregion
        // R7: гейт загрузки. Отказ — DllMain возвращает FALSE, модуль остаётся
        // незагруженным: user32 не получит колбэк в этом процессе, а значит не будет
        // ни «висящей» OS-ссылки, ни остатка DLL после Stop, ни AV в чужом стеке.
        {
            wchar_t exePath[MAX_PATH]{};
            const DWORD exeLen = GetModuleFileNameW(nullptr, exePath, _countof(exePath));
            const wchar_t* exeStem = exePath;
            if (exeLen > 0)
            {
                for (DWORD i = exeLen; i > 0; --i)
                {
                    if (exePath[i - 1] == L'\\' || exePath[i - 1] == L'/')
                    {
                        exeStem = exePath + i;
                        break;
                    }
                }
            }
            GateMode gateMode = GateMode::Off;
            wchar_t modeText[16]{};
            const bool allow = GateAllowsAttach(exeStem, gateMode, modeText, _countof(modeText));
            char exeJson[160]{};
            GlassUlTrace::WideToJson(exeJson, sizeof(exeJson), exeStem);
            GlassUlTrace::Logf(
                "H97",
                "DllMain.cpp:DllMain",
                "attach_gate",
                "\"exe\":\"%s\",\"mode\":\"%s\",\"allow\":%d",
                exeJson,
                modeText,
                allow ? 1 : 0);
            if (!allow)
            {
                GlassUlTrace::Logf(
                    "H97",
                    "DllMain.cpp:DllMain",
                    "dll_attach_denied",
                    "\"exe\":\"%s\",\"mode\":\"%s\"",
                    exeJson,
                    modeText);
                return FALSE;
            }
        }
        break;
    case DLL_PROCESS_DETACH:
        // Не Stop() лог-клиента здесь: join воркера под loader lock запрещён.
        // GUI / MinimalUnloadWatcher / short-lived Main стопают клиент сами.
        // #region agent log
        // GLASS-ULTRACE
        GlassUlTrace::Logf(
            "H82",
            "DllMain.cpp:DllMain",
            "dll_detach",
            "\"base\":\"%p\",\"reason\":\"detach\",\"process_exit\":%d,\"alive_ms\":%llu",
            static_cast<void*>(hModule),
            reserved ? 1 : 0,
            g_ultraceAttachTick ? (GetTickCount64() - g_ultraceAttachTick) : 0ull);
        // #endregion
        if (GlassCompat::IsProcessCrashGuardInstalled())
            GlassCompat::UninstallProcessCrashGuard();
        break;
    }
    return TRUE;
}
