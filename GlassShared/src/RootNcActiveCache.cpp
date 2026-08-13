// Кэш визуальной активности заголовка (WM_NCACTIVATE) по HWND root.

#include "SharedPch.h"
#include <Helpers/RootNcActiveCache.h>

namespace Shared::RootNcActiveCache
{
    namespace
    {
        std::shared_mutex     g_mutex;
        std::unordered_map<HWND, bool> g_map;
    }

    void Set(HWND root, bool ncActive) noexcept
    {
        if (!root || root == INVALID_HANDLE_VALUE)
            return;
        std::unique_lock lk(g_mutex);
        g_map[root] = ncActive;
    }

    bool Get(HWND root) noexcept
    {
        if (!root || root == INVALID_HANDLE_VALUE)
            return true;
        std::shared_lock lk(g_mutex);
        const auto it = g_map.find(root);
        if (it == g_map.end())
            return true;
        return it->second;
    }

    void Remove(HWND root) noexcept
    {
        if (!root || root == INVALID_HANDLE_VALUE)
            return;
        std::unique_lock lk(g_mutex);
        (void)g_map.erase(root);
    }
} // namespace Shared::RootNcActiveCache
