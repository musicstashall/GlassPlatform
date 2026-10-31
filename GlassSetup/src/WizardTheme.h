#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace WizardTheme {

inline constexpr COLORREF kDefaultFooterTop    = RGB(27, 29, 49);
inline constexpr COLORREF kDefaultFooterBottom = RGB(20, 22, 39);
inline constexpr COLORREF kDefaultFooterLine   = RGB(54, 58, 86);
inline constexpr COLORREF kDefaultCanvasBg     = RGB(16, 18, 37);

void SetRootWindow(
    HWND rootWindow,
    int contentHeightAt96Dpi = 540,
    int windowHeightAt96Dpi = 600,
    COLORREF footerTop = kDefaultFooterTop,
    COLORREF footerBottom = kDefaultFooterBottom) noexcept;

bool InstallDetours() noexcept;
void UninstallDetours() noexcept;

void PrewarmButtonCache(HDC reference, const std::vector<int>& widths, int height) noexcept;
void ResetButtonCache() noexcept;

void AttachButtonScope(HWND button) noexcept;
void DetachButtonScope(HWND button) noexcept;

void ApplyDarkWindowChrome(HWND hwnd) noexcept;
void SetSystemCloseEnabled(HWND hwnd, bool enabled) noexcept;

void PaintFooterBar(
    HDC dc,
    int clientWidth,
    int footerTopPx,
    int clientHeight,
    COLORREF topColor = kDefaultFooterTop,
    COLORREF bottomColor = kDefaultFooterBottom,
    COLORREF borderColor = kDefaultFooterLine) noexcept;

std::wstring Utf8ToWide(const std::string& text);
std::string WideToUtf8(const std::wstring& text);
std::wstring JsonEscape(const std::wstring& text);

} // namespace WizardTheme
