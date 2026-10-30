#include "WizardTheme.h"
#include "InstallSupport/GlassInstallLog.h"
#include <Helpers/ThemeHelper.h>
#include <commctrl.h>
#include <detours.h>
#include <dwmapi.h>
#include "GdiPlusCompat.h"
#include <uxtheme.h>
#include <vssym32.h>
#include <algorithm>
#include <cmath>
#include <iterator>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

namespace WizardTheme {

namespace {

using DrawThemeBackgroundFn = HRESULT(WINAPI*)(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
using DrawThemeTextFn = HRESULT(WINAPI*)(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, DWORD, LPCRECT);

PVOID g_originalDrawThemeBackground = nullptr;
PVOID g_originalDrawThemeText = nullptr;
HMODULE g_uxThemeModule = nullptr;
bool g_themeDetoursInstalled = false;

HWND g_rootWindow = nullptr;
int g_contentHeight96 = 540;
int g_windowHeight96 = 600;
COLORREF g_footerTop = kDefaultFooterTop;
COLORREF g_footerBottom = kDefaultFooterBottom;

constexpr UINT_PTR kButtonScopeSubclassId = 0x47505354;
thread_local HWND g_dtbCurrentButton = nullptr;

struct ButtonSkin {
    COLORREF top;
    COLORREF bottom;
    COLORREF border;
    COLORREF highlight;
    BYTE opacity;
};

ButtonSkin ButtonSkinFor(bool focusedBlue, int state) noexcept {
    if (focusedBlue) {
        switch (state) {
        case PBS_HOT: return {RGB(62, 137, 207), RGB(37, 86, 145), RGB(132, 205, 255), RGB(217, 243, 255), 255};
        case PBS_PRESSED: return {RGB(30, 77, 127), RGB(24, 57, 98), RGB(80, 161, 226), RGB(122, 191, 239), 255};
        case PBS_DISABLED: return {RGB(43, 58, 75), RGB(31, 40, 56), RGB(69, 89, 111), RGB(84, 108, 132), 150};
        case PBS_DEFAULTED:
        case PBS_DEFAULTED_ANIMATING: return {RGB(51, 119, 183), RGB(34, 76, 129), RGB(112, 195, 255), RGB(209, 239, 255), 255};
        default: return {RGB(47, 109, 171), RGB(32, 73, 124), RGB(93, 180, 247), RGB(198, 231, 255), 255};
        }
    }
    switch (state) {
    case PBS_HOT: return {RGB(58, 62, 89), RGB(34, 37, 63), RGB(132, 139, 179), RGB(205, 207, 232), 255};
    case PBS_PRESSED: return {RGB(21, 23, 42), RGB(32, 35, 56), RGB(84, 90, 126), RGB(110, 115, 148), 255};
    case PBS_DISABLED: return {RGB(30, 32, 47), RGB(23, 25, 39), RGB(62, 65, 86), RGB(75, 78, 98), 150};
    case PBS_DEFAULTED:
    case PBS_DEFAULTED_ANIMATING: return {RGB(47, 49, 72), RGB(27, 29, 49), RGB(119, 125, 164), RGB(190, 192, 218), 255};
    default: return {RGB(42, 45, 67), RGB(25, 27, 47), RGB(91, 96, 132), RGB(165, 170, 202), 255};
    }
}

COLORREF MixColor(COLORREF from, COLORREF to, double amount) noexcept {
    amount = std::clamp(amount, 0.0, 1.0);
    const auto mix = [amount](BYTE a, BYTE b) {
        return static_cast<BYTE>(std::lround(a + (b - a) * amount));
    };
    return RGB(mix(GetRValue(from), GetRValue(to)),
               mix(GetGValue(from), GetGValue(to)),
               mix(GetBValue(from), GetBValue(to)));
}

bool InsideRoundedRect(double x, double y, double width, double height, double radius) noexcept {
    if (x < 0.0 || y < 0.0 || x >= width || y >= height) return false;
    const double cx = std::clamp(x, radius, width - radius);
    const double cy = std::clamp(y, radius, height - radius);
    const double dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= radius * radius;
}

void RasterizeButtonImage(DWORD* pixels, int width, int height, bool focusedBlue, int state) noexcept {
    if (!pixels || width <= 0 || height <= 0) return;
    const ButtonSkin skin = ButtonSkinFor(focusedBlue, state);
    constexpr int kSamples = 4;
    constexpr int kSampleCount = kSamples * kSamples;
    const double radius = std::min(7.5, std::min(width, height) / 3.0);
    const double innerRadius = std::max(1.0, radius - 1.25);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            unsigned alphaSum = 0, redSum = 0, greenSum = 0, blueSum = 0;
            for (int sy = 0; sy < kSamples; ++sy) {
                for (int sx = 0; sx < kSamples; ++sx) {
                    const double px = x + (sx + 0.5) / kSamples;
                    const double py = y + (sy + 0.5) / kSamples;
                    if (!InsideRoundedRect(px, py, width, height, radius)) continue;
                    const bool border = !InsideRoundedRect(px - 1.25, py - 1.25,
                        width - 2.5, height - 2.5, innerRadius);
                    const double vertical = height > 1 ? py / (height - 1.0) : 0.0;
                    COLORREF color = border ? skin.border : MixColor(skin.top, skin.bottom, vertical);
                    if (!border && state != PBS_PRESSED && py < 2.0)
                        color = MixColor(color, skin.highlight, 0.34);
                    const unsigned opacity = skin.opacity;
                    alphaSum += opacity;
                    redSum += GetRValue(color) * opacity / 255;
                    greenSum += GetGValue(color) * opacity / 255;
                    blueSum += GetBValue(color) * opacity / 255;
                }
            }
            const unsigned a = (alphaSum + kSampleCount / 2) / kSampleCount;
            const unsigned r = (redSum + kSampleCount / 2) / kSampleCount;
            const unsigned g = (greenSum + kSampleCount / 2) / kSampleCount;
            const unsigned b = (blueSum + kSampleCount / 2) / kSampleCount;
            pixels[static_cast<size_t>(y) * width + x] =
                (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

class ButtonImageCache {
    struct Entry {
        int width = 0, height = 0, state = 0;
        bool focusedBlue = false;
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ previous = nullptr;
    };
    std::vector<Entry> entries_;

    bool AddState(HDC reference, int width, int height, int state, bool blue) noexcept {
        if (!reference || width <= 0 || height <= 0 || width > 2048 || height > 256)
            return false;
        if (std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) {
                return e.width == width && e.height == height && e.state == state && e.focusedBlue == blue;
            })) return true;
        Entry entry;
        entry.width = width;
        entry.height = height;
        entry.state = state;
        entry.focusedBlue = blue;
        entry.dc = CreateCompatibleDC(reference);
        if (!entry.dc) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        entry.bitmap = CreateDIBSection(reference, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!entry.bitmap || !bits) {
            if (entry.bitmap) DeleteObject(entry.bitmap);
            DeleteDC(entry.dc);
            return false;
        }
        entry.previous = SelectObject(entry.dc, entry.bitmap);
        if (!entry.previous || entry.previous == HGDI_ERROR) {
            DeleteDC(entry.dc);
            DeleteObject(entry.bitmap);
            return false;
        }
        RasterizeButtonImage(static_cast<DWORD*>(bits), width, height, blue, state);
        try {
            entries_.push_back(entry);
        } catch (...) {
            SelectObject(entry.dc, entry.previous);
            DeleteDC(entry.dc);
            DeleteObject(entry.bitmap);
            return false;
        }
        return true;
    }

public:
    ButtonImageCache() = default;
    ButtonImageCache(const ButtonImageCache&) = delete;
    ButtonImageCache& operator=(const ButtonImageCache&) = delete;
    ~ButtonImageCache() { Reset(); }

    void Reset() noexcept {
        for (auto& entry : entries_) {
            if (entry.dc) {
                if (entry.previous && entry.previous != HGDI_ERROR)
                    SelectObject(entry.dc, entry.previous);
                DeleteDC(entry.dc);
            }
            if (entry.bitmap) DeleteObject(entry.bitmap);
        }
        entries_.clear();
    }

    void Prewarm(HDC reference, const std::vector<int>& widths, int height) noexcept {
        Reset();
        if (!reference || height <= 0) return;
        static constexpr int states[] = {
            PBS_NORMAL, PBS_HOT, PBS_PRESSED, PBS_DISABLED, PBS_DEFAULTED, PBS_DEFAULTED_ANIMATING
        };
        for (int w : widths)
            for (bool blue : {false, true})
                for (int state : states)
                    (void)AddState(reference, w, height, state, blue);
    }

    bool EnsureAndDraw(HDC destination, LPCRECT rect, LPCRECT clip, int state, bool focusedBlue) noexcept {
        if (!destination || !rect || IsRectEmpty(rect)) return false;
        const int width = rect->right - rect->left;
        const int height = rect->bottom - rect->top;
        int normalizedState = state;
        if (normalizedState < PBS_NORMAL || normalizedState > PBS_DEFAULTED_ANIMATING)
            normalizedState = PBS_NORMAL;

        auto findIt = [&]() {
            return std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
                return entry.width == width && entry.height == height
                    && entry.state == normalizedState && entry.focusedBlue == focusedBlue;
            });
        };
        auto found = findIt();
        if (found == entries_.end()) {
            // Rasterize only the state actually requested by comctl32. Prewarming
            // every size and state delayed the first visible installer window.
            if (!AddState(destination, width, height, normalizedState, focusedBlue))
                return false;
            found = findIt();
            if (found == entries_.end())
                return false;
        }

        RECT clipped = *rect;
        if (clip && !IntersectRect(&clipped, rect, clip)) return true;
        const int drawWidth = clipped.right - clipped.left;
        const int drawHeight = clipped.bottom - clipped.top;
        const int sourceX = clipped.left - rect->left;
        const int sourceY = clipped.top - rect->top;
        const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        return AlphaBlend(destination, clipped.left, clipped.top, drawWidth, drawHeight,
            found->dc, sourceX, sourceY, drawWidth, drawHeight, blend) != FALSE;
    }
};

ButtonImageCache g_buttonCache;

void PaintFooterBackdropForButton(HDC dc, HWND button, LPCRECT rect, LPCRECT clip) noexcept {
    if (!dc || !button || !rect || !g_rootWindow || GetAncestor(button, GA_ROOT) != g_rootWindow)
        return;
    POINT buttonOrigin{0, 0};
    MapWindowPoints(button, g_rootWindow, &buttonOrigin, 1);
    const UINT dpi = GetDpiForWindow(g_rootWindow);
    const int footerTop = MulDiv(g_contentHeight96, static_cast<int>(dpi), 96);
    const int footerBottom = MulDiv(g_windowHeight96, static_cast<int>(dpi), 96);
    RECT paint = *rect;
    if (clip && !IntersectRect(&paint, rect, clip)) return;
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, paint.left, paint.top, paint.right, paint.bottom);
    Gdiplus::Graphics graphics(dc);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    Gdiplus::LinearGradientBrush backdrop(
        Gdiplus::Point(paint.left, footerTop - buttonOrigin.y),
        Gdiplus::Point(paint.left, footerBottom - buttonOrigin.y),
        Gdiplus::Color(255, GetRValue(g_footerTop), GetGValue(g_footerTop), GetBValue(g_footerTop)),
        Gdiplus::Color(255, GetRValue(g_footerBottom), GetGValue(g_footerBottom), GetBValue(g_footerBottom)));
    graphics.FillRectangle(&backdrop,
        static_cast<INT>(paint.left),
        static_cast<INT>(paint.top),
        static_cast<INT>(paint.right - paint.left),
        static_cast<INT>(paint.bottom - paint.top));
    RestoreDC(dc, saved);
}

