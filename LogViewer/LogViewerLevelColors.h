#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>

// Цвет текста строки лога в списке LogViewer и заливка квадратиков фильтра N/I/W/E/C на тулбаре.
// Уровни совпадают с константами DBG_* в GlassShared\include\DebugLog.h (0…4).
namespace LogViewerColors
{
    inline constexpr COLORREF kLevelFg[5] = {
        RGB(0xF0, 0xF0, 0xF0), // DBG_NORMAL — N
        RGB(0x3C, 0xE0, 0xC4), // DBG_IMPORTANT — I
        RGB(0xFF, 0xE0, 0x7A), // DBG_WARNING — W
        RGB(0xFF, 0x70, 0x70), // DBG_ERROR — E
        RGB(0xFF, 0x80, 0xFF), // DBG_CRITICAL — C
    };

    [[nodiscard]] inline COLORREF ForegroundRgb(uint32_t level) noexcept
    {
        return kLevelFg[level < 5 ? level : 0];
    }

    [[nodiscard]] inline COLORREF ToolbarSquareRgb(std::size_t index0to4) noexcept
    {
        return kLevelFg[index0to4 < 5 ? index0to4 : 0];
    }
}
