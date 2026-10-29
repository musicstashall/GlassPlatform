#pragma once

namespace GlassInstallLog {

void Line(const wchar_t* message) noexcept;
void LineFormat(const wchar_t* format, ...) noexcept;

} // namespace GlassInstallLog
