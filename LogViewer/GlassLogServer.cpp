#include "SharedPch.h"

#include "framework.h"
#include "GlassLogServer.h"
#include <vector>

static void DbgOutServer(const wchar_t* msg)
{
    OutputDebugStringW(L"[GlassLogServer] ");
    OutputDebugStringW(msg);
    OutputDebugStringW(L"\n");
}

namespace
{
    // WriteLine: отброс самой старой строки при переполнении m_queue (kMaxQueueSize).
    std::atomic<std::uint64_t> g_inboundDroppedDueToCapacity{};
}

namespace Shared::Log
{
    GlassLogServer& GlassLogServer::Instance()
    {
        static GlassLogServer g_instance;
        return g_instance;
    }

    GlassLogServer::GlassLogServer()
        : m_running(false)
        , m_ready(false)
        , m_pipe(INVALID_HANDLE_VALUE)
    {
    }

    GlassLogServer::~GlassLogServer()
    {
        Stop();
    }

    bool GlassLogServer::Start()
    {
#if !defined(GLASS_ENABLE_LOGGING)
        return true;
#else
        DbgOutServer(L"Start enter");
        bool expected = false;
        if (!m_running.compare_exchange_strong(expected, true))
        {
            DbgOutServer(L"Start: already running");
            return true;
        }

        m_ready.store(false, std::memory_order_release);

        DbgOutServer(L"Start: launching threads");
        m_acceptThread = std::thread(&GlassLogServer::AcceptThreadProc, this);
        m_flushThread  = std::thread(&GlassLogServer::FlushThreadProc, this);

        // Не блокируемся здесь в ожидании клиента: LogClientThread стартует позже.
        // Пайп будет готов к приёму в фоновом потоке AcceptThreadProc.

        DbgOutServer(L"Start: done (non-blocking)");
        return true;
#endif
    }

    void GlassLogServer::Stop()
    {
#if !defined(GLASS_ENABLE_LOGGING)
        return;
#else
        bool expected = true;
        if (!m_running.compare_exchange_strong(expected, false))
            return;

        m_queueCv.notify_all();

        if (m_flushThread.joinable())
            m_flushThread.join();

        if (m_acceptThread.joinable())
            m_acceptThread.join();

        {
            std::lock_guard<std::mutex> lock(m_pipeMutex);
            if (m_pipe != INVALID_HANDLE_VALUE)
            {
                ::FlushFileBuffers(m_pipe);
                ::DisconnectNamedPipe(m_pipe);
                ::CloseHandle(m_pipe);
                m_pipe = INVALID_HANDLE_VALUE;
            }
        }

        m_ready.store(false, std::memory_order_release);
#endif
    }

    bool GlassLogServer::CreatePipeInstance(HANDLE& outHandle)
    {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;

        PSECURITY_DESCRIPTOR sd =
            (PSECURITY_DESCRIPTOR)LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
        if (!sd)
            return false;

        if (!InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION))
        {
            LocalFree(sd);
            return false;
        }

        if (!SetSecurityDescriptorDacl(sd, TRUE, nullptr, FALSE))
        {
            LocalFree(sd);
            return false;
        }

        sa.lpSecurityDescriptor = sd;

        HANDLE pipe = CreateNamedPipeW(
            kLogViewerPipeName,
            PIPE_ACCESS_OUTBOUND,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1,
            256 * 1024,
            256 * 1024,
            0,
            &sa
        );

        LocalFree(sd);

        if (pipe == INVALID_HANDLE_VALUE)
            return false;

        outHandle = pipe;
        return true;
    }

    void GlassLogServer::AcceptThreadProc()
    {
#if !defined(GLASS_ENABLE_LOGGING)
        return;
#else
        DbgOutServer(L"AcceptThreadProc: started");
        while (m_running.load())
        {
            HANDLE pipe = INVALID_HANDLE_VALUE;
            if (!CreatePipeInstance(pipe))
            {
                DbgOutServer(L"AcceptThreadProc: CreatePipeInstance failed, retry");
                Sleep(500);
                continue;
            }

            DbgOutServer(L"AcceptThreadProc: waiting for client (ConnectNamedPipe)");
            BOOL connected = ::ConnectNamedPipe(pipe, nullptr)
                ? TRUE
                : (::GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);

            if (!connected)
            {
                DbgOutServer(L"AcceptThreadProc: ConnectNamedPipe failed");
                ::CloseHandle(pipe);
                continue;
            }
            DbgOutServer(L"AcceptThreadProc: client connected");

            {
                std::lock_guard<std::mutex> lock(m_pipeMutex);
                if (m_pipe != INVALID_HANDLE_VALUE)
                {
                    ::FlushFileBuffers(m_pipe);
                    ::DisconnectNamedPipe(m_pipe);
                    ::CloseHandle(m_pipe);
                }
                m_pipe = pipe;
            }

            m_ready.store(true, std::memory_order_release);
            m_queueCv.notify_all();

            while (m_running.load())
            {
                DWORD bytes = 0;
                BOOL ok = ::PeekNamedPipe(pipe, nullptr, 0, nullptr, &bytes, nullptr);

                if (!ok)
                {
                    DWORD err = ::GetLastError();
                    if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA)
                        break;
                    Sleep(100);
                    continue;
                }

                Sleep(100);
            }

            {
                std::lock_guard<std::mutex> lock(m_pipeMutex);
                if (m_pipe == pipe)
                {
                    ::FlushFileBuffers(m_pipe);
                    ::DisconnectNamedPipe(m_pipe);
                    ::CloseHandle(m_pipe);
                    m_pipe = INVALID_HANDLE_VALUE;
                }
            }
        }
