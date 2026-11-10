#pragma once
#include <string>
#include <mutex>
// Discovery hint only, never a trust assertion. Each MSI is independently
// verified by the acquisition provider. Set before inspection threads start.
namespace GlassSetupSourceLayout {
inline std::wstring directory;
// Per-launch catalog snapshot, transported only by a validated controller parent.
// Empty + inherited means the parent was offline; do not retry the same request.
inline std::string catalogText;
inline bool catalogInherited=false;
inline bool catalogChecked=false;
inline std::mutex catalogMutex;
}
