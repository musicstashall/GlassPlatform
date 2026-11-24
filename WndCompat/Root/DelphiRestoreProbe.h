#pragma once
#include <windows.h>
#include <DebugLog.h>
#include <GlassControlKind.h>

// Debug builds only. What the surface of a VCL top-level window holds where
// each of its child controls lies, at three moments after the window was shown
// or restored:
//   shown   - on the first WM_NCPAINT / WM_ERASEBKGND of the window: before
//             Windows has erased anything in it;
//   erased  - on a message posted from there: after the erase passes Windows
//             runs on its own, before the first WM_PAINT (a posted message is
//             taken before WM_PAINT);
//   painted - on a timer set from there: after the pending WM_PAINTs (WM_TIMER
//             is taken after WM_PAINT).
// One line per visible child: its class, its parent, its style, its rectangle
// in the client of the top-level window, three pixels of it and whether a
// paint is still pending for it. The pixels: the centre, the left quarter of
// the same row, and a point near the top left corner - where a container
// seldom has a child, and a control its text. They are read through a DC of
// the top-level window without child clipping: the children share its
// surface. Nothing is drawn, no message is sent to a control, and nothing the
// platform decides depends on what is read. The DC is taken the way any DC of
// that window is: for the time of a read the platform sees one more GetDC
// session of the top-level window itself.
namespace WndCompat::DelphiRestoreProbe
{
#if defined(_DEBUG)
    inline constexpr UINT_PTR kTimerId = 0x47504C52;        // 'GPLR'
    inline constexpr WPARAM kPostedWParam = 0x47504C52;
    inline constexpr LPARAM kPostedLParam = 0x52455354;     // 'REST'
    inline constexpr unsigned kLinesPerMoment = 200;
    inline constexpr ULONGLONG kQuietMs = 3000;             // one probe in three seconds
    inline constexpr UINT kPaintedAfterMs = 100;

    enum class Stage { Idle, Armed, Shown, Erased };
    struct State
    {
        HWND root{};
        Stage stage{ Stage::Idle };
        ULONGLONG startedAt{};      // when the last probe read its first moment
    };
    inline thread_local State t_state;

    struct Moment
    {
        HWND root{};
        HDC dc{};
        const wchar_t* name{};
        unsigned left{};
    };

    inline BOOL CALLBACK ReportChild(HWND child, LPARAM lParam)
    {
        auto* moment = reinterpret_cast<Moment*>(lParam);
        if (!moment || !moment->left) return FALSE;
        if (!IsWindowVisible(child)) return TRUE;
        wchar_t cls[64]{}, parentCls[64]{};
        if (!GetClassNameW(child, cls, static_cast<int>(_countof(cls)))) return TRUE;
        const HWND parent = GetParent(child);
        if (parent) GetClassNameW(parent, parentCls, static_cast<int>(_countof(parentCls)));
        RECT rc{};
        if (!GetWindowRect(child, &rc)) return TRUE;
        MapWindowPoints(HWND_DESKTOP, moment->root, reinterpret_cast<POINT*>(&rc), 2);
        if (rc.right <= rc.left || rc.bottom <= rc.top) return TRUE;
        const LONG width = rc.right - rc.left, height = rc.bottom - rc.top;
        const int y = (rc.top + rc.bottom) / 2;
        const COLORREF centre = GetPixel(moment->dc, (rc.left + rc.right) / 2, y);
        const COLORREF quarter = GetPixel(moment->dc, rc.left + width / 4, y);
        const COLORREF corner = GetPixel(moment->dc, rc.left + (width > 8 ? 4 : width / 2),
            rc.top + (height > 8 ? 4 : height / 2));
        RECT update{};
        const BOOL pending = GetUpdateRect(child, &update, FALSE);
        --moment->left;
        DebugLog(DBG_NORMAL,
            L"[WndCompat][DelphiRestore] %s hwnd=0x%p class=%s parent=0x%p pclass=%s kind=%u style=0x%08X rect=[%ld,%ld,%ld,%ld] px=0x%08X/0x%08X/0x%08X pending=%d",
            moment->name, child, cls, parent, parentCls,
            static_cast<unsigned>(GlassShared::GlassGetWindowKind(child)),
            static_cast<unsigned>(GetWindowLongW(child, GWL_STYLE)),
            rc.left, rc.top, rc.right, rc.bottom,
            static_cast<unsigned>(centre), static_cast<unsigned>(quarter), static_cast<unsigned>(corner),
            pending ? 1 : 0);
        return TRUE;
    }

