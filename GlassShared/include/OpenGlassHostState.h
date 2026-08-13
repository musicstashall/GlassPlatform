#pragma once

#include <cstdint>

namespace Shared::OpenGlassHostState
{
    inline constexpr wchar_t kOpenGlassHostServiceName[] = L"OpenGlassHost";
    inline constexpr wchar_t kOpenGlassHostExeName[] = L"OpenGlassHost.exe";

    /// Состояние задачи планировщика OpenGlass Host (как GlassSettings / страница «Система»).
    enum class HostUiState : std::int32_t
    {
        NoTask = 0,
        Disabled = 1,
        Ready = 2,
        Running = 3,
    };

    enum class OpenGlassBackend : std::int32_t
    {
        None = 0,
        Task = 1,
        Service = 2,
    };

    [[nodiscard]] HostUiState QueryHostUiState() noexcept;

    [[nodiscard]] bool IsPipelineAffectingBorders() noexcept;

    [[nodiscard]] bool LegacyHostTaskExists() noexcept;

    /// Задача или служба OpenGlass зарегистрированы.
    [[nodiscard]] bool OrchestrationInstalled() noexcept;

    /// Агрегат task + service: NoInstall / Disabled / Ready / Running.
    [[nodiscard]] HostUiState QueryOrchestrationUiState() noexcept;

    /// Служба OpenGlassHost зарегистрирована.
    [[nodiscard]] bool ServiceInstalled() noexcept;

    /// SCM: служба RUNNING.
    [[nodiscard]] bool IsServiceRunning() noexcept;

    /// Автозапуск службы (SERVICE_AUTO_START).
    [[nodiscard]] bool IsServiceAutostartEnabled() noexcept;

    /// Ручной (false) или автоматический (true) запуск службы.
    [[nodiscard]] bool SetServiceAutostartEnabled(bool enable) noexcept;

    /// Процесс OpenGlassHost.exe в системе.
    [[nodiscard]] bool IsHostProcessRunning() noexcept;

    /// Какой backend активен (task / service / none).
    [[nodiscard]] OpenGlassBackend QueryActiveBackend() noexcept;
}
