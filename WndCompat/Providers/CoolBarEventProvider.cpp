#include "WndPch.h"
#include <CoolBarEventProvider.h>

namespace WndCompat::CoolBar
{
    static CoolBarEventProvider g_provider{};
    static bool g_initialized = false;

    void SetCoolBarEventProvider(const CoolBarEventProvider& provider)
    {
        g_provider = provider;
        g_initialized = true;
    }

    const CoolBarEventProvider* GetCoolBarEventProvider()
    {
        return g_initialized ? &g_provider : nullptr;
    }
}