LRESULT CALLBACK ButtonScopeSubclassProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR /*subclassId*/,
    DWORD_PTR /*reference*/) {
    // Сохраняем только контекст HWND на стеке потока: WM_PAINT остаётся за comctl32
    // вместе со штатной анимацией переходов BufferedPaintRenderAnimation.
    HWND previous = g_dtbCurrentButton;
    g_dtbCurrentButton = hwnd;
    const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
    g_dtbCurrentButton = previous;
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(hwnd, ButtonScopeSubclassProc, kButtonScopeSubclassId);
    return result;
}

bool IsActiveFocusedButton(HWND button) noexcept {
    return button && IsWindow(button) && button == GetFocus() && IsWindowEnabled(button) &&
           g_rootWindow && GetAncestor(button, GA_ROOT) == g_rootWindow &&
           GetForegroundWindow() == g_rootWindow;
}

COLORREF ButtonTextColorForThemeState(int part, int state, bool focusedBlue) noexcept {
    if (part != BP_PUSHBUTTON) return RGB(239, 237, 249);
    if (state == PBS_DISABLED) return RGB(143, 147, 166);
    if (focusedBlue) {
        switch (state) {
        case PBS_HOT: return RGB(255, 255, 255);
        case PBS_PRESSED: return RGB(229, 243, 255);
        case PBS_DEFAULTED:
        case PBS_DEFAULTED_ANIMATING: return RGB(250, 252, 255);
        default: return RGB(246, 250, 255);
        }
    }
    switch (state) {
    case PBS_HOT: return RGB(250, 250, 255);
    case PBS_PRESSED: return RGB(222, 224, 239);
    case PBS_DEFAULTED:
    case PBS_DEFAULTED_ANIMATING: return RGB(246, 245, 253);
    default: return RGB(239, 237, 249);
    }
}

