#pragma once

#include <windows.h>
#include <cstdint>

namespace Shared::GlassHostRegistry
{
    // Флаги на top-level HWND (GA_ROOT). Расширяемо.
    constexpr std::uint32_t kSubtreeGlassDisabled = 1u << 0;

    void Remove(HWND hWnd) noexcept;

    [[nodiscard]] bool IsSubtreeGlassDisabled(HWND hTopLevel) noexcept;

    void MarkSubtreeGlassDisabled(HWND hTopLevel) noexcept;
}
