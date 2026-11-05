#pragma once

#include <string>

struct SetupCommandLine {
    bool modify = false;
    std::wstring productCode;
};

[[nodiscard]] SetupCommandLine ParseSetupCommandLine() noexcept;
[[nodiscard]] bool AuthorizeMaintenanceModifyLaunch() noexcept;
[[nodiscard]] bool PathsEqualCanonical(const std::wstring& left, const std::wstring& right) noexcept;
