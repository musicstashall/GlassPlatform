#pragma once

#include <OpenGlassHostState.h>
#include <PlatformPresenceState.h>

namespace Shared::PlatformPresenceQuery
{
    [[nodiscard]] bool IsHostPresenceAvailable() noexcept;

    [[nodiscard]] PlatformPresenceState::PresenceUiState QueryOpenGlassOrchestration() noexcept;

    [[nodiscard]] OpenGlassHostState::HostUiState MapToHostUiState(
        PlatformPresenceState::PresenceUiState state) noexcept;

    [[nodiscard]] OpenGlassHostState::HostUiState QueryOpenGlassOrchestrationHostUiState() noexcept;

    [[nodiscard]] bool IsOpenGlassPipelineTransitioning() noexcept;
}