// В HookDrawThemeText вызываем DrawThemeTextEx с подправленным цветом текста,
// оригинал DrawThemeText не вызываем (без патча IAT uxtheme.dll -> DrawTextExW).
HRESULT WINAPI HookDrawThemeText(
    HTHEME theme,
    HDC dc,
    int part,
    int state,
    LPCWSTR text,
    int textLength,
    DWORD textFlags,
    DWORD textFlags2,
    LPCRECT textRect) {
    const auto original = reinterpret_cast<DrawThemeTextFn>(g_originalDrawThemeText);
    if (!original) return E_UNEXPECTED;
    HWND button = g_dtbCurrentButton;
    const bool validButton = button && IsWindow(button);
    const DWORD buttonType = validButton ?
        (static_cast<DWORD>(GetWindowLongPtrW(button, GWL_STYLE)) & BS_TYPEMASK) : 0xFFFFFFFFu;
    const bool ourPushButton = validButton && g_rootWindow && GetAncestor(button, GA_ROOT) == g_rootWindow &&
        (buttonType == BS_PUSHBUTTON || buttonType == BS_DEFPUSHBUTTON);
    if (part != BP_PUSHBUTTON || !ourPushButton || !textRect)
        return original(theme, dc, part, state, text, textLength, textFlags, textFlags2, textRect);

    DTTOPTS opts{};
    opts.dwSize = sizeof(opts);
    opts.dwFlags = DTT_TEXTCOLOR;
    opts.crText = ButtonTextColorForThemeState(part, state, IsActiveFocusedButton(button));
    RECT mutableRect = *textRect;
    return DrawThemeTextEx(theme, dc, part, state, text, textLength, textFlags, &mutableRect, &opts);
}

