#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include "LogPacket.h"   // LogPacket, LOG_LEVEL_DISCONNECT

namespace Shared::Log
{
    inline constexpr wchar_t kLogViewerPipeName[] = L"\\\\.\\pipe\\GlassLog";

    class GlassLogServer
    {
    public:
        static GlassLogServer& Instance();

        bool Start();
        void Stop();

        // Не блокируется: кладёт в очередь и немедленно возвращается
        void WriteLine(
            std::uint32_t level,
            std::uint32_t processId,
            std::uint32_t threadId,
            const wchar_t* text
        );

        // Строки, выброшенные из входной очереди при достижении kMaxQueueSize (WriteLine).
        [[nodiscard]] std::uint64_t ConsumeInboundDroppedCount() noexcept;

        bool IsReady() const { return m_ready.load(std::memory_order_acquire); }

    private:
        GlassLogServer();
        ~GlassLogServer();

        void AcceptThreadProc();
        void FlushThreadProc();
        bool CreatePipeInstance(HANDLE& outHandle);

        // Внутренний элемент очереди
        struct LogEntry
        {
            std::uint32_t level;
            std::uint32_t processId;
            std::uint32_t threadId;
            std::wstring  text;     // уже отформатированный текст с временной меткой
        };

    private:
        std::thread              m_acceptThread;
        std::thread              m_flushThread;
        std::atomic<bool>        m_running;
        std::atomic<bool>        m_ready;
        HANDLE                   m_pipe;
        std::mutex               m_pipeMutex;

        // Очередь для неблокирующей передачи логов в flush-поток (deque — возврат batch при отсутствии пайпа)
        std::deque<LogEntry>     m_queue;
        std::mutex               m_queueMutex;
        std::condition_variable  m_queueCv;

        static constexpr std::size_t kMaxQueueSize = 20'000;
    };
}
