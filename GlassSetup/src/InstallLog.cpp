#include "InstallLog.h"
#include <shlwapi.h>
#include <cstdarg>
#include <cstdio>
#include <cwchar>

namespace {

HANDLE gLogFile = INVALID_HANDLE_VALUE;
SRWLOCK gLock = SRWLOCK_INIT;
std::wstring gLogDir;
std::wstring gSessionStamp;
unsigned gMsiLogSerial = 0;

void WriteRaw(const wchar_t* text) noexcept {
    if (!text || gLogFile == INVALID_HANDLE_VALUE) return;
    AcquireSRWLockExclusive(&gLock);
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes > 1) {
        std::string utf8(static_cast<size_t>(bytes - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), bytes, nullptr, nullptr);
        utf8.push_back('\n');
        DWORD written = 0;
        WriteFile(gLogFile, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        FlushFileBuffers(gLogFile);
    }
    ReleaseSRWLockExclusive(&gLock);
}

std::wstring NowStamp() noexcept {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[64]{};
    swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

std::wstring FileStamp() noexcept {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[32]{};
    swprintf_s(buf, L"%04u%02u%02u_%02u%02u%02u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

bool ResolveLogDirectory(std::wstring& outDir) noexcept {
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    if (!PathRemoveFileSpecW(exe)) return false;
    outDir.assign(exe);
    return true;
}

} // namespace

namespace InstallLog {

void Initialize() noexcept {
    if (gLogFile != INVALID_HANDLE_VALUE) return;
    if (!ResolveLogDirectory(gLogDir)) return;
    gSessionStamp = FileStamp();
    const std::wstring path = gLogDir + L"\\GlassSetup.log";
    gLogFile = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (gLogFile == INVALID_HANDLE_VALUE) return;

    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    LineFormat(L"======== Session %s ========", gSessionStamp.c_str());
    LineFormat(L"GlassSetup: %s", exe);
    LineFormat(L"Log directory: %s", gLogDir.c_str());
    BOOL admin = FALSE;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    PSID adminGroup = nullptr;
    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &admin);
        FreeSid(adminGroup);
    }
    LineFormat(L"Process elevated (admin): %s", admin ? L"yes" : L"no");
}

void Shutdown() noexcept {
    if (gLogFile != INVALID_HANDLE_VALUE) {
        Line(L"======== Session end ========");
        CloseHandle(gLogFile);
        gLogFile = INVALID_HANDLE_VALUE;
    }
}

void Line(const wchar_t* message) noexcept {
    if (!message) return;
    if (gLogFile == INVALID_HANDLE_VALUE) Initialize();
    std::wstring line = NowStamp();
    line += L" | ";
    line += message;
    WriteRaw(line.c_str());
}

void LineFormat(const wchar_t* format, ...) noexcept {
    if (!format) return;
    wchar_t buffer[2048]{};
    va_list args;
    va_start(args, format);
    vswprintf_s(buffer, format, args);
    va_end(args);
    Line(buffer);
}

std::wstring NextMsiVerboseLogPath(const wchar_t* msiLabel) noexcept {
    if (gLogDir.empty() && !ResolveLogDirectory(gLogDir)) {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        gLogDir = temp;
    }
    if (gSessionStamp.empty()) gSessionStamp = FileStamp();
    const unsigned id = ++gMsiLogSerial;
    wchar_t name[MAX_PATH]{};
    swprintf_s(name, L"GlassSetup-msi-%s_%u_%s.log", gSessionStamp.c_str(), id,
        msiLabel ? msiLabel : L"msi");
    for (wchar_t* p = name; *p; ++p) {
        if (*p == L' ' || *p == L'(' || *p == L')') *p = L'_';
    }
    return gLogDir + L"\\" + name;
}

} // namespace InstallLog
