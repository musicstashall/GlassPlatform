#include "WndPch.h"
#include <RebarEventProvider.h>

namespace WndCompat::Rebar
{
    static RebarEventProvider g_provider{};
    static bool g_initialized = false;

    void SetRebarEventProvider(const RebarEventProvider& provider)
    {
        g_provider = provider;
        g_initialized = true;
    }

    const RebarEventProvider* GetRebarEventProvider()
    {
        return g_initialized ? &g_provider : nullptr;
    }
}
