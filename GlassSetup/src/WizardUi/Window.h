#pragma once

#include <windows.h>
#include <string>

namespace WizardUi {

inline int Scale(UINT dpi, int value) noexcept { return MulDiv(value, static_cast<int>(dpi), 96); }
inline RECT Box(UINT dpi, int x, int y, int w, int h) noexcept {
    return {Scale(dpi,x), Scale(dpi,y), Scale(dpi,x+w), Scale(dpi,y+h)};
}

void Move(HWND parent, HWND control, RECT rect, bool redraw) noexcept;
struct FooterAction { HWND window; int x; int width; };
void LayoutFooter(HWND parent, UINT dpi, int contentHeight, FooterAction back, FooterAction next,
    HWND cancel, bool terminal, bool redraw) noexcept;
class WebHost;
void LayoutWebSurface(HWND parent, HWND host, WebHost& web, RECT rect, bool redraw) noexcept;
void DrawLoadingSpinner(HDC dc, UINT dpi, bool animate) noexcept;

class ErrorDetails final {
public:
    void Create(HWND parent, HINSTANCE instance, bool russian) noexcept;
    HWND Get() const noexcept { return edit_; }
    void Update(bool visible, const std::wstring& text);

private:
    HWND edit_ = nullptr;
    std::wstring text_;
};

} // namespace WizardUi
