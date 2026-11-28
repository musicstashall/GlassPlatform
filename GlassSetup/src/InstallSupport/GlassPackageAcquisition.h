#pragma once
#include "GlassPackageCatalog.h"
#include "GlassReleaseClient.h"
#include <Windows.h>
#include <atomic>

namespace GlassPackageAcquisition {
// All writes stay in a private, administrator-owned ProgramData workspace.
// A failed/malformed remote catalog does not disable independent local packages.
bool FetchCatalog(const std::atomic<bool>& cancel, GlassPackageCatalog::Catalog& catalog,
    std::wstring& diagnostic, GlassReleaseClient::Failure* failure) noexcept;
// Parent self-update or product-card retry only. Does not authorize a local file
// and does not run from product MSI download or the child handoff itself.
void PrepareCatalogRetry() noexcept;
// After Continue with no valid catalog, let the next FetchCatalog be a normal request.
void ReleaseFailedCatalogAttempt() noexcept;
// Read-only offer discovery: no workspace, package downloads, installation or
// publication. A release offer is metadata, not yet verified downloaded bytes.
// localPath optionally identifies the selected local MSI for payload metadata;
// it is empty for a release-only offer and is not install authorization.
// attachedVersion/attachedPath independently describe verified original-EXE
// media only, even when a newer remote candidate is selected. Never Current
// or a prepared session hint; these outputs do not authorize execution.
bool Describe(GlassPackageCatalog::Product product,const GlassPackageCatalog::Catalog& catalog,
    const std::atomic<bool>& cancel,std::wstring& version,bool& verifiedLocal,std::wstring* localPath=nullptr,
    std::wstring* attachedVersion=nullptr,std::wstring* attachedPath=nullptr) noexcept;
bool Prepare(GlassPackageCatalog::Product product, const GlassPackageCatalog::Catalog& catalog,
    const std::wstring& workspace, const std::atomic<bool>& cancel, std::wstring& error) noexcept;
// Prepare only; publication/restart must independently implement the handoff journal.
// Never execute the user-writable release path or silently accept a new license.
bool PrepareSetup(const GlassPackageCatalog::Asset& asset, const std::wstring& workspace,
    const std::atomic<bool>& cancel, std::wstring& prepared, std::wstring& error,
    GlassReleaseClient::Failure* failure) noexcept;
bool CreateWorkspace(std::wstring& directory) noexcept;
}
