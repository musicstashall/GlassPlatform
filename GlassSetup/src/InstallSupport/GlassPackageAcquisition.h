#pragma once
#include "GlassPackageCatalog.h"
#include <Windows.h>
#include <atomic>

namespace GlassPackageAcquisition {
// All writes stay in a private, administrator-owned ProgramData workspace.
// A failed/malformed remote catalog does not disable independent local packages.
bool FetchCatalog(const std::atomic<bool>& cancel, GlassPackageCatalog::Catalog& catalog,
    std::wstring& diagnostic) noexcept;
bool Prepare(GlassPackageCatalog::Product product, const GlassPackageCatalog::Catalog& catalog,
    const std::wstring& workspace, const std::atomic<bool>& cancel, std::wstring& error) noexcept;
bool CreateWorkspace(std::wstring& directory) noexcept;
}
