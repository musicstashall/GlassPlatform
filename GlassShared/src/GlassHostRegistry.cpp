#include "SharedPch.h"
#include <GlassHostRegistry.h>

#include <shared_mutex>
#include <unordered_map>

namespace Shared::GlassHostRegistry
{
    namespace
    {
        std::shared_mutex g_mutex;
        std::unordered_map<HWND, std::uint32_t> g_flagsByHwnd;
    }

    void Remove(HWND hWnd) noexcept
    {
        if (!hWnd)
            return;
        std::unique_lock lock(g_mutex);
        (void)g_flagsByHwnd.erase(hWnd);
    }

    bool IsSubtreeGlassDisabled(HWND hTopLevel) noexcept
    {
        if (!hTopLevel)
            return false;
        std::shared_lock lock(g_mutex);
        const auto it = g_flagsByHwnd.find(hTopLevel);
        if (it == g_flagsByHwnd.end())
            return false;
        return (it->second & kSubtreeGlassDisabled) != 0;
    }

    void MarkSubtreeGlassDisabled(HWND hTopLevel) noexcept
    {
        if (!hTopLevel)
            return;
        std::unique_lock lock(g_mutex);
        g_flagsByHwnd[hTopLevel] |= kSubtreeGlassDisabled;
    }
}
