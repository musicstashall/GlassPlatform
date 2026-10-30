#pragma once
#include <windows.h>
#include <string>

// Журналы установки находятся в защищённом Windows\Temp\GlassSetupLogs-*.
namespace InstallLog {

void Initialize() noexcept;
void Shutdown() noexcept;

void Line(const wchar_t* message) noexcept;
void LineFormat(const wchar_t* format, ...) noexcept;

// Путь для msiexec /l*v (уникальное имя на один запуск MSI).
std::wstring NextMsiVerboseLogPath(const wchar_t* msiLabel) noexcept;

} // namespace InstallLog
