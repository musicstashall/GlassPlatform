#include "WndPch.h"
#include "MenuEventProvider.h"

namespace WndCompat::Menu
{
    static MenuEventProvider g_menuEventProvider{};
    static bool g_menuEventProviderInitialized = false;

    void SetMenuEventProvider(const MenuEventProvider& provider)
    {
        g_menuEventProvider = provider;
        g_menuEventProviderInitialized = true;
    }

    const MenuEventProvider* GetMenuEventProvider()
    {
        return g_menuEventProviderInitialized ? &g_menuEventProvider : nullptr;
    }
}
