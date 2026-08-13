#pragma once
#include <atomic>
#include <thread>
#include <vector>
#include <Windows.h>

namespace Shared::Log
{
    class GlassLogBridge
    {
    public:
        static GlassLogBridge& Instance();

        void Start();
        void Stop();

    private:
        GlassLogBridge();
        ~GlassLogBridge();

        void AcceptThreadProc();
        static DWORD WINAPI ClientThreadProc(LPVOID param);

    private:
        std::atomic<bool> m_running;
        std::thread       m_acceptThread;
    };
}
