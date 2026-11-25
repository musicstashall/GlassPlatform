#include "Window.h"

#include "WebHost.h"
#include "../../res/resource.h"
#include <commctrl.h>
#include <windowsx.h>
#include <cmath>

namespace WizardUi {
namespace {
LRESULT CALLBACK ErrorEditProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR caption) noexcept {
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, ErrorEditProc, id);
    if (msg == WM_CONTEXTMENU) {
        DWORD start = 0, end = 0;
        SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
        POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (point.x == -1 && point.y == -1) {
            if (!GetCaretPos(&point)) point = {};
            ClientToScreen(hwnd, &point);
        }
        if (HMENU menu = CreatePopupMenu()) {
            AppendMenuW(menu, MF_STRING | (start == end ? MF_GRAYED : MF_ENABLED), 1,
                reinterpret_cast<const wchar_t*>(caption));
            if (TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                point.x, point.y, 0, hwnd, nullptr) == 1) SendMessageW(hwnd, WM_COPY, 0, 0);
            DestroyMenu(menu);
        }
        return 0;
    }
    return DefSubclassProc(hwnd, msg, w, l);
}
} // namespace

void Move(HWND parent, HWND control, RECT rect, bool redraw) noexcept {
    if (!control) return;
    RECT current{};
    if (GetWindowRect(control, &current)) {
        MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&current), 2);
        if (EqualRect(&current, &rect)) return;
    }
    SetWindowPos(control, nullptr, rect.left, rect.top, rect.right-rect.left, rect.bottom-rect.top,
        SWP_NOZORDER | SWP_NOACTIVATE | (redraw ? 0 : SWP_NOREDRAW));
}

void LayoutFooter(HWND parent, UINT dpi, int contentHeight, FooterAction back, FooterAction next,
    HWND cancel, bool terminal, bool redraw) noexcept {
    constexpr int closeX=864, closeWidth=104, buttonHeight=32;
    const int y=contentHeight+12;
    Move(parent,back.window,Box(dpi,back.x,y,back.width,buttonHeight),redraw);
    Move(parent,next.window,Box(dpi,terminal?closeX:next.x,y,terminal?closeWidth:next.width,buttonHeight),redraw);
    Move(parent,cancel,Box(dpi,closeX,y,closeWidth,buttonHeight),redraw);
}

void LayoutWebSurface(HWND parent, HWND host, WebHost& web, RECT rect, bool redraw) noexcept {
    Move(parent, host, rect, redraw);
    web.SetBounds({0, 0, rect.right-rect.left, rect.bottom-rect.top});
}

void DrawLoadingSpinner(HDC dc, UINT dpi, bool animate) noexcept {
    const int width = Scale(dpi, 3);
    const int radius = Scale(dpi, 27);
    const int x = Scale(dpi, 473) + radius;
    const int y = Scale(dpi, 243) + radius;
    const double phase = animate ? (GetTickCount64() % 1200) / 1200.0 : 0.0;
    POINT points[49]{};
    const auto draw = [&](COLORREF color, double start, double sweep) noexcept {
        const LOGBRUSH brush{BS_SOLID, color, 0};
        HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
            width, &brush, 0, nullptr);
        if (!pen) return;
        const auto old = SelectObject(dc, pen);
        for (size_t i = 0; i < 49; ++i) {
            const double angle = (start + sweep * i / 48) * 0.017453292519943295;
            points[i] = {x + std::lround(radius * std::cos(angle)), y + std::lround(radius * std::sin(angle))};
        }
        Polyline(dc, points, 49);
        SelectObject(dc, old);
        DeleteObject(pen);
    };
    draw(RGB(50,52,78), 0, 360);
    draw(RGB(193,180,255), -90 + phase * 360, 268);
}

std::wstring LoadText(HINSTANCE instance, LANGID language, UINT id) {
    // Явно выбираем язык мастера, а не язык потока или системный порядок поиска.
    const auto block = MAKEINTRESOURCEW((id >> 4) + 1);
    HRSRC resource = FindResourceExW(instance, RT_STRING, block, language);
    if (!resource) resource = FindResourceExW(instance, RT_STRING, block,
        MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
    if (!resource) return {};
    const auto data = static_cast<const WORD*>(LockResource(LoadResource(instance, resource)));
    if (!data) return {};
    const WORD* entry = data;
    for (UINT i = 0; i < (id & 15); ++i) entry += 1 + *entry;
    return std::wstring(reinterpret_cast<const wchar_t*>(entry + 1), *entry);
}

void ErrorDetails::Create(HWND parent, HINSTANCE instance, LANGID language) {
    if (edit_) return;
    copyCaption_ = LoadText(instance, language, IDS_COPY);
    edit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL |
        ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | ES_NOHIDESEL,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ERROR_DETAILS)), instance, nullptr);
    if (edit_ && !SetWindowSubclass(edit_, ErrorEditProc, 1, reinterpret_cast<DWORD_PTR>(copyCaption_.c_str()))) {
        DestroyWindow(edit_);
        edit_ = nullptr;
    }
}

void ErrorDetails::Update(bool visible, const std::wstring& text) {
    if (!edit_) return;
    if (visible && !text.empty()) {
        if (text_ != text) {
            text_ = text;
            std::wstring lines;
            for (size_t i = 0; i < text.size(); ++i) {
                if (text[i] == L'\n' && (i == 0 || text[i-1] != L'\r')) lines += L'\r';
                lines += text[i];
            }
            SetWindowTextW(edit_, lines.c_str());
            SendMessageW(edit_, EM_SETSEL, 0, 0);
        }
        ShowWindow(edit_, SW_SHOWNA);
    } else {
        ShowWindow(edit_, SW_HIDE);
        if (!text_.empty()) { SetWindowTextW(edit_, L""); text_.clear(); }
    }
}

} // namespace WizardUi
