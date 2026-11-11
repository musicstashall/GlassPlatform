#pragma once
#include <Windows.h>
#include <atomic>
#include <string>
#include <cstdint>

namespace GlassOpenGlassPackage {
struct Package {
    std::wstring version,setup,dll,host,gui;
    bool milComp=false;
};
// Metadata only: no workspace, payload download or execution.
bool Describe(const std::atomic<bool>& cancel,std::wstring& version,std::uint64_t& size,std::wstring& error) noexcept;
// Protected staging ONLY: no installation, service registration or upstream EXE
// execution. Caller must implement product snapshot/commit/rollback separately.
bool PrepareForCurrentSystem(const std::wstring& workspace,const std::atomic<bool>& cancel,Package& package,std::wstring& error) noexcept;
bool Prepare(const std::wstring& workspace,unsigned windowsBuild,
    const std::atomic<bool>& cancel,Package& package,std::wstring& error) noexcept;
}