#endif
    }

    void GlassLogServer::FlushThreadProc()
    {
#if !defined(GLASS_ENABLE_LOGGING)
        return;
#else
        std::vector<LogEntry> batch;
        batch.reserve(256);

        while (m_running.load(std::memory_order_acquire))
        {
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_queueCv.wait_for(lock, std::chrono::milliseconds(8), [this]() {
                    return !m_queue.empty()
                        || !m_running.load(std::memory_order_acquire);
                });

                while (!m_queue.empty())
                {
                    batch.push_back(std::move(m_queue.front()));
                    m_queue.pop_front();
                }
            }

            if (batch.empty())
                continue;

            std::lock_guard<std::mutex> pipeLock(m_pipeMutex);

            if (m_pipe == INVALID_HANDLE_VALUE)
            {
                // Пайпа нет — не терять batch, вернуть в голову очереди (порядок FIFO)
                std::lock_guard<std::mutex> ql(m_queueMutex);
                for (size_t i = batch.size(); i > 0; --i)
                    m_queue.push_front(std::move(batch[i - 1]));
                batch.clear();
                continue;
            }

            std::size_t sentCount = 0;
            for (; sentCount < batch.size(); ++sentCount)
            {
                LogEntry& e = batch[sentCount];
                const std::size_t len   = e.text.size() + 1;
                const std::size_t bytes = sizeof(LogPacket) + len * sizeof(wchar_t);

                BYTE* buffer = new BYTE[bytes];
                auto* packet = reinterpret_cast<LogPacket*>(buffer);

                packet->level     = e.level;
                packet->processId = e.processId;
                packet->threadId  = e.threadId;
                packet->length    = static_cast<std::uint32_t>(len);
                memcpy(packet->text, e.text.c_str(), len * sizeof(wchar_t));

                DWORD written = 0;
                BOOL ok = ::WriteFile(
                    m_pipe,
                    buffer,
                    static_cast<DWORD>(bytes),
                    &written,
                    nullptr
                );

                delete[] buffer;

                const bool complete = ok != FALSE
                    && written == static_cast<DWORD>(bytes);
                if (!complete)
                {
                    ::FlushFileBuffers(m_pipe);
                    ::DisconnectNamedPipe(m_pipe);
                    ::CloseHandle(m_pipe);
                    m_pipe = INVALID_HANDLE_VALUE;
                    // Неотправленная текущая и хвост батча — вернуть в очередь (FIFO).
                    {
                        std::lock_guard<std::mutex> ql(m_queueMutex);
                        for (std::size_t j = batch.size(); j > sentCount; --j)
                            m_queue.push_front(std::move(batch[j - 1]));
                    }
                    m_queueCv.notify_one();
                    break;
                }
            }

            batch.clear();
        }
#endif
    }

    void GlassLogServer::WriteLine(
        std::uint32_t level,
        std::uint32_t processId,
        std::uint32_t threadId,
        const wchar_t* text
    )
    {
#if !defined(GLASS_ENABLE_LOGGING)
        (void)level; (void)processId; (void)threadId; (void)text;
        return;
#else
        if (!text)
            return;

        SYSTEMTIME st{};
        ::GetLocalTime(&st);

        wchar_t prefix[64];
        swprintf_s(prefix, L"[%02d:%02d:%02d.%03d] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

        std::wstring out;
        out.reserve(wcslen(prefix) + wcslen(text) + 4);
        out += prefix;
        out += text;

        std::lock_guard<std::mutex> lock(m_queueMutex);

        if (m_queue.size() >= kMaxQueueSize)
        {
            m_queue.pop_front();
            g_inboundDroppedDueToCapacity.fetch_add(1, std::memory_order_relaxed);
        }

        const bool wasEmpty = m_queue.empty();
        m_queue.push_back({ level, processId, threadId, std::move(out) });

        if (wasEmpty)
            m_queueCv.notify_one();
#endif
    }

    std::uint64_t GlassLogServer::ConsumeInboundDroppedCount() noexcept
    {
        return g_inboundDroppedDueToCapacity.exchange(0, std::memory_order_acq_rel);
    }
}
