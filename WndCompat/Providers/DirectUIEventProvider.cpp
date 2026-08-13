#include "WndPch.h"
#include <DirectUIEventProvider.h>

namespace WndCompat::DirectUI
{
    static DirectUIEventProvider g_provider{};
    static bool g_initialized = false;

    void SetDirectUIEventProvider(const DirectUIEventProvider& provider)
    {
        g_provider = provider;
        g_initialized = true;
    }

    const DirectUIEventProvider* GetDirectUIEventProvider()
    {
        return g_initialized ? &g_provider : nullptr;
    }
}