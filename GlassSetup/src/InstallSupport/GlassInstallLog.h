#pragma once
#include <string>

namespace GlassInstallLog {

// In GlassUnins, lazily creates an administrator-only per-process log directory.
std::wstring SecureLogDirectory() noexcept;
void Line(const wchar_t* message) noexcept;
void LineFormat(const wchar_t* format, ...) noexcept;

} // namespace GlassInstallLog
