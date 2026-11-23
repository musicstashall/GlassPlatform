#pragma once
#include <windows.h>
#include <uxtheme.h>
namespace GlassCore::DelphiListBox
{
    void Startup() noexcept;
    void Shutdown() noexcept;
    bool IsInsideDraw() noexcept;
    bool BeginDraw(HWND parent, const DRAWITEMSTRUCT* dis) noexcept;
    void EndDraw() noexcept;
    bool TryCheckListThemeBackground(decltype(&::DrawThemeBackground) orig,
        HTHEME theme, HDC dc, int part, int state, LPCRECT rect, LPCRECT clip,
        HRESULT& result) noexcept;
}
