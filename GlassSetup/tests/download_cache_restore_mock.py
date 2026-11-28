#!/usr/bin/env python3
"""Restore an interrupted download selection from the production function.

Fake files and catalog only. No Windows, network, MSI, or product mutation.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
main = (root / "GlassSetup/src/main.cpp").read_text(encoding="utf-8")
start = main.index("    void RestoreDownloadCache() noexcept {")
end = main.index("    void OnInspectReady()")
function = "\n".join(
    line[4:] if line.startswith("    ") else line for line in main[start:end].splitlines()
)
assert "allValid" not in function
assert "RestoreInterruptedSelection" in function
assert "aheadOfCache" in function and "aheadOfOffer" in function
complete = main[main.index("    void CompleteVersionCheck()"):main.index("    void MaybeStartDebugDemo()")]
ready = main[main.index("    void OnInspectReady()"):main.index("    void SetPage(", main.index("    void OnInspectReady()"))]
assert complete.index("releaseCatalog_=result.catalog;") < complete.index("RestoreDownloadCache();")
assert "RestoreDownloadCache" not in ready
header = (root / "GlassSetup/src/InstallSupport/GlassDownloadCache.h").read_text(encoding="utf-8")
assert "RestoreInterruptedSelection" in header

prefix = r'''
#include "GlassSetup/src/InstallSupport/GlassPackageRules.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
struct ProductReport {
    bool installed=false, sourceVerified=false;
    std::wstring installedPackageVersion, installedPayloadVersion, targetVersion, targetPayloadVersion, remotePayloadVersion;
};
struct Offer {std::wstring localVersion, releaseVersion, releaseTag; bool localVerified=false;};
struct Report {
    ProductReport x64,x86,settings64,settings32,og,secureUxTheme,openGlassResources;
    Offer secureUxOffer;
};
namespace GlassProductInspect {using ::ProductReport; using ::Report;}
namespace GlassPackageCatalog {
using GlassPackageRules::Version;
struct Asset {Version version; bool downloadable=false;};
constexpr std::size_t kCount=6;
struct Catalog {std::array<std::optional<Asset>,kCount> products{};};
}
namespace GlassDownloadCache {
constexpr std::size_t kSlots=7;
struct Entry {unsigned slot=0; std::wstring file; std::string version, sha256;};
struct Manifest {std::array<bool,kSlots> selection{}; bool hasSelection=false; std::vector<Entry> entries;};
inline bool RestoreInterruptedSelection(bool saved, bool installedNewerThanCache, bool installedNewerThanOffer) noexcept {
    return saved && !installedNewerThanCache && !installedNewerThanOffer;
}
namespace Detail {
inline std::string Narrow(const std::wstring& text){return {text.begin(),text.end()};}
inline std::wstring WideAscii(const std::string& text){return {text.begin(),text.end()};}
}
std::vector<std::wstring> present, removed;
inline std::wstring Path(const std::wstring& name){
    for(const auto& file:present) if(file==name) return L"Downloads\\"+name;
    return {};
}
inline bool ReadManifest(Manifest& out) noexcept;
inline bool Remove(const std::wstring& name) noexcept {
    removed.push_back(name);
    auto found=std::find(present.begin(),present.end(),name);
    if(found!=present.end())present.erase(found);
    return true;
}
}
namespace GlassInstallUtil {inline bool IsOs64Bit() noexcept {return true;}}
bool VersionOlder(const std::wstring& installed, const std::wstring& latest) {
    GlassPackageRules::Version a{},b{};
    return GlassPackageRules::ParseVersion(std::string(installed.begin(),installed.end()),a) &&
        GlassPackageRules::ParseVersion(std::string(latest.begin(),latest.end()),b) &&
        GlassPackageRules::Compare(a,b)<0;
}
namespace InstallLog {template<class... T> void LineFormat(const wchar_t*, T...) {} void Line(const wchar_t*) {}}
struct Selection {
    bool platform64=false, platform32=false, settings64=false, settings32=false, openGlass=false;
    bool openGlassResources=false, secureUxTheme=false, secureUxThemeRemove=false;
    unsigned removeMask=0;
    void SyncSettings(bool os64){if(!os64)settings64=false;}
};
struct App {
    bool downloadCacheChecked_=false, sutMetadataBlocked_=false, sutMetadataLocal_=true;
    Selection selection_{};
    Report inspect_{};
    GlassPackageCatalog::Catalog releaseCatalog_{};
    std::mutex inspectMutex_;
    int updates=0, syncs=0;
    bool SimulationBuild() const noexcept {return false;}
    bool SecureUxThemeSelectionBlocked() const noexcept {return false;}
    void ClampSecureUxThemeSelection() noexcept {}
    void UpdateNextText(){++updates;}
    void SyncWebViewState(){++syncs;}
'''
suffix = r'''
};
GlassDownloadCache::Manifest gManifest;
bool GlassDownloadCache::ReadManifest(Manifest& out) noexcept {out=gManifest;return true;}
GlassPackageCatalog::Asset Asset(const char* version){
    GlassPackageCatalog::Asset asset; asset.downloadable=true;
    assert(GlassPackageRules::ParseVersion(version,asset.version));
    return asset;
}
void Entry(unsigned slot,const char* version){
    GlassDownloadCache::present.push_back(L"slot"+std::to_wstring(slot)+L".msi");
    gManifest.entries.push_back({slot,GlassDownloadCache::present.back(),version,"abc"});
}
int main(){
    assert(GlassDownloadCache::RestoreInterruptedSelection(true,false,false));
    assert(!GlassDownloadCache::RestoreInterruptedSelection(true,true,false));
    assert(!GlassDownloadCache::RestoreInterruptedSelection(true,false,true));
    assert(!GlassDownloadCache::RestoreInterruptedSelection(false,false,false));
    App app;
    gManifest.hasSelection=true;
    gManifest.selection={{true,true,true,false,true,true,true}};
    // 0 current verified cache, 1 stale cache, 2 manually installed newer than cache and offer.
    Entry(0,"2.0.0.10"); Entry(1,"2.0.0.10"); Entry(2,"2.0.0.10");
    Entry(4,"3.0.2.3749"); Entry(5,"1.2.0"); Entry(6,"2.0.0.10");
    app.releaseCatalog_.products[0]=Asset("2.0.0.10");
    app.releaseCatalog_.products[1]=Asset("2.0.0.20");
    app.releaseCatalog_.products[2]=Asset("2.0.0.10");
    app.releaseCatalog_.products[5]=Asset("2.0.0.10");
    app.inspect_.x64.sourceVerified=true; app.inspect_.x64.targetVersion=L"2.0.0.10";
    app.inspect_.x86.sourceVerified=true; app.inspect_.x86.targetVersion=L"2.0.0.10";
    app.inspect_.settings64.installed=true; app.inspect_.settings64.installedPackageVersion=L"2.0.0.30";
    app.inspect_.settings64.sourceVerified=true; app.inspect_.settings64.targetVersion=L"2.0.0.10";
    app.inspect_.og.remotePayloadVersion=L"3.0.2.3749";
    app.inspect_.secureUxOffer.localVerified=true; app.inspect_.secureUxOffer.localVersion=L"1.2.0";
    app.inspect_.secureUxOffer.releaseVersion=L"1.2.0"; app.inspect_.secureUxOffer.releaseTag=L"v1.2.0";
    app.inspect_.openGlassResources.sourceVerified=true; app.inspect_.openGlassResources.targetVersion=L"2.0.0.10";
    app.RestoreDownloadCache();
    assert(app.selection_.platform64);
    assert(app.selection_.platform32);
    assert(!app.selection_.settings64);
    assert(!app.selection_.settings32);
    assert(app.selection_.openGlass);
    assert(app.selection_.secureUxTheme);
    assert(app.selection_.openGlassResources);
    assert(app.updates==1 && app.syncs==1);
    bool removedStale=false, removedCurrent=false;
    for(const auto& name:GlassDownloadCache::removed){
        removedStale|=name==L"slot1.msi";
        removedCurrent|=name==L"slot0.msi"||name==L"slot5.msi"||name==L"slot6.msi";
    }
    assert(removedStale);
    assert(!removedCurrent);
    // A second call must not consume the interrupted selection again.
    app.selection_={}; app.updates=0;
    app.RestoreDownloadCache();
    assert(!app.selection_.platform64 && app.updates==0);
    // Installed version is newer than the current offer, even without a cache file.
    App manual; manual.inspect_.x64.installed=true; manual.inspect_.x64.installedPackageVersion=L"2.0.0.40";
    manual.releaseCatalog_.products[0]=Asset("2.0.0.10");
    gManifest.entries.clear(); GlassDownloadCache::present.clear(); GlassDownloadCache::removed.clear();
    gManifest.selection={{true,false,false,false,false,false,false}};
    manual.RestoreDownloadCache();
    assert(!manual.selection_.platform64);
    assert(GlassDownloadCache::removed.empty());
    // No catalog yet: keep the interrupted product selected and do not delete
    // its file. The one restore still happens only after the catalog is published.
    App early;
    GlassDownloadCache::removed.clear();
    gManifest.entries={{0,L"slot0.msi","2.0.0.10","abc"}};
    GlassDownloadCache::present={L"slot0.msi"};
    early.RestoreDownloadCache();
    assert(early.selection_.platform64);
    assert(GlassDownloadCache::removed.empty());
    assert(early.downloadCacheChecked_);
    std::cout<<"PASS: interrupted selection restores per product; newer installed product stays unselected\n";
}
'''
with tempfile.TemporaryDirectory(prefix="DownloadCacheRestore-") as directory:
    path = Path(directory) / "test.cpp"
    exe = Path(directory) / "test"
    path.write_text(prefix + function + suffix, encoding="utf-8")
    subprocess.run(
        [os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
         "-I" + str(root), str(path), "-o", str(exe)],
        check=True,
    )
    subprocess.run([str(exe)], check=True)
