// DllMain.cpp (минимальный)
#include "CompatPch.h"
#include "GlassCompat.h"
#include <CrashGuard.h>
#include <Windows.h>
#include <wchar.h>
#include <string.h>
#include <GlassUlTrace.h>
#include <ProcessBlockList.h>
#include <cstddef>

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

    // R9: путь гейта. В прогоне 9 приложение Яндекс Музыки (`Y.Music.exe`, pid 904)
    // получило `gate="missing"` при уже записанном файле — почти наверняка потому, что
    // процесс живёт с другой средой (AppContainer/пакетное приложение переписывает
    // LOCALAPPDATA на свой LocalCache). Поэтому: LOCALAPPDATA → USERPROFILE\AppData\Local,
    // а если и это недоступно — гейт работает fail-closed (см. GateAllowsAttach).
    bool GateBuildPath(wchar_t* out, DWORD cch) noexcept
    {
        wchar_t base[MAX_PATH]{};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, _countof(base));
        if (n > 0 && n < _countof(base))
            return swprintf_s(out, cch, L"%s\\%s", base, kGateRelPath) > 0;

        const DWORD m = GetEnvironmentVariableW(L"USERPROFILE", base, _countof(base));
        if (m > 0 && m < _countof(base))
            return swprintf_s(out, cch, L"%s\\AppData\\Local\\%s", base, kGateRelPath) > 0;

        return false;
    }

    // Процессы, которые обязаны получать DLL даже при нечитаемом/отсутствующем гейте:
    // иначе платформа не запустится (rundll32-хост), не покажет процессы (GlassSettings),
    // не отрисует shell-хосты UICompat и не починит панель задач (explorer).
    bool GateIsAlwaysTrusted(const wchar_t* exeName) noexcept
    {
        static const wchar_t* const kTrusted[] = {
            L"rundll32.exe",
            L"GlassPlatform.exe",
            L"GlassSettings.exe",
            L"LogViewer.exe",
            L"explorer.exe",
            L"shellexperiencehost.exe",
            L"shellhost.exe",
            L"startmenuexperiencehost.exe",
            L"searchhost.exe",
            L"searchapp.exe",
        };
        for (const wchar_t* t : kTrusted)
        {
            if (GateNameEquals(exeName, t))
                return true;
        }
        return false;
    }

    // Встроенный deny: имена из ProcessBlockList (header-only список, без реестра и без
    // I/O — в DllMain это обязательное условие) плюс приложения, которые падали в чужом
    // стеке после выгрузки GlassCompat.
    bool GateIsBuiltInDeny(const wchar_t* exeName) noexcept
    {
        static const wchar_t* const kExtra[] = {
            L"Y.Music.exe",
            L"Яндекс Музыка.exe",
            L"YMusic.exe",
            L"RuntimeBroker.exe",
            L"ApplicationFrameHost.exe",
        };
        for (const wchar_t* n : kExtra)
        {
            if (GateNameEquals(exeName, n))
                return true;
        }
        for (std::size_t i = 0; i < ProcessBlockList::BuiltInCount(); ++i)
        {
            if (GateNameEquals(exeName, ProcessBlockList::kBuiltInNames[i]))
                return true;
        }
        return false;
    }

    /// true — процессу разрешено грузить DLL. `reasonOut` — короткая причина для лога.
    bool GateAllowsAttach(const wchar_t* exeName, GateMode& modeOut, char* modeText,
        size_t modeTextCch, char* reasonOut, size_t reasonCch, unsigned long& gleOut) noexcept
    {
        modeOut = GateMode::Off;
        gleOut = 0;
        strncpy_s(modeText, modeTextCch, "missing", _TRUNCATE);
        strncpy_s(reasonOut, reasonCch, "file", _TRUNCATE);

        // (1) Встроенный список: не зависит ни от файла, ни от реестра, ни от среды.
        // Именно здесь ловится прогон 9: `Y.Music.exe` получил allow=1 лишь потому, что
        // гейт-файл в его среде не читался ("missing").
        if (GateIsBuiltInDeny(exeName))
        {
            strncpy_s(modeText, modeTextCch, "builtin", _TRUNCATE);
            strncpy_s(reasonOut, reasonCch, "builtin", _TRUNCATE);
            return false;
        }
        if (GateIsAlwaysTrusted(exeName))
        {
            strncpy_s(reasonOut, reasonCch, "trusted", _TRUNCATE);
            return true;
        }

        // (2) Файл гейта. FAIL-CLOSED: если его не прочитать (нет среды, ACL AppContainer,
        // платформа не стартовала) — не грузимся. Для остальных процессов это безопасно:
        // DLL приходит только от наших хуков, и без работающей платформы ей там нечего делать.
        wchar_t path[MAX_PATH * 2]{};
        if (!GateBuildPath(path, _countof(path)))
        {
            strncpy_s(modeText, modeTextCch, "noenv", _TRUNCATE);
            strncpy_s(reasonOut, reasonCch, "noenv", _TRUNCATE);
            return false;
        }

        HANDLE hFile = CreateFileW(
            path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            gleOut = static_cast<unsigned long>(GetLastError());
            return false;
        }

        static wchar_t s_buf[kGateMaxChars];
        DWORD read = 0;
        const BOOL readOk = ReadFile(
            hFile, s_buf, static_cast<DWORD>(sizeof(s_buf) - sizeof(wchar_t)), &read, nullptr);
        CloseHandle(hFile);
        if (!readOk || read == 0)
        {
            gleOut = static_cast<unsigned long>(GetLastError());
            return false;
        }

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
                    strncpy_s(modeText, modeTextCch, "allow", _TRUNCATE);
                }
                else if (isDeny)
                {
                    modeOut = GateMode::Deny;
                    allow = true;
                    strncpy_s(modeText, modeTextCch, "deny", _TRUNCATE);
                }
                else if (isOff)
                {
                    modeOut = GateMode::Off;
                    allow = true;
                    strncpy_s(modeText, modeTextCch, "off", _TRUNCATE);
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
            // Отказ кэшируем на процесс. Статик модуля НЕ годится: при возврате FALSE модуль
            // не загружается, и его состояние исчезает — так в прогоне 9 получилось 21
            // сообщение вместо одного. Держим kernel-объект с именем pid: он живёт в таблице
            // хэндлов процесса, а не в модуле, и переживает возврат FALSE.
            wchar_t denyName[64]{};
            swprintf_s(denyName, L"Local\\GlassCompatGateDeny_%lu",
                static_cast<unsigned long>(GetCurrentProcessId()));
            if (HANDLE hDenied = OpenMutexW(SYNCHRONIZE, FALSE, denyName))
            {
                CloseHandle(hDenied);
                return FALSE;
            }

            GateMode gateMode = GateMode::Off;
            // mode — узкая строка: Logf принимает const char*, wide-строка печаталась
            // как один символ ("d" вместо "deny") в прогоне 8.
            char modeText[16]{};
            char reason[16]{};
            unsigned long gle = 0;
            const bool allow = GateAllowsAttach(
                exeStem, gateMode, modeText, _countof(modeText),
                reason, _countof(reason), gle);
            // exe уже есть в контексте строки лога (GlassUlTrace::Log добавляет его сам),
            // поэтому в data кладём только решение гейта и причину.
            GlassUlTrace::Logf(
                "H97",
                "DllMain.cpp:DllMain",
                "attach_gate",
                "\"gate\":\"%s\",\"reason\":\"%s\",\"allow\":%d,\"gle\":%lu",
                modeText,
                reason,
                allow ? 1 : 0,
                gle);
            if (!allow)
            {
                // Маркер «уже отказали» — повторные LoadLibrary от user32 молчат.
                (void)CreateMutexW(nullptr, FALSE, denyName);
                GlassUlTrace::Logf(
                    "H97",
                    "DllMain.cpp:DllMain",
                    "dll_attach_denied",
                    "\"gate\":\"%s\",\"reason\":\"%s\",\"gle\":%lu",
                    modeText,
                    reason,
                    gle);
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
