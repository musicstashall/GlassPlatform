#pragma once

#include <windows.h>

namespace Shared::GlassInstallerLaunch {

// HKLM\Software\StanislavKa\GlassPlatform\Installer
[[nodiscard]] bool IssueMaintenanceTicket() noexcept;
[[nodiscard]] bool TryConsumeMaintenanceTicket() noexcept;
[[nodiscard]] bool IsEulaAccepted() noexcept;
[[nodiscard]] bool MarkEulaAccepted() noexcept;

} // namespace Shared::GlassInstallerLaunch
