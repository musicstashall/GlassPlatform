#pragma once
#include <windows.h>

namespace GlassCore::ListViewControl
{
    void Prepare();
    void Startup();
    void Shutdown();
    void OnComctl32ModuleUnload(HMODULE unloaded) noexcept;
}
