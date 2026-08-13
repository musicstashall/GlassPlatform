#include "WndPch.h"
#include <DropDownEventProvider.h>

namespace WndCompat::DropDown
{
    static DropDownEventProvider g_provider{};
    static bool g_initialized = false;

    void SetDropDownEventProvider(const DropDownEventProvider& provider)
    {
        g_provider = provider;
        g_initialized = true;
    }

    const DropDownEventProvider* GetDropDownEventProvider()
    {
        return g_initialized ? &g_provider : nullptr;
    }
}