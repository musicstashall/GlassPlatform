#pragma once
#include <windows.h>
#include <uxtheme.h>
#include <cstddef>

// Internal bridge: TBitBtn shares the existing Button dispatcher and theme policy.
namespace GlassCore::Button
{
    bool AcquireDelphiBitBtnHook(HMODULE module, size_t index, bool* slotFound) noexcept;
    void ReleaseDelphiBitBtnHook(HMODULE module, size_t index) noexcept;
    bool IsButtonHookShutdown() noexcept;
    decltype(&DrawThemeBackground) GetDelphiBitBtnThemeOriginal(HMODULE module) noexcept;
    decltype(&DrawThemeTextEx) GetDelphiBitBtnTextOriginal(HMODULE module) noexcept;
    decltype(&DrawThemeParentBackground) GetDelphiBitBtnParentOriginal(HMODULE module) noexcept;
    HRESULT InvokeDelphiBitBtnThemeBackground(decltype(&DrawThemeBackground) orig, HMODULE module,
        HWND hwnd, HTHEME theme, HDC dc, int part, int state, LPCRECT rect, LPCRECT clip) noexcept;
}