    inline void Report(HWND root, const wchar_t* name)
    {
        wchar_t cls[64]{};
        GetClassNameW(root, cls, static_cast<int>(_countof(cls)));
        DebugLog(DBG_NORMAL,
            L"[WndCompat][DelphiRestore] %s root=0x%p class=%s visible=%d iconic=%d foreground=%d",
            name, root, cls, IsWindowVisible(root) ? 1 : 0, IsIconic(root) ? 1 : 0,
            GetForegroundWindow() == root ? 1 : 0);
        const HDC dc = GetDCEx(root, nullptr, DCX_CACHE);
        if (!dc) return;
        Moment moment{ root, dc, name, kLinesPerMoment };
        EnumChildWindows(root, ReportChild, reinterpret_cast<LPARAM>(&moment));
        ReleaseDC(root, dc);
    }

    // True: the message was the probe's own and is not passed on.
    [[nodiscard]] inline bool OnRootMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        State& state = t_state;
        switch (message)
        {
        case WM_WINDOWPOSCHANGING:
        {
            // A window that is being shown, or one that leaves the taskbar.
            const auto* pos = reinterpret_cast<const WINDOWPOS*>(lParam);
            if (!pos || (!(pos->flags & SWP_SHOWWINDOW) && !IsIconic(hwnd))) return false;
            if (GetAncestor(hwnd, GA_ROOT) != hwnd) return false;
            // VCL forms only: TForm1, TfrmMain, TDelphiControlsForm.
            wchar_t cls[4]{};
            if (!GetClassNameW(hwnd, cls, static_cast<int>(_countof(cls))) || cls[0] != L'T') return false;
            // A probe has just run or is running; one that got stuck gives way.
            if (state.startedAt && GetTickCount64() - state.startedAt < kQuietMs) return false;
            if (state.stage == Stage::Erased && state.root) KillTimer(state.root, kTimerId);
            state.root = hwnd;
            state.stage = Stage::Armed;
            return false;
        }
        case WM_NCPAINT:
        case WM_ERASEBKGND:
            if (state.stage != Stage::Armed || state.root != hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd))
                return false;
            state.stage = Stage::Shown;
            state.startedAt = GetTickCount64();
            Report(hwnd, L"shown");
            if (!PostMessageW(hwnd, WM_NULL, kPostedWParam, kPostedLParam)) state.stage = Stage::Idle;
            return false;
        case WM_NULL:
            if (state.stage != Stage::Shown || state.root != hwnd
                || wParam != kPostedWParam || lParam != kPostedLParam) return false;
            state.stage = Stage::Erased;
            Report(hwnd, L"erased");
            if (!SetTimer(hwnd, kTimerId, kPaintedAfterMs, nullptr)) state.stage = Stage::Idle;
            return true;
        case WM_TIMER:
            if (wParam != kTimerId || state.stage != Stage::Erased || state.root != hwnd) return false;
            KillTimer(hwnd, kTimerId);
            state.stage = Stage::Idle;
            Report(hwnd, L"painted");
            return true;
        case WM_NCDESTROY:
            if (state.root == hwnd)
            {
                if (state.stage == Stage::Erased) KillTimer(hwnd, kTimerId);
                state = State{};
            }
            return false;
        default:
            return false;
        }
    }
#else
    [[nodiscard]] inline bool OnRootMessage(HWND, UINT, WPARAM, LPARAM) noexcept { return false; }
#endif
}
