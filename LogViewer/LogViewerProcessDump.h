#pragma once

#include <cstdint>
#include <string>

struct LogViewerProcessDumpResult
{
    bool ok = false;
    wchar_t path[MAX_PATH]{};
    wchar_t error[256]{};
};

[[nodiscard]] std::wstring BuildDefaultDumpFileName(
    uint32_t pid,
    const std::wstring& labelHint) noexcept;

[[nodiscard]] bool GetDefaultDumpDirectory(wchar_t* outDir, size_t outDirChars) noexcept;

[[nodiscard]] bool ShowSaveDumpFileDialog(
    HWND hwndOwner,
    const wchar_t* defaultDir,
    const wchar_t* defaultFileName,
    wchar_t* outPath,
    size_t outPathChars) noexcept;

[[nodiscard]] bool WriteProcessMiniDumpToFile(
    uint32_t pid,
    const wchar_t* path,
    wchar_t* outError,
    size_t outErrorChars) noexcept;

void StartProcessDumpAsync(HWND hwndNotify, uint32_t pid, const wchar_t* path) noexcept;