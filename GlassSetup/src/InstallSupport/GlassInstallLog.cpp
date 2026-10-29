#include "GlassInstallLog.h"
#ifndef GLASS_UNINS
#include "../InstallLog.h"
#endif
#include <cstdarg>
#include <stdio.h>
#include <windows.h>

namespace GlassInstallLog {

namespace {

void AppendLine(const wchar_t* message) noexcept {
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp)) return;
    wchar_t path[MAX_PATH]{};
    swprintf_s(path, L"%sGlassInstallSupport.log", temp);
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const size_t len = message ? wcslen(message) : 0;
    DWORD written = 0;
    if (len > 0)
        WriteFile(file, message, static_cast<DWORD>(len * sizeof(wchar_t)), &written, nullptr);
    static const wchar_t crlf[] = L"\r\n";
    WriteFile(file, crlf, static_cast<DWORD>(sizeof(crlf) - sizeof(wchar_t)), &written, nullptr);
    CloseHandle(file);
}

} // namespace

void Line(const wchar_t* message) noexcept {
#ifdef GLASS_UNINS
    AppendLine(message);
#else
    InstallLog::Line(message);
#endif
}

void LineFormat(const wchar_t* format, ...) noexcept {
    va_list args;
    va_start(args, format);
    wchar_t buffer[2048]{};
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
#ifdef GLASS_UNINS
    AppendLine(buffer);
#else
    InstallLog::Line(buffer);
#endif
}

} // namespace GlassInstallLog
