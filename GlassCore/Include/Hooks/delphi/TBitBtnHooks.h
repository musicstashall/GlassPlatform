#pragma once
#include <windows.h>
#include <uxtheme.h>
#include <cstddef>

namespace GlassCore::Button
{
    [[nodiscard]] bool IsDelphiBitBtn(HWND hwnd) noexcept;
#if defined(_DEBUG)
    void LogDelphiBitBtnBeginPaint(HWND hwnd, HMODULE caller, size_t depth) noexcept;
#endif
    bool BeginDelphiBitBtnOwnerDraw(HWND parent, const DRAWITEMSTRUCT* dis) noexcept;
    void EndDelphiBitBtnOwnerDraw() noexcept;
    void ClearDelphiBitBtnDraw() noexcept;
    bool TryDelphiBitBtnThemeBgDetour(decltype(&DrawThemeBackground) orig,
        HTHEME theme, HDC dc, int part, int state, LPCRECT rect, LPCRECT clip,
        HRESULT& result) noexcept;
    HRESULT WINAPI DelphiBitBtnThemeBg(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
    HRESULT WINAPI DelphiBitBtnThemeText(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, LPRECT, const DTTOPTS*);
    HRESULT WINAPI DelphiBitBtnParentBg(HWND, HDC, const RECT*);
}
