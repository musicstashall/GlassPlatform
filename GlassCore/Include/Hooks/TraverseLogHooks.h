// GlassCore/include/Hooks/TraverseLogHooks.h
#pragma once
#include <windows.h>

namespace GlassCore::Hooks::TraverseLog
{
    void Prepare();
    void Startup();
    void Shutdown();
    void EnableHooks(bool enable);
    void DisableHooks();
}