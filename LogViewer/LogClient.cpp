#include "SharedPch.h"

#include "framework.h"
#include "LogClient.h"
#include "UiConsts.h"
#include "Ui/Messages/LogViewerAppMessages.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

static HWND g_hwndTarget = nullptr;

// ---------------------------------------------------------------------------
// Очередь LogEntry между пайп-потоком и GUI
// ---------------------------------------------------------------------------

static std::mutex              g_entryQueueMutex;
static std::deque<LogEntry>    g_pendingEntries;
static std::atomic<std::uint64_t> g_droppedEntries{0};

static void ScheduleLogDrain() noexcept
{
    if (!g_hwndTarget || !IsWindow(g_hwndTarget))
        return;
    (void)SetTimer(g_hwndTarget, IDT_LOG_DRAIN, kLogDrainCoalesceMs, nullptr);
}

static void EnqueueLogEntryFromPipeThread(LogEntry&& e)
{
    std::lock_guard<std::mutex> lock(g_entryQueueMutex);
    while (g_pendingEntries.size() >= kMaxPendingLogEntries)
    {
        g_pendingEntries.pop_front();
        g_droppedEntries.fetch_add(1, std::memory_order_relaxed);
    }
    const bool wasEmpty = g_pendingEntries.empty();
    g_pendingEntries.push_back(std::move(e));
    if (wasEmpty)
        ScheduleLogDrain();
}

std::vector<LogEntry> DrainLogEntryQueueUpTo(std::size_t maxEntries)
{
    std::vector<LogEntry> result;
    std::lock_guard<std::mutex> lock(g_entryQueueMutex);
    const std::size_t n = (std::min)(maxEntries, g_pendingEntries.size());
    result.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        result.push_back(std::move(g_pendingEntries.front()));
        g_pendingEntries.pop_front();
    }
    return result;
}

void RequeueLogEntriesFront(std::vector<LogEntry>&& entries)
{
    if (entries.empty())
        return;
    std::lock_guard<std::mutex> lock(g_entryQueueMutex);
    g_pendingEntries.insert(g_pendingEntries.begin(),
        std::make_move_iterator(entries.begin()),
        std::make_move_iterator(entries.end()));
    entries.clear();
}

std::uint64_t ConsumeLogQueueDroppedCount() noexcept
{
    return g_droppedEntries.exchange(0, std::memory_order_acq_rel);
}

bool HasPendingLogEntries() noexcept
{
    std::lock_guard<std::mutex> lock(g_entryQueueMutex);
    return !g_pendingEntries.empty();
}

// ---------------------------------------------------------------------------
// Фоновое разрешение имён PID (OpenProcess вне GUI-потока)
// ---------------------------------------------------------------------------

static std::mutex                     g_pidQueueMutex;
static std::condition_variable        g_pidCv;
static std::deque<uint32_t>           g_pidQueue;
static std::unordered_set<uint32_t>   g_pidQueued;
static std::atomic<bool>              g_pidWorkerStop{false};
static std::thread                    g_pidWorker;

static std::mutex                                      g_pidResultMutex;
static std::vector<std::pair<uint32_t, std::wstring>> g_pidResults;
static std::atomic<bool>                               g_pidFlushPosted{false};

[[nodiscard]] static std::wstring ResolveProcessName(uint32_t pid)
{
    wchar_t exeName[MAX_PATH] = L"unknown";
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc)
    {
        DWORD sz = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, exeName, &sz))
        {
            if (wchar_t* sl = wcsrchr(exeName, L'\\'))
                wcscpy_s(exeName, sl + 1);
        }
        CloseHandle(hProc);
    }
    wchar_t buf[MAX_PATH + 32];
    swprintf_s(buf, L"%s (%u)", exeName, pid);
    return std::wstring(buf);
}