HRESULT WINAPI HookDrawThemeBackground(
    HTHEME theme,
    HDC dc,
    int part,
    int state,
    LPCRECT rect,
    LPCRECT clip) {
    const auto original = reinterpret_cast<DrawThemeBackgroundFn>(g_originalDrawThemeBackground);
    if (!original) return E_UNEXPECTED;
    wchar_t themeClass[MAX_PATH]{L"?"};
    const HRESULT classHr = theme ? Shared::ThemeHelper::GetThemeClass(theme, themeClass, MAX_PATH) : E_INVALIDARG;
    const bool buttonTheme = SUCCEEDED(classHr) && _wcsicmp(themeClass, L"Button") == 0;
    if (buttonTheme && part == BP_PUSHBUTTON && dc && rect && !IsRectEmpty(rect)) {
        HWND button = g_dtbCurrentButton;
        if (!button || !g_rootWindow || GetAncestor(button, GA_ROOT) != g_rootWindow)
            button = WindowFromDC(dc);
        const DWORD type=button?(static_cast<DWORD>(GetWindowLongPtrW(button,GWL_STYLE)) & BS_TYPEMASK):0xFFFFFFFFu;
        wchar_t cls[16]{};
        const bool push=button && GetClassNameW(button,cls,static_cast<int>(std::size(cls))) &&
            _wcsicmp(cls,L"Button")==0 && (type==BS_PUSHBUTTON || type==BS_DEFPUSHBUTTON);
        if (push && g_rootWindow && GetAncestor(button, GA_ROOT) == g_rootWindow) {
            const bool activeFocus = IsActiveFocusedButton(button);
            PaintFooterBackdropForButton(dc, button, rect, clip);
            if (g_buttonCache.EnsureAndDraw(dc, rect, clip, state, activeFocus))
                return S_OK;
        }
    }
    return original(theme, dc, part, state, rect, clip);
}

} // namespace

