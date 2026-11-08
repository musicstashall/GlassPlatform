#pragma once
#include "GlassPackageCatalog.h"
#include <Windows.h>
#include <atomic>

namespace GlassPackageAcquisition {
// All writes stay in a private, administrator-owned ProgramData workspace.
// A failed/malformed remote catalog does not disable independent local packages.
bool FetchCatalog(const std::atomic<bool>& cancel, GlassPackageCatalog::Catalog& catalog,
    std::wstring& diagnostic) noexcept;
// Read-only offer discovery: no workspace, package downloads, installation or
// publication. A release offer is metadata, not yet verified downloaded bytes.
bool Describe(GlassPackageCatalog::Product product,const GlassPackageCatalog::Catalog& catalog,
    const std::atomic<bool>& cancel,std::wstring& version,bool& verifiedLocal) noexcept;
bool Prepare(GlassPackageCatalog::Product product, const GlassPackageCatalog::Catalog& catalog,
    const std::wstring& workspace, const std::atomic<bool>& cancel, std::wstring& error) noexcept;
// Prepare only; publication/restart must independently implement the handoff journal.
// Never execute the user-writable release path or silently accept a new license.
bool PrepareSetup(const GlassPackageCatalog::Asset& asset, const std::wstring& workspace,
    const std::atomic<bool>& cancel, std::wstring& prepared, std::wstring& error) noexcept;
bool CreateWorkspace(std::wstring& directory) noexcept;
}
