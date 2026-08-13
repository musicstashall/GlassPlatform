#pragma once
#include <windows.h>

namespace GlassCore::KbxLabelHooks
{
    void Prepare();
    void Startup();
    void Shutdown();

    void EnablePaintHooks(bool enable);
}
