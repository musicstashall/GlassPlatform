#include "CompatPch.h"
#include "HostShellWatchdog.h"
#include "HostPidHelper.h"
#include <Windows.h>

// По мотивам BWM Framework::DoExplorerCrashCheck: два исчезновения shell PID подряд
// за короткое окно — вероятная серия падений проводника при активной платформе.

void GlassCompat::HostPollExplorerShellWatchdog() noexcept
{
    static ULONGLONG s_nextDueMs = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < s_nextDueMs)
        return;
    s_nextDueMs = now + 2000;

    DWORD explorerPid = 0;
    if (HWND shell = GetShellWindow())
        (void)GetWindowThreadProcessId(shell, &explorerPid);

    static DWORD s_lastShellPid = 0;
    static ULONGLONG s_lastDeathTickMs = 0;

    if (explorerPid != 0)
    {
        s_lastShellPid = explorerPid;
        return;
    }

    if (s_lastShellPid == 0)
        return;

    constexpr ULONGLONG kRapidDeathWindowMs = 30'000;
    const ULONGLONG t = GetTickCount64();

    if (s_lastDeathTickMs != 0 && (t - s_lastDeathTickMs) < kRapidDeathWindowMs)
    {
        const int res = MessageBoxW(
            nullptr,
            L"Оболочка проводника (GetShellWindow) дважды пропала за короткое время.\n"
            L"Остановить GlassPlatform, чтобы предотвратить цикл перезапусков?",
            L"GlassPlatform",
            MB_ICONERROR | MB_YESNO | MB_SETFOREGROUND | MB_TOPMOST);

        if (res == IDYES)
        {
            const wchar_t* names[] = {
                L"GlassPlatform.StopEvent.x64",
                L"GlassPlatform.StopEvent.x86",
            };
            for (const wchar_t* name : names)
            {
                if (HANDLE hStop = OpenEventW(EVENT_MODIFY_STATE, FALSE, name))
                {
                    (void)SetEvent(hStop);
                    CloseHandle(hStop);
                }
            }
        }

        s_lastDeathTickMs = 0;
        s_lastShellPid = 0;
        return;
    }

    s_lastDeathTickMs = t;
    s_lastShellPid = 0;
}
