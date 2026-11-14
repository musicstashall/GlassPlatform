#pragma once
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <functional>

// Shared by the Glass catalog and the original SecureUxTheme provider. No
// implicit redirects, credentials, external tools or DLL execution.
namespace GlassReleaseClient {
// Worker-local observer; catalogue requests never count as package bytes.
inline thread_local std::function<void(std::uint64_t,std::uint64_t)> packageProgress;
HANDLE CreatePrivateFile(const std::wstring& path);
std::wstring Wide(const std::string& text);
bool Get(const std::wstring& url, HANDLE output, std::string* text, std::uint64_t limit,
    const std::atomic<bool>& cancel, std::wstring& error);
bool VerifyHash(HANDLE file, std::uint64_t size, const std::string& sha256,
    const std::atomic<bool>& cancel, std::wstring& error);
}
