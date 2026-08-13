#pragma once

#include <windows.h>
#include <cstddef>

bool ListViewIsScrolledToBottom(HWND hList, int itemCount) noexcept;

void FormatFilterToolbarLabel(
    wchar_t* buf,
    size_t bufChars,
    bool restrictActive,
    size_t selectedCount,
    const wchar_t* allLabel,
    const wchar_t* form1,
    const wchar_t* form2to4,
    const wchar_t* form5plus) noexcept;

bool IsGlassPlatformRunning();
