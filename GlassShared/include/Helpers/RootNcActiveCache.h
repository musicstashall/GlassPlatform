#pragma once

#include "framework.h"
#include <shared_mutex>
#include <unordered_map>

namespace Shared
{
    // Кэш «NC active» для top-level root: обновляется из WM_NCACTIVATE (WndCompat / GlassSettings).
    // Внимание: GlassShared — статическая библиотека; у каждого PE (GlassCore.dll, WndCompat.dll, exe)
    // своя копия этой карты. Выбор альфы DrawThemeBackground в GlassCore НЕ опирается на RootNcActiveCache::Get —
    // в GlassCore своя карта root→ncActive + кэш GFW (ThemePaintRegistry, Finalize/Remove экспорты).
    namespace RootNcActiveCache
    {
        void Set(HWND root, bool ncActive) noexcept;
        [[nodiscard]] bool Get(HWND root) noexcept;
        void Remove(HWND root) noexcept;
    } // namespace RootNcActiveCache
} // namespace Shared
