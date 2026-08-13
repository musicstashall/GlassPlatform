#include "WndPch.h"
#include "TooltipEventProvider.h"

namespace WndCompat::Tooltip
{
    static TooltipEventProvider g_provider{};
    static bool g_initialized = false;

    void SetTooltipEventProvider(const TooltipEventProvider& provider)
    {
        g_provider = provider;
        g_initialized = true;
    }

    const TooltipEventProvider* GetTooltipEventProvider()
    {
        return g_initialized ? &g_provider : nullptr;
    }
}
