#pragma once
#include <string>
// Discovery hint only, never a trust assertion. Each MSI is independently
// verified by the acquisition provider. Set before inspection threads start.
namespace GlassSetupSourceLayout { inline std::wstring directory; }
