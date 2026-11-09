#pragma once

#include <string>

struct SetupCommandLine {
    bool modify = false;
    std::wstring productCode;
};

[[nodiscard]] SetupCommandLine ParseSetupCommandLine() noexcept;
[[nodiscard]] bool AuthorizeMaintenanceModifyLaunch() noexcept;
[[nodiscard]] bool PathsEqualCanonical(const std::wstring& left, const std::wstring& right) noexcept;

// Controller handoff has no authority to replay product actions or EULA consent.
// Child startup blocks before dispatching any UI/product work until commit.
[[nodiscard]] bool BeginControllerChild(std::wstring& error) noexcept;
[[nodiscard]] bool CompleteControllerChild(bool windowReady) noexcept;
[[nodiscard]] bool IsControllerChild() noexcept;
enum class ControllerTransition { Continue, Transferred, Failed };
[[nodiscard]] ControllerTransition EnsureCurrentController(const std::wstring& downloaded,
    bool modify,const std::wstring& product,std::wstring& error) noexcept;

// Readiness commits controller lifecycle only, never product transactions.
void FinalizeControllerStartup(bool ready) noexcept;
