#pragma once
#include <Windows.h>
#include <atomic>
#include <string>

namespace GlassOpenGlassPackage {
struct Package {
    std::wstring version,setup,dll,host,gui;
    bool milComp=false;
};
// Protected staging ONLY: no installation, service registration or upstream EXE
// execution. Caller must implement product snapshot/commit/rollback separately.
bool Prepare(const std::wstring& workspace,unsigned windowsBuild,
    const std::atomic<bool>& cancel,Package& package,std::wstring& error) noexcept;
}
