#pragma once

#include <windows.h>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <chrono>
#include <vector>
#include <cstdint>
#include <cstring>

#include "GlassLogProtocol.h"

namespace Shared::Log
{
    namespace
    {
        // LogViewer: не ставить строки в именованный канал — иначе рекурсия (Core логирует в тот же процесс).
        [[nodiscard]] inline bool IsCurrentExeLogViewerProcess() noexcept
        {
            static std::atomic<int> s_st{ -1 };
            const int v = s_st.load(std::memory_order_relaxed);
            if (v >= 0)
                return v != 0;
            wchar_t path[MAX_PATH]{};
            const DWORD n = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
            bool yes = false;
            if (n > 0)
            {
                const wchar_t* base = path;
                for (wchar_t* p = path; *p; ++p)
                    if (*p == L'/' || *p == L'\\')
                        base = p + 1;
                yes = (_wcsicmp(base, L"logviewer.exe") == 0);
            }
            s_st.store(yes ? 1 : 0, std::memory_order_relaxed);
            return yes;
        }
    }

    class GlassLogClient
    {
    public:
        static GlassLogClient& Instance()
        {
            static GlassLogClient g;
            return g;
        }

        // Возвращает true, если Stop() уже был вызван (необратимо)
        static bool IsStopped()
        {
            return Instance().m_stopped.load(std::memory_order_acquire);
        }

        void LogLine(const std::wstring& text, int level)
        {
#if !defined(_DEBUG) || !defined(GLASS_ENABLE_LOGGING)
            (void)text;
            (void)level;
            return;
#else
            if (text.empty())
                return;

            // Вывод в отладчик уже делает DebugLog.h (DebugLogImpl до SendLog и wstring-
            // перегрузка после). Повторный ODS здесь давал каждую строку дважды в Output.
            if (IsCurrentExeLogViewerProcess())
                return;

            // После необратимой остановки — только OutputDebugStringW, без pipe-потока
            if (m_stopped.load(std::memory_order_acquire))
                return;

            EnsureWorkerStarted();

            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_queue.push_back({ text, level, ::GetCurrentThreadId() });
            }
            m_queueCv.notify_one();
#endif
        }

        static void Write(const std::wstring& text, int level = 0)
        {
            Instance().LogLine(text, level);
        }

        // Явная остановка с GUI / watcher-потока: дождаться воркера.
        void Stop()
        {
            StopImpl(/*joinWorker*/ true);
        }

    private:
        GlassLogClient()
            : m_running(false)
            , m_pipe(INVALID_HANDLE_VALUE)
        {
        }

        ~GlassLogClient()
        {
            // join() из деструктора / CRT teardown / DllMain DETACH даёт deadlock
            // или std::terminate → abort()-диалог Debug CRT при reboot.
            StopImpl(/*joinWorker*/ false);
        }

        void StopImpl(bool joinWorker)
        {
#if defined(_DEBUG) && defined(GLASS_ENABLE_LOGGING)
            m_stopped.store(true, std::memory_order_release);

            bool expected = true;
            if (m_running.compare_exchange_strong(expected, false))
                m_queueCv.notify_all();

            if (m_worker.joinable())
            {
                if (joinWorker)
                    m_worker.join();
                else
                    m_worker.detach();
            }

            if (joinWorker)
                ClosePipe();
#else
            (void)joinWorker;
#endif
        }

        struct PendingItem
        {
            std::wstring text;
            int          level;
            DWORD        sourceThreadId; // поток, вызвавший DebugLog (не воркер очереди)
        };

        void EnsureWorkerStarted()
        {
            // Проверяем необратимый флаг остановки первым — до m_running
            if (m_stopped.load(std::memory_order_acquire))
                return;

            bool expected = false;
            if (!m_running.compare_exchange_strong(expected, true))
                return;

            m_worker = std::thread(&GlassLogClient::WorkerThreadProc, this);
        }