static void PidWorkerThreadProc()
{
    using namespace std::chrono_literals;

    for (;;)
    {
        uint32_t pid = 0;
        bool have = false;
        {
            std::unique_lock<std::mutex> lock(g_pidQueueMutex);
            g_pidCv.wait_for(lock, 200ms, [] {
                return g_pidWorkerStop.load(std::memory_order_relaxed) || !g_pidQueue.empty();
            });
            if (g_pidWorkerStop.load(std::memory_order_relaxed) && g_pidQueue.empty())
                return;
            if (g_pidQueue.empty())
                continue;
            pid = g_pidQueue.front();
            g_pidQueue.pop_front();
            g_pidQueued.erase(pid);
            have = true;
        }

        if (!have)
            continue;

        std::wstring label = ResolveProcessName(pid);
        {
            std::lock_guard<std::mutex> rl(g_pidResultMutex);
            g_pidResults.emplace_back(pid, std::move(label));
        }
        if (!g_pidFlushPosted.exchange(true, std::memory_order_acq_rel))
        {
            if (g_hwndTarget && IsWindow(g_hwndTarget))
                (void)PostMessageW(g_hwndTarget, WM_APP_PID_LABELS_FLUSH, 0, 0);
        }

        std::this_thread::sleep_for(10ms);
    }
}

void EnqueuePidForLabelResolution(uint32_t processId) noexcept
{
    try
    {
        {
            std::lock_guard<std::mutex> lock(g_pidQueueMutex);
            if (g_pidQueued.contains(processId))
                return;
            g_pidQueued.insert(processId);
            g_pidQueue.push_back(processId);
        }
        g_pidCv.notify_one();
    }
    catch (...)
    {
    }
}

void TakePendingPidLabelResults(std::vector<std::pair<uint32_t, std::wstring>>& out)
{
    out.clear();
    std::lock_guard<std::mutex> lock(g_pidResultMutex);
    out.swap(g_pidResults);
    g_pidFlushPosted.store(false, std::memory_order_release);
}

void StopLogClientSupportThreads() noexcept
{
    g_pidWorkerStop.store(true, std::memory_order_release);
    g_pidCv.notify_all();
    if (g_pidWorker.joinable())
        g_pidWorker.join();
    g_pidWorker = std::thread();
}

static void EnsurePidWorkerStarted()
{
    if (g_pidWorker.joinable())
        return;
    g_pidWorkerStop.store(false, std::memory_order_release);
    g_pidWorker = std::thread(PidWorkerThreadProc);
}

// ---------------------------------------------------------------------------
// Поток чтения пайпа
// ---------------------------------------------------------------------------

DWORD WINAPI LogClientThread(LPVOID)
{
    constexpr DWORD kBufSize = 256 * 1024;
    BYTE* buf = new BYTE[kBufSize];

    while (true)
    {
        HANDLE pipe = CreateFileW(
            L"\\\\.\\pipe\\GlassLog",
            GENERIC_READ,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr
        );

        if (pipe == INVALID_HANDLE_VALUE)
        {
            Sleep(1000);
            continue;
        }

        PostMessageW(g_hwndTarget, WM_LOG_CONNECTED, 1, 0);

        while (true)
        {
            DWORD bytesRead = 0;
            BOOL ok = ReadFile(pipe, buf, kBufSize, &bytesRead, nullptr);

            if (!ok || bytesRead < sizeof(LogPacket))
                break;

            const LogPacket* src = reinterpret_cast<const LogPacket*>(buf);
            const size_t minSize = sizeof(LogPacket) + src->length * sizeof(wchar_t);
            if (bytesRead < minSize || src->length == 0)
                continue;

            LogEntry e{};
            e.processId = src->processId;
            e.threadId = src->threadId;

            if (src->level == LOG_LEVEL_DISCONNECT)
            {
                e.level = LOG_LEVEL_DISCONNECT;
                e.threadId = 0;
            }
            else
            {
                e.level = src->level < 5 ? src->level : 0;
                if (src->length > 1)
                    e.text.assign(src->text, src->length - 1);
                ParseLogTags(e.text, e.module, e.subsystem);
            }

            EnqueueLogEntryFromPipeThread(std::move(e));
        }

        CloseHandle(pipe);
        PostMessageW(g_hwndTarget, WM_LOG_CONNECTED, 0, 0);
    }

    delete[] buf;
    return 0;
}

void StartLogClientThread(HWND hwndTarget)
{
    g_hwndTarget = hwndTarget;
    EnsurePidWorkerStarted();
    CreateThread(nullptr, 0, LogClientThread, nullptr, 0, nullptr);
}
