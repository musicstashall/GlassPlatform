#include "WndPch.h"
#include <RootEventProvider.h>

namespace WndCompat::Root
{
    static RootEventProvider g_provider{};
    static bool g_initialized = false;

    void SetRootEventProvider(const RootEventProvider& provider)
    {
        g_provider = provider;
        g_initialized = true;
    }

    const RootEventProvider* GetRootEventProvider()
    {
        return g_initialized ? &g_provider : nullptr;
    }
}
