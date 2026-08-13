#pragma once

#include "framework.h"
#include "DebugLog.h"
#include "LogViewerLevelColors.h"
#include <cstdint>

inline constexpr COLORREF LogViewerListBgBase = RGB(0, 0, 0);
inline constexpr COLORREF LogViewerListBgSel = RGB(0x26, 0x4F, 0x78);
inline constexpr COLORREF LogViewerListBgSelInactive = RGB(0x3A, 0x3A, 0x3A);
inline constexpr COLORREF LogViewerListGridLine = RGB(0x2A, 0x2A, 0x2A);

inline const COLORREF kLogViewerThreadPalette[] = {
    RGB(0, 0, 0),
    RGB(0x14, 0x2A, 0x2A),
    RGB(0x2A, 0x14, 0x2A),
    RGB(0x2A, 0x2A, 0x14),
    RGB(0x14, 0x1C, 0x30),
    RGB(0x30, 0x1C, 0x14),
    RGB(0x14, 0x2C, 0x1C),
    RGB(0x2C, 0x14, 0x1C),
};
inline constexpr int kLogViewerThreadPaletteCount =
    static_cast<int>(sizeof(kLogViewerThreadPalette) / sizeof(kLogViewerThreadPalette[0]));

inline COLORREF LogViewerLevelFg(uint32_t level)
{
    return LogViewerColors::ForegroundRgb(level);
}

inline const wchar_t* LogViewerLevelName(uint32_t level)
{
    switch (level)
    {
    case DBG_NORMAL:    return L"INFO";
    case DBG_IMPORTANT: return L"IMPRT";
    case DBG_WARNING:   return L"WARN";
    case DBG_ERROR:     return L"ERROR";
    case DBG_CRITICAL:  return L"CRIT";
    default:            return L"?";
    }
}
