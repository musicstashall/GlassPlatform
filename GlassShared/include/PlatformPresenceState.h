#pragma once

#include <Windows.h>
#include <cstdint>

namespace Shared::PlatformPresenceState
{
    /// Состояние задачи/службы для UI (как GlassSettings PlatformSchedulerRow).
    enum class PresenceUiState : std::int32_t
    {
        NoInstall = 0,
        Disabled = 1,
        Ready = 2,
        Running = 3,
    };

    /// Фаза pipeline OpenGlass в DWM.
    enum class PipelinePhase : std::int32_t
    {
        Idle = 0,
        Starting = 1,
        Active = 2,
        Stopping = 3,
    };

    struct Snapshot
    {
        DWORD revision = 0;
        PresenceUiState glassState = PresenceUiState::NoInstall;
        PresenceUiState openGlassOrchestration = PresenceUiState::NoInstall;
        PipelinePhase openGlassPipelinePhase = PipelinePhase::Idle;
        DWORD rootBackdropMode = 2;
        DWORD openGlassPipelineActive = 0;
        DWORD openGlassHostProcessRunning = 0;
        /// Shared::OpenGlassHostState::OpenGlassBackend
        std::int32_t openGlassBackend = 0;
    };

    inline constexpr wchar_t kPresenceStateMappingName[] = L"Local\\GlassPlatform.PresenceState";
    inline constexpr wchar_t kPresenceChangedEventName[] = L"GlassPlatform.PresenceChanged";

    [[nodiscard]] bool CreateHostPresenceResources(SECURITY_ATTRIBUTES* sa) noexcept;
    void CloseHostPresenceResources() noexcept;

    [[nodiscard]] bool TryReadSnapshot(Snapshot& out) noexcept;
    [[nodiscard]] bool TryWriteSnapshot(const Snapshot& in, bool signalChanged) noexcept;

    [[nodiscard]] HANDLE OpenPresenceChangedEvent() noexcept;

    /// Разбудить подписчиков Presence (Settings Home / ShellExt busy).
    void PulsePresenceChanged() noexcept;
}
