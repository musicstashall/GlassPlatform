#pragma once

#include <windows.h>

namespace Shared::GlassInstallerLaunch {

// HKLM\Software\StanislavKa\GlassPlatform\Installer
[[nodiscard]] bool IssueMaintenanceTicket() noexcept;
[[nodiscard]] bool TryConsumeMaintenanceTicket() noexcept;
// Per-user, per Setup version/license digest. Legacy global DWORD is not consent.
[[nodiscard]] bool IsEulaAccepted(const wchar_t* identity) noexcept;
[[nodiscard]] bool MarkEulaAccepted(const wchar_t* identity) noexcept;

} // namespace Shared::GlassInstallerLaunch
