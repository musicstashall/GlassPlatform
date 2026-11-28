#pragma once
#include <string>
#include <mutex>
#include <array>
#include <cstdint>
// Discovery hint only, never a trust assertion. Each MSI is independently
// verified by the acquisition provider. Set before inspection threads start.
namespace GlassSetupSourceLayout {
inline std::wstring directory;
// Session-only hints, never authority. Acquisition rechecks hash, MSI identity and trust.
inline std::mutex secureUxCatalogMutex;
inline std::string secureUxCatalogText;
inline std::mutex openGlassCatalogMutex;
inline std::string openGlassCatalogText;
inline std::mutex preparedMutex;
inline std::array<std::wstring,7> prepared{};
inline std::wstring Prepared(std::size_t i) {std::lock_guard<std::mutex> lock(preparedMutex);return prepared.at(i);}
inline void SetPrepared(std::size_t i,const std::wstring& path) {std::lock_guard<std::mutex> lock(preparedMutex);prepared.at(i)=path;}

// Per-launch catalog snapshot, transported only by a validated controller parent.
// Empty inherited snapshots allow one recovery request in the child process.
inline std::string catalogText;
inline bool catalogInherited=false;
inline bool catalogChecked=false;
inline std::uint8_t catalogFailureKind=0;
inline std::uint32_t catalogFailureCode=0;
inline std::wstring catalogFailureDiagnostic;
inline std::mutex catalogMutex;
}
