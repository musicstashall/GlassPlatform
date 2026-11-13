#include "CompatPch.h"
#include <GlassStop.h>
#include "CrashGuard.h"
#include "HostShellWatchdog.h"

#include <atomic>
#include <process.h>

// Хост считает серию падений Explorer (реестр, окно 3 минуты) и два исчезновения
// оболочки подряд. Диалог один, на потоке хоста: через 20 с без ответа платформа
// останавливается. В падающем Explorer диалог не показываем — кнопки там не нажимаются.

namespace
{
    constexpr ULONGLONG kPollPeriodMs = 2000;
    constexpr ULONGLONG kRapidDeathWindowMs = 30'000;
    constexpr ULONGLONG kPromptTimeoutMs = 20'000;
    constexpr DWORD kCrashSeriesThreshold = 3;

    enum class PromptPhase : int
    {
        Idle = 0,
        Showing = 1,
        Stopping = 2,
    };

    PromptPhase g_phase = PromptPhase::Idle;
    ULONGLONG g_deadlineMs = 0;
    DWORD g_promptThreadId = 0;
    HANDLE g_promptThread = nullptr;
    std::atomic<int> g_answer{ 0 };

    void SignalPlatformStopBothArch() noexcept
    {
        for (const auto arch : { Shared::GlassStop::PlatformArch::X64,
                                 Shared::GlassStop::PlatformArch::X86 })
        {
            Shared::GlassStop::Options options{};
            options.reason = Shared::GlassStop::Reason::Crash;
            options.gracefulMs = 0;
            options.hostGoneMs = 0;
            (void)Shared::GlassStop::Request(arch, options);
        }
    }

    BOOL CALLBACK ClosePromptWindow(HWND hwnd, LPARAM) noexcept
    {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return TRUE;
    }

    void DismissPromptWindows() noexcept
    {
        if (g_promptThreadId == 0)
            return;
        EnumThreadWindows(g_promptThreadId, ClosePromptWindow, 0);
    }

    void ClosePromptThreadHandle() noexcept
    {
        if (!g_promptThread)
            return;
        CloseHandle(g_promptThread);
        g_promptThread = nullptr;
        g_promptThreadId = 0;
    }

    unsigned __stdcall CrashPromptThread(void*) noexcept
    {
        const int result = MessageBoxW(
            nullptr,
            L"Проводник несколько раз завершился с ошибкой за короткое время.\n"
            L"GlassPlatform остановится через 20 секунд, если не ответить.\n"
            L"Остановить платформу сейчас?",
            L"GlassPlatform",
            MB_ICONERROR | MB_YESNO | MB_SETFOREGROUND | MB_TOPMOST);
        g_answer.store(result, std::memory_order_release);
        return 0;
    }

    void ArmCrashPrompt(ULONGLONG now) noexcept
    {
        if (g_phase != PromptPhase::Idle)
            return;

        g_answer.store(0, std::memory_order_relaxed);
        g_deadlineMs = now + kPromptTimeoutMs;
        g_phase = PromptPhase::Showing;

        unsigned threadId = 0;
        const uintptr_t thread = _beginthreadex(nullptr, 0, CrashPromptThread, nullptr, 0, &threadId);
        if (!thread)
        {
            // Спросить не удалось — цикл падений нельзя оставлять без остановки.
            SignalPlatformStopBothArch();
            g_phase = PromptPhase::Stopping;
            return;
        }
        g_promptThread = reinterpret_cast<HANDLE>(thread);
        g_promptThreadId = threadId;
    }

    enum class PromptPoll : int
    {
        Continue = 0,
        ResetShell = 1,
        Stopping = 2,
    };

    [[nodiscard]] PromptPoll PollCrashPrompt(ULONGLONG now) noexcept
    {
        if (g_phase == PromptPhase::Stopping)
            return PromptPoll::Stopping;
        if (g_phase != PromptPhase::Showing)
            return PromptPoll::Continue;

        const int answer = g_answer.load(std::memory_order_acquire);
        if (answer == IDNO)
        {
            GlassCompat::ResetExplorerCrashSeriesCount();
            ClosePromptThreadHandle();
            g_phase = PromptPhase::Idle;
            return PromptPoll::ResetShell;
        }
        if (answer == IDYES || now >= g_deadlineMs)
        {
            DismissPromptWindows();
            SignalPlatformStopBothArch();
            ClosePromptThreadHandle();
            g_phase = PromptPhase::Stopping;
            return PromptPoll::Stopping;
        }
        return PromptPoll::Continue;
    }
}

void GlassCompat::HostPollExplorerShellWatchdog() noexcept
{
    static ULONGLONG s_nextDueMs = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < s_nextDueMs)
        return;
    s_nextDueMs = now + kPollPeriodMs;

    const PromptPoll prompt = PollCrashPrompt(now);
    if (prompt == PromptPoll::Stopping)
        return;

    static DWORD s_lastShellPid = 0;
    static ULONGLONG s_lastDeathTickMs = 0;
    if (prompt == PromptPoll::ResetShell)
    {
        s_lastShellPid = 0;
        s_lastDeathTickMs = 0;
    }

    DWORD explorerPid = 0;
    if (HWND shell = GetShellWindow())
        (void)GetWindowThreadProcessId(shell, &explorerPid);

    if (explorerPid != 0)
    {
        s_lastShellPid = explorerPid;
    }
    else if (s_lastShellPid != 0)
    {
        if (s_lastDeathTickMs != 0 && (now - s_lastDeathTickMs) < kRapidDeathWindowMs)
        {
            ArmCrashPrompt(now);
            s_lastDeathTickMs = 0;
            s_lastShellPid = 0;
        }
        else
        {
            s_lastDeathTickMs = now;
            s_lastShellPid = 0;
        }
    }

    if (ReadExplorerCrashSeriesCount() >= kCrashSeriesThreshold)
        ArmCrashPrompt(now);
}