void SetRootWindow(
    HWND rootWindow,
    int contentHeightAt96Dpi,
    int windowHeightAt96Dpi,
    COLORREF footerTop,
    COLORREF footerBottom) noexcept {
    g_rootWindow = rootWindow;
    g_contentHeight96 = contentHeightAt96Dpi;
    g_windowHeight96 = windowHeightAt96Dpi;
    g_footerTop = footerTop;
    g_footerBottom = footerBottom;
}

bool InstallDetours() noexcept {
    if (g_themeDetoursInstalled)
        return true;
    if (!g_uxThemeModule) {
        g_uxThemeModule = LoadLibraryW(L"uxtheme.dll");
        if (!g_uxThemeModule)
            return false;
    }
    if (!g_originalDrawThemeBackground)
        g_originalDrawThemeBackground = reinterpret_cast<PVOID>(GetProcAddress(g_uxThemeModule, "DrawThemeBackground"));
    if (!g_originalDrawThemeText)
        g_originalDrawThemeText = reinterpret_cast<PVOID>(GetProcAddress(g_uxThemeModule, "DrawThemeText"));
    if (!g_originalDrawThemeBackground || !g_originalDrawThemeText)
        return false;

    DetourSetIgnoreTooSmall(TRUE);
    LONG status = DetourTransactionBegin();
    if (status != NO_ERROR) return false;
    status = DetourUpdateThread(GetCurrentThread());
    if (status == NO_ERROR)
        status = DetourAttach(&g_originalDrawThemeBackground, reinterpret_cast<PVOID>(HookDrawThemeBackground));
    if (status == NO_ERROR)
        status = DetourAttach(&g_originalDrawThemeText, reinterpret_cast<PVOID>(HookDrawThemeText));
    if (status != NO_ERROR) {
        DetourTransactionAbort();
        return false;
    }
    status = DetourTransactionCommit();
    g_themeDetoursInstalled = (status == NO_ERROR);
    return g_themeDetoursInstalled;
}

void UninstallDetours() noexcept {
    if (!g_themeDetoursInstalled)
        return;
    if (DetourTransactionBegin() == NO_ERROR) {
        DetourUpdateThread(GetCurrentThread());
        if (g_originalDrawThemeBackground)
            DetourDetach(&g_originalDrawThemeBackground, reinterpret_cast<PVOID>(HookDrawThemeBackground));
        if (g_originalDrawThemeText)
            DetourDetach(&g_originalDrawThemeText, reinterpret_cast<PVOID>(HookDrawThemeText));
        DetourTransactionCommit();
    }
    g_themeDetoursInstalled = false;
}

void PrewarmButtonCache(HDC reference, const std::vector<int>& widths, int height) noexcept {
    g_buttonCache.Prewarm(reference, widths, height);
}

void ResetButtonCache() noexcept {
    g_buttonCache.Reset();
}

void AttachButtonScope(HWND button) noexcept {
    if (!button || !IsWindow(button)) return;
    SetWindowSubclass(button, ButtonScopeSubclassProc, kButtonScopeSubclassId, 0);
}

void DetachButtonScope(HWND button) noexcept {
    if (!button || !IsWindow(button)) return;
    RemoveWindowSubclass(button, ButtonScopeSubclassProc, kButtonScopeSubclassId);
}

void ApplyDarkWindowChrome(HWND hwnd) noexcept {
    if (!hwnd) return;
    const BOOL dark = TRUE;
    (void)DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const DWORD roundPref = 2; // DWMWCP_ROUND
    (void)DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &roundPref, sizeof(roundPref));
}

void SetSystemCloseEnabled(HWND hwnd, bool enabled) noexcept {
    if (!hwnd) return;
    HMENU sysMenu = GetSystemMenu(hwnd, FALSE);
    if (!sysMenu) return;
    EnableMenuItem(sysMenu, SC_CLOSE,
        MF_BYCOMMAND | (enabled ? MF_ENABLED : (MF_DISABLED | MF_GRAYED)));
}

void PaintFooterBar(
    HDC dc,
    int clientWidth,
    int footerTopPx,
    int clientHeight,
    COLORREF topColor,
    COLORREF bottomColor,
    COLORREF borderColor) noexcept {
    if (!dc || clientWidth <= 0 || clientHeight <= footerTopPx) return;
    Gdiplus::Graphics g(dc);
    Gdiplus::LinearGradientBrush grad(
        Gdiplus::Point(0, footerTopPx),
        Gdiplus::Point(0, clientHeight),
        Gdiplus::Color(255, GetRValue(topColor), GetGValue(topColor), GetBValue(topColor)),
        Gdiplus::Color(255, GetRValue(bottomColor), GetGValue(bottomColor), GetBValue(bottomColor)));
    g.FillRectangle(&grad, 0, footerTopPx, clientWidth, clientHeight - footerTopPx);
    Gdiplus::Pen linePen(Gdiplus::Color(255, GetRValue(borderColor), GetGValue(borderColor), GetBValue(borderColor)), 1.0f);
    g.DrawLine(&linePen, 0, footerTopPx, clientWidth, footerTopPx);
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring wide(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), count);
    return wide;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string utf8(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        utf8.data(), count, nullptr, nullptr);
    return utf8;
}

std::wstring JsonEscape(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size() + 16);
    for (wchar_t ch : text) {
        switch (ch) {
        case L'\\': out += L"\\\\"; break;
        case L'"':  out += L"\\\""; break;
        case L'\r': out += L"\\r"; break;
        case L'\n': out += L"\\n"; break;
        case L'\t': out += L"\\t"; break;
        default:
            if (ch < 0x20) {
                wchar_t hex[8]{};
                swprintf_s(hex, L"\\u%04x", static_cast<unsigned>(ch));
                out += hex;
            } else {
                out.push_back(ch);
            }
            break;
        }
    }
    return out;
}

} // namespace WizardTheme
