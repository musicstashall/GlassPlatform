#include "SharedPch.h"
#include <PlatformPresenceMonitor.h>
#include <PlatformPresenceState.h>
#include <OpenGlassHostCommand.h>
#include <RootPolicyQuery.h>
#include <OpenGlassDwmPolicy.h>
#include <OpenGlassHostState.h>
#include <GlassPlatformTask.h>
#include <ReloadRenderFlags.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace Shared::PlatformPresenceMonitor
{
    namespace
    {
        using PlatformPresenceState::PipelinePhase;
        using PlatformPresenceState::PresenceUiState;
        using PlatformPresenceState::Snapshot;

        std::atomic<bool> g_running{ false };
        std::atomic<bool> g_refreshRequested{ false };
        HANDLE g_thread = nullptr;
        HANDLE g_stopEvent = nullptr;

        PipelinePhase g_pipelinePhase = PipelinePhase::Idle;
        ULONGLONG g_pipelinePhaseSinceMs = 0;

        [[nodiscard]] PresenceUiState MapGlassAutorunState() noexcept
        {
            using GlassShared::GlassPlatformTask::AutorunUiState;
            switch (GlassShared::GlassPlatformTask::QueryGlassAutorunUiState())
            {
            case AutorunUiState::Running:
                return PresenceUiState::Running;
            case AutorunUiState::Ready:
                return PresenceUiState::Ready;
            case AutorunUiState::Disabled:
                return PresenceUiState::Disabled;
            default:
                return PresenceUiState::NoInstall;
            }
        }

        [[nodiscard]] PresenceUiState MapOgOrchestrationState() noexcept
        {
            switch (OpenGlassHostState::QueryOrchestrationUiState())
            {
            case OpenGlassHostState::HostUiState::Running:
                return PresenceUiState::Running;
            case OpenGlassHostState::HostUiState::Ready:
                return PresenceUiState::Ready;
            case OpenGlassHostState::HostUiState::Disabled:
                return PresenceUiState::Disabled;
            default:
                return PresenceUiState::NoInstall;
            }
        }

        [[nodiscard]] DWORD ComputeRootBackdropMode(bool pipelineActive) noexcept
        {
            if (RootPolicyQuery::ReadNoBackdropFromRegistry() != 0)
                return 0u;
            if (pipelineActive)
                return 1u;
            return 2u;
        }

        void UpdatePipelinePhase(PresenceUiState orchestration, bool pipelineWindow) noexcept
        {
            const ULONGLONG now = GetTickCount64();
            const bool orchRunning = orchestration == PresenceUiState::Running;

            switch (g_pipelinePhase)
            {
            case PipelinePhase::Idle:
                if (orchRunning)
                {
                    g_pipelinePhase = PipelinePhase::Starting;
                    g_pipelinePhaseSinceMs = now;
                }
                else if (pipelineWindow)
                {
                    g_pipelinePhase = PipelinePhase::Active;
                    g_pipelinePhaseSinceMs = now;
                }
                break;

            case PipelinePhase::Starting:
                if (!orchRunning && !pipelineWindow)
                {
                    g_pipelinePhase = PipelinePhase::Idle;
                    g_pipelinePhaseSinceMs = now;
                }
                else if (pipelineWindow)
                {
                    g_pipelinePhase = PipelinePhase::Active;
                    g_pipelinePhaseSinceMs = now;
                }
                else if (!orchRunning)
                {
                    g_pipelinePhase = PipelinePhase::Idle;
                    g_pipelinePhaseSinceMs = now;
                }
                break;

            case PipelinePhase::Active:
                if (!orchRunning)
                {
                    g_pipelinePhase = PipelinePhase::Stopping;
                    g_pipelinePhaseSinceMs = now;
                }
                else if (!pipelineWindow)
                {
                    g_pipelinePhase = PipelinePhase::Starting;
                    g_pipelinePhaseSinceMs = now;
                }
                break;

            case PipelinePhase::Stopping:
                if (!pipelineWindow)
                {
                    g_pipelinePhase = PipelinePhase::Idle;
                    g_pipelinePhaseSinceMs = now;
                }
                else if (orchRunning)
                {
                    g_pipelinePhase = PipelinePhase::Active;
                    g_pipelinePhaseSinceMs = now;
                }
                break;
            }
        }

        [[nodiscard]] DWORD PollIntervalMs(PipelinePhase phase, PresenceUiState ogOrch) noexcept
        {
            if (phase == PipelinePhase::Starting || phase == PipelinePhase::Stopping)
                return 300;
            if (ogOrch == PresenceUiState::Running)
                return 2000;
            return 5000;
        }

        void ApplySnapshot(const PlatformPresenceState::Snapshot& snap, DWORD prevMode) noexcept
        {
            PlatformPresenceState::TryWriteSnapshot(snap, true);
            RootPolicyQuery::SetRootBackdropMode(snap.rootBackdropMode);
            RootPolicyQuery::RefreshRootBackdropPolicyCache();

            if (prevMode != snap.rootBackdropMode
                && GlassShared::GlassPlatformTask::IsGlassPlatformRunning())
            {
                (void)GlassShared::GlassPlatformTask::SignalReloadRender(
                    kReloadRenderFlagRootBackdropReapply | kReloadRenderFlagGlassCore);
            }
        }

        struct ComInitGuard
        {
            ComInitGuard() noexcept
            {
                hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                if (hr == RPC_E_CHANGED_MODE)
                    hr = S_OK;
            }

            ~ComInitGuard() noexcept
            {
                if (SUCCEEDED(hr) && hr != RPC_E_CHANGED_MODE)
                    CoUninitialize();
            }

            HRESULT hr = E_FAIL;
        };

        void MonitorLoop() noexcept
        {
            ComInitGuard com;
            (void)com;

            DWORD prevMode = 0xFFFFFFFFu;

            wil::unique_handle cmdWake{ OpenGlassHostCommand::OpenCommandWakeEvent() };
            HANDLE waitHandles[2] = { g_stopEvent, cmdWake.get() };
            const DWORD waitCount = cmdWake ? 2u : 1u;

            while (g_running.load(std::memory_order_acquire))
            {
                if (g_refreshRequested.exchange(false, std::memory_order_acq_rel))
                {
                }

                OpenGlassHostCommand::ProcessPending();

                const PresenceUiState glass = MapGlassAutorunState();
                const PresenceUiState ogOrch = MapOgOrchestrationState();
                const bool backdropPipeline =
                    OpenGlassDwmPolicy::IsOpenGlassBackdropPipelineActive();
                const bool hostProc = OpenGlassHostState::IsHostProcessRunning();

                UpdatePipelinePhase(ogOrch, backdropPipeline);

                Snapshot snap{};
                snap.glassState = glass;
                snap.openGlassOrchestration = ogOrch;
                snap.openGlassPipelinePhase = g_pipelinePhase;
                snap.openGlassPipelineActive = backdropPipeline ? 1u : 0u;
                snap.openGlassHostProcessRunning = hostProc ? 1u : 0u;
                snap.openGlassBackend = static_cast<std::int32_t>(OpenGlassHostState::QueryActiveBackend());
                snap.rootBackdropMode = ComputeRootBackdropMode(backdropPipeline);

                ApplySnapshot(snap, prevMode);
                prevMode = snap.rootBackdropMode;

                const DWORD waitMs = PollIntervalMs(g_pipelinePhase, ogOrch);
                if (waitCount == 2u)
                {
                    if (WaitForMultipleObjects(waitCount, waitHandles, FALSE, waitMs) == WAIT_OBJECT_0)
                        break;
                }
                else if (WaitForSingleObject(g_stopEvent, waitMs) == WAIT_OBJECT_0)
                {
                    break;
                }
            }
        }

        DWORD WINAPI MonitorThreadProc(LPVOID) noexcept
        {
            MonitorLoop();
            return 0;
        }

        void PublishRootBackdropModeImpl(DWORD mode) noexcept
        {
            Snapshot snap{};
            DWORD prevMode = 0xFFFFFFFFu;
            if (PlatformPresenceState::TryReadSnapshot(snap) && snap.revision != 0)
            {
                prevMode = snap.rootBackdropMode;
            }
            else
            {
                snap.glassState = MapGlassAutorunState();
                snap.openGlassOrchestration = MapOgOrchestrationState();
                snap.openGlassPipelinePhase = g_pipelinePhase;
                const bool backdropPipeline =
                    OpenGlassDwmPolicy::IsOpenGlassBackdropPipelineActive();
                snap.openGlassPipelineActive = backdropPipeline ? 1u : 0u;
                snap.openGlassHostProcessRunning =
                    OpenGlassHostState::IsHostProcessRunning() ? 1u : 0u;
                snap.openGlassBackend =
                    static_cast<std::int32_t>(OpenGlassHostState::QueryActiveBackend());
            }

            snap.rootBackdropMode = mode;
            ApplySnapshot(snap, prevMode);
        }
    } // namespace

    bool Start() noexcept
    {
        if (g_running.load(std::memory_order_acquire))
            return true;

        g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!g_stopEvent)
            return false;

        g_pipelinePhase = PipelinePhase::Idle;
        g_pipelinePhaseSinceMs = GetTickCount64();
        g_running.store(true, std::memory_order_release);

        g_thread = CreateThread(nullptr, 0, MonitorThreadProc, nullptr, 0, nullptr);
        if (!g_thread)
        {
            g_running.store(false, std::memory_order_release);
            CloseHandle(g_stopEvent);
            g_stopEvent = nullptr;
            return false;
        }

        ComPush::TryRegisterScmPush();

        return true;
    }

    void Stop() noexcept
    {
        if (!g_running.exchange(false, std::memory_order_acq_rel))
            return;

        if (g_stopEvent)
            SetEvent(g_stopEvent);

        if (g_thread)
        {
            WaitForSingleObject(g_thread, 15000);
            CloseHandle(g_thread);
            g_thread = nullptr;
        }

        if (g_stopEvent)
        {
            CloseHandle(g_stopEvent);
            g_stopEvent = nullptr;
        }
    }

    bool IsRunning() noexcept
    {
        return g_running.load(std::memory_order_acquire);
    }

    void RequestRefresh() noexcept
    {
        g_refreshRequested.store(true, std::memory_order_release);
    }

    void PublishRootBackdropMode(DWORD mode) noexcept
    {
        PublishRootBackdropModeImpl(mode);
    }
}
