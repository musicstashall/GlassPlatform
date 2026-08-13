#include "SharedPch.h"

#include "framework.h"
#include "GlassLogBridge.h"
#include <GlassLogProtocol.h>
#include "GlassLogServer.h"
#include <vector>

namespace Shared::Log
{
    GlassLogBridge& GlassLogBridge::Instance()
    {
        static GlassLogBridge g;
        return g;
    }

    GlassLogBridge::GlassLogBridge()
        : m_running(false)
    {
    }

    GlassLogBridge::~GlassLogBridge()
    {
        Stop();
    }

    void GlassLogBridge::Start()
    {
#if !defined(GLASS_ENABLE_LOGGING)
        return;
#else
        bool expected = false;
        if (!m_running.compare_exchange_strong(expected, true))
            return;

        m_acceptThread = std::thread(&GlassLogBridge::AcceptThreadProc, this);
#endif
    }

    void GlassLogBridge::Stop()
    {
#if !defined(GLASS_ENABLE_LOGGING)
        return;
#else
        bool expected = true;
        if (!m_running.compare_exchange_strong(expected, false))
            return;

        // Разбудить AcceptThreadProc, если он висит в ConnectNamedPipe
        HANDLE h = CreateFileW(
            kLogPipeName,
            GENERIC_WRITE,
            0, nullptr,
            OPEN_EXISTING,
            0, nullptr
        );
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);

        if (m_acceptThread.joinable())
            m_acceptThread.join();
#endif
    }

    struct ClientContext
    {
        HANDLE pipe;
        DWORD  processId;
    };

    DWORD WINAPI GlassLogBridge::ClientThreadProc(LPVOID param)
    {
#if !defined(GLASS_ENABLE_LOGGING)
        return 0;
#else
        auto* ctx = reinterpret_cast<ClientContext*>(param);
        HANDLE pipe = ctx->pipe;
        DWORD  clientPid = ctx->processId;
        delete ctx;

        std::vector<std::uint8_t> buffer(64 * 1024);

        for (;;)
        {
            DWORD bytesRead = 0;
            BOOL ok = ReadFile(
                pipe,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &bytesRead,
                nullptr
            );

            if (!ok || bytesRead == 0)
                break;

            if (bytesRead < sizeof(MessageHeader))
                continue;

            const MessageHeader* header =
                reinterpret_cast<const MessageHeader*>(buffer.data());

            if (header->size < sizeof(MessageHeader) ||
                header->size > bytesRead)
            {
                continue;
            }

            const std::uint8_t* payload =
                buffer.data() + sizeof(MessageHeader);
            const std::uint32_t payloadSize =
                header->size - static_cast<std::uint32_t>(sizeof(MessageHeader));

            MessageType type = static_cast<MessageType>(header->type);

            if (type == MessageType::LogLine)
            {
                int level = DecodeLevel(header->flags);
                DWORD clientTid = header->threadId;

                std::wstring text;

                if (header->flags & LogFlag_WideChar)
                {
                    const wchar_t* w = reinterpret_cast<const wchar_t*>(payload);
                    std::size_t count = payloadSize / sizeof(wchar_t);
                    if (count > 0)
                        text.assign(w, w + count - 1);
                }
                else if (header->flags & LogFlag_Utf8)
                {
                    const char* c = reinterpret_cast<const char*>(payload);
                    int len = static_cast<int>(payloadSize);
                    if (len > 0)
                    {
                        int wlen = MultiByteToWideChar(CP_UTF8, 0, c, len, nullptr, 0);
                        if (wlen > 0)
                        {
                            text.resize(wlen);
                            MultiByteToWideChar(CP_UTF8, 0, c, len, text.data(), wlen);
                            if (!text.empty() && text.back() == L'\0')
                                text.pop_back();
                        }
                    }
                }

                if (!text.empty())
                {
                    GlassLogServer::Instance().WriteLine(
                        static_cast<std::uint32_t>(level),
                        clientPid,
                        clientTid,
                        text.c_str()
                    );
                }
            }
            else if (type == MessageType::Shutdown)
            {
                break;
            }
        }

        GlassLogServer::Instance().WriteLine(
            LOG_LEVEL_DISCONNECT, clientPid, 0, L"disconnected"
        );

        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        return 0;
#endif
    }

    void GlassLogBridge::AcceptThreadProc()
    {
#if !defined(GLASS_ENABLE_LOGGING)
        return;
#else
        while (m_running.load())
        {
            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = FALSE;

            PSECURITY_DESCRIPTOR sd =
                (PSECURITY_DESCRIPTOR)LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
            if (!sd)
            {
                Sleep(500);
                continue;
            }

            if (!InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION) ||
                !SetSecurityDescriptorDacl(sd, TRUE, nullptr, FALSE))
            {
                LocalFree(sd);
                Sleep(500);
                continue;
            }

            sa.lpSecurityDescriptor = sd;

            HANDLE pipe = CreateNamedPipeW(
                kLogPipeName,
                PIPE_ACCESS_INBOUND,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                64 * 1024,
                64 * 1024,
                0,
                &sa
            );

            LocalFree(sd);

            if (pipe == INVALID_HANDLE_VALUE)
            {
                Sleep(500);
                continue;
            }

            BOOL connected = ConnectNamedPipe(pipe, nullptr)
                ? TRUE
                : (GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);

            if (!connected)
            {
                CloseHandle(pipe);
                continue;
            }

            if (!m_running.load())
            {
                FlushFileBuffers(pipe);
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
                break;
            }

            DWORD clientPid = 0;
            GetNamedPipeClientProcessId(pipe, &clientPid);

            auto* ctx = new ClientContext{ pipe, clientPid };

            HANDLE hThread = CreateThread(nullptr, 0, ClientThreadProc, ctx, 0, nullptr);

            if (hThread)
                CloseHandle(hThread);
            else
            {
                delete ctx;
                FlushFileBuffers(pipe);
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
            }
        }
#endif
    }
}
