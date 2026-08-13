#include "SharedPch.h"
#include <PlatformPresenceQuery.h>

namespace Shared::PlatformPresenceQuery
{
    bool IsHostPresenceAvailable() noexcept
    {
        PlatformPresenceState::Snapshot snap{};
        return PlatformPresenceState::TryReadSnapshot(snap) && snap.revision != 0;
    }

    PlatformPresenceState::PresenceUiState QueryOpenGlassOrchestration() noexcept
    {
        PlatformPresenceState::Snapshot snap{};
        if (!PlatformPresenceState::TryReadSnapshot(snap) || snap.revision == 0)
            return PlatformPresenceState::PresenceUiState::NoInstall;
        return snap.openGlassOrchestration;
    }

    OpenGlassHostState::HostUiState MapToHostUiState(
        PlatformPresenceState::PresenceUiState state) noexcept
    {
        switch (state)
        {
        case PlatformPresenceState::PresenceUiState::Running:
            return OpenGlassHostState::HostUiState::Running;
        case PlatformPresenceState::PresenceUiState::Ready:
            return OpenGlassHostState::HostUiState::Ready;
        case PlatformPresenceState::PresenceUiState::Disabled:
            return OpenGlassHostState::HostUiState::Disabled;
        default:
            return OpenGlassHostState::HostUiState::NoTask;
        }
    }

    OpenGlassHostState::HostUiState QueryOpenGlassOrchestrationHostUiState() noexcept
    {
        return MapToHostUiState(QueryOpenGlassOrchestration());
    }

    bool IsOpenGlassPipelineTransitioning() noexcept
    {
        PlatformPresenceState::Snapshot snap{};
        if (!PlatformPresenceState::TryReadSnapshot(snap) || snap.revision == 0)
            return false;
        const auto phase = snap.openGlassPipelinePhase;
        return phase == PlatformPresenceState::PipelinePhase::Starting
            || phase == PlatformPresenceState::PipelinePhase::Stopping;
    }
}