        void WorkerThreadProc()
        {
            for (;;)
            {
                PendingItem item;

                {
                    std::unique_lock<std::mutex> lock(m_queueMutex);
                    m_queueCv.wait(lock, [this]() {
                        return !m_running.load(std::memory_order_acquire) || !m_queue.empty();
                        });

                    if (!m_running.load(std::memory_order_acquire) && m_queue.empty())
                        break;

                    if (m_queue.empty())
                        continue;

                    item = std::move(m_queue.front());
                    m_queue.pop_front();
                }

                if (!EnsureConnected())
                {
                    // При Stop() EnsureConnected сразу возвращает false. Если снова положить
                    // элемент в очередь, условие выхода (!m_running && empty) никогда не выполнится —
                    // join() в Stop() зависнет (без LogViewer канал не открывается).
                    if (m_running.load(std::memory_order_acquire))
                    {
                        {
                            std::lock_guard<std::mutex> lock(m_queueMutex);
                            m_queue.push_front(std::move(item));
                        }
                        m_queueCv.notify_one();
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    continue;
                }

                std::wstring payload = item.text;
                if (payload.empty() || payload.back() != L'\0')
                    payload.push_back(L'\0');

                MessageHeader header{};
                header.size = static_cast<std::uint32_t>(
                    sizeof(MessageHeader) + payload.size() * sizeof(wchar_t)
                    );
                header.type = static_cast<std::uint16_t>(MessageType::LogLine);
                header.flags = static_cast<std::uint16_t>(
                    LogFlag_WideChar | EncodeLevel(item.level)
                    );
                header.threadId = item.sourceThreadId;

                const size_t totalSize = sizeof(MessageHeader) + payload.size() * sizeof(wchar_t);

                std::vector<std::uint8_t> buffer(totalSize);
                std::memcpy(buffer.data(), &header, sizeof(header));
                std::memcpy(buffer.data() + sizeof(header),
                    payload.data(),
                    payload.size() * sizeof(wchar_t));

                DWORD written = 0;
                BOOL ok = ::WriteFile(
                    m_pipe,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &written,
                    nullptr
                );

                if (!ok || written != buffer.size())
                {
                    ClosePipe();
                    if (m_running.load(std::memory_order_acquire))
                    {
                        {
                            std::lock_guard<std::mutex> lock(m_queueMutex);
                            m_queue.push_front(std::move(item));
                        }
                        m_queueCv.notify_one();
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                }
            }

            ClosePipe();
        }

        bool EnsureConnected()
        {
            if (m_pipe != INVALID_HANDLE_VALUE)
                return true;

            constexpr int   kMaxAttempts = 50;
            constexpr DWORD kDelayMs = 100;
            constexpr DWORD kSeriesPauseMs = 500;

            for (;;)
            {
                if (!m_running.load(std::memory_order_acquire))
                    return false;

                bool connected = false;

                for (int i = 0; i < kMaxAttempts; ++i)
                {
                    // Проверяем флаг остановки до попытки подключения,
                    // чтобы не блокировать выгрузку DLL на ~5 секунд.
                    if (!m_running.load(std::memory_order_acquire))
                        return false;

                    HANDLE h = ::CreateFileW(
                        kLogPipeName,
                        GENERIC_WRITE,
                        0,
                        nullptr,
                        OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL,
                        nullptr
                    );

                    if (h != INVALID_HANDLE_VALUE)
                    {
                        m_pipe = h;
                        connected = true;
                        break;
                    }

                    DWORD err = ::GetLastError();
                    if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND)
                        break;

                    ::Sleep(kDelayMs);
                }

                if (connected)
                    return true;

                ::Sleep(kSeriesPauseMs);

                if (!m_running.load(std::memory_order_acquire))
                    return false;
            }
        }

        void ClosePipe()
        {
            if (m_pipe != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(m_pipe);
                m_pipe = INVALID_HANDLE_VALUE;
            }
        }

    private:
        std::atomic<bool>        m_stopped{false}; // необратимый флаг: true после Stop()
        std::atomic<bool>        m_running;
        std::thread              m_worker;

        HANDLE                   m_pipe;

        std::mutex               m_queueMutex;
        std::condition_variable  m_queueCv;
        std::deque<PendingItem>  m_queue;
    };

    inline void SendLog(const std::wstring& text, int level)
    {
        if (GlassLogClient::IsStopped())
            return;
        GlassLogClient::Instance().LogLine(text, level);
    }
}
