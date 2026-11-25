#pragma once
#include <windows.h>
#include <GlassBackdropMark.h>
#include <InjectionMode.h>
#include <Tracker/WinEventHandler.h>

// A top-level window that is shown again does not have to start from a
// cleared surface. LogViewer-25: on the first WM_NCPAINT of Restorator's form
// after the application was restored, every control lay on a light picture
// that was not the form's. Windows then erases the controls at once, outside
// any paint, and paints them only later. A control that answers the erase
// without drawing - a button, a check box, a transparent toolbar, a track
// bar - shows that picture until its WM_PAINT.
//
// So the surface is cleared once, where it is still nobody's: on the first
// WM_NCPAINT / WM_ERASEBKGND of a VCL top-level window after it was shown or
// restored, the transparent base is laid under every pixel whose owner is
// about to repaint it. The owner of a pixel is the topmost visible window
// there; it is about to repaint the pixel when the pixel lies in its update
// region. Nothing else is touched: a window with nothing pending keeps its
// picture, frames are left to WM_NCPAINT, and windows of another thread,
// layered and mirrored ones are left alone.
namespace WndCompat::DelphiShowErase
{
    inline constexpr ULONGLONG kArmedMs = 1000;     // the paint of a show follows it at once
    inline constexpr unsigned kMaxWindows = 2000;
    inline constexpr unsigned kMaxDepth = 32;

    struct State
    {
        HWND root{};
        ULONGLONG armedAt{};
    };
    inline thread_local State t_state;

    // Every window of VCL carries the property VCL finds its object by:
    // 'Delphi%.8X' of the process id (Controls.pas, InitControls).
    [[nodiscard]] inline bool IsVclWindow(HWND hwnd) noexcept
    {
        static const wchar_t* name = []() -> const wchar_t*
        {
            static wchar_t buffer[] = L"Delphi00000000";
            DWORD pid = GetCurrentProcessId();
            for (int i = 13; i >= 6; --i, pid >>= 4)
                buffer[i] = L"0123456789ABCDEF"[pid & 0xF];
            return buffer;
        }();
        return GetPropW(hwnd, name) != nullptr;
    }

    struct Regions
    {
        HRGN total{ CreateRectRgn(0, 0, 0, 0) };    // what is cleared, in the client coordinates of the top-level window
        HRGN part{ CreateRectRgn(0, 0, 0, 0) };
        HRGN bound{ CreateRectRgn(0, 0, 0, 0) };
        Regions() = default;
        Regions(const Regions&) = delete;
        Regions& operator=(const Regions&) = delete;
        ~Regions()
        {
            if (total) DeleteObject(total);
            if (part) DeleteObject(part);
            if (bound) DeleteObject(bound);
        }
        [[nodiscard]] bool Valid() const noexcept { return total && part && bound; }
    };

    struct Walk
    {
        HWND root{};
        DWORD thread{};
        Regions rgn;
        unsigned windows{};         // visited
        unsigned kept{};            // of them, those that keep their picture
        bool failed{};
    };

    // Painter's order: the window, then its children from the bottom of the Z
    // order to its top. Inside its rectangle a window replaces what the windows
    // beneath it contributed. `clip`: what its parents let through.
    inline void Visit(Walk& walk, HWND hwnd, const RECT& clip, unsigned depth) noexcept
    {
        if (++walk.windows > kMaxWindows) { walk.failed = true; return; }
        const bool top = hwnd == walk.root;
        const LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
        if (ex & WS_EX_LAYERED) return;                 // it draws into a surface of its own
        RECT client{};
        if (!GetClientRect(hwnd, &client)) { walk.failed = true; return; }
        RECT window = client;
        if (!top)
        {
            if (!GetWindowRect(hwnd, &window)) { walk.failed = true; return; }
            MapWindowPoints(HWND_DESKTOP, walk.root, reinterpret_cast<POINT*>(&window), 2);
            MapWindowPoints(hwnd, walk.root, reinterpret_cast<POINT*>(&client), 2);
        }
        RECT shown{};
        if (!IntersectRect(&shown, &window, &clip)) return;     // its parents cut all of it away
        // Under this window nothing of the others is cleared...
        if (!SetRectRgn(walk.rgn.bound, shown.left, shown.top, shown.right, shown.bottom)
            || CombineRgn(walk.rgn.total, walk.rgn.total, walk.rgn.bound, RGN_DIFF) == ERROR)
        { walk.failed = true; return; }
        if ((ex & WS_EX_LAYOUTRTL) || depth >= kMaxDepth
            || GetWindowThreadProcessId(hwnd, nullptr) != walk.thread)
        { ++walk.kept; return; }
        RECT inner{};
        if (!IntersectRect(&inner, &client, &clip)) return;
        // ...and what it is about to repaint itself is.
        const int pending = GetUpdateRgn(hwnd, walk.rgn.part, FALSE);
        if (pending == ERROR) { walk.failed = true; return; }
        if (pending == NULLREGION) ++walk.kept;
        else if (OffsetRgn(walk.rgn.part, client.left, client.top) == ERROR
            || !SetRectRgn(walk.rgn.bound, inner.left, inner.top, inner.right, inner.bottom)
            || CombineRgn(walk.rgn.part, walk.rgn.part, walk.rgn.bound, RGN_AND) == ERROR
            || CombineRgn(walk.rgn.total, walk.rgn.total, walk.rgn.part, RGN_OR) == ERROR)
        { walk.failed = true; return; }
        HWND child = GetWindow(hwnd, GW_CHILD);
        if (child) child = GetWindow(child, GW_HWNDLAST);
        for (; child && !walk.failed; child = GetWindow(child, GW_HWNDPREV))
            if (GetWindowLongW(child, GWL_STYLE) & WS_VISIBLE) Visit(walk, child, inner, depth + 1);
    }

    // False: nothing in the window is waiting for a paint yet.
    [[nodiscard]] inline bool Clear(HWND root) noexcept
    {
        RECT client{};
        if (!GetClientRect(root, &client) || IsRectEmpty(&client)) return true;
        Walk walk;
        walk.root = root;
        walk.thread = GetCurrentThreadId();
        if (!walk.rgn.Valid()) return true;
        Visit(walk, root, client, 0);
        RECT box{};
        const int kind = walk.failed ? ERROR : GetRgnBox(walk.rgn.total, &box);
        if (kind > NULLREGION)
        {
            // A DC of the window itself that its children do not clip: they
            // draw into its surface.
            if (const HDC dc = GetDCEx(root, nullptr, DCX_CACHE))
            {
                if (SelectClipRgn(dc, walk.rgn.total) != ERROR)
                {
                    PatBlt(dc, box.left, box.top, box.right - box.left, box.bottom - box.top, BLACKNESS);
                    SelectClipRgn(dc, nullptr);
                }
                ReleaseDC(root, dc);
            }
        }
        return kind != NULLREGION;
    }

    // Watches the messages of a window the root subclass serves. Takes none.
    inline void OnRootMessage(HWND hwnd, UINT message, LPARAM lParam) noexcept
    {
        State& state = t_state;
        switch (message)
        {
        case WM_WINDOWPOSCHANGING:
        {
            // A window that is being shown, or one that leaves the taskbar.
            const auto* pos = reinterpret_cast<const WINDOWPOS*>(lParam);
            if (!pos || (!(pos->flags & SWP_SHOWWINDOW) && !IsIconic(hwnd))
                || GetAncestor(hwnd, GA_ROOT) != hwnd) return;
            state.root = hwnd;
            state.armedAt = GetTickCount64();
            return;
        }
        case WM_NCPAINT:
        case WM_ERASEBKGND:
        {
            if (state.root != hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return;
            // Too late to be the paint of that show, no glass here, a surface
            // or a layout of another kind, not VCL: the window is passed over.
            if (GetTickCount64() - state.armedAt >= kArmedMs
                || WndCompat::WinEventHook_GetMode() != Shared::InjectionMode::Full
                || !GlassShared::GlassIsBackdropAllowedForHwnd(hwnd)
                || (GetWindowLongW(hwnd, GWL_EXSTYLE) & (WS_EX_LAYERED | WS_EX_LAYOUTRTL))
                || !IsVclWindow(hwnd))
            {
                state = State{};
                return;
            }
            // Once. A frame painted before anything was invalidated is not the
            // paint of that show: the window stays armed for it.
            if (Clear(hwnd)) state = State{};
            return;
        }
        case WM_NCDESTROY:
            if (state.root == hwnd) state = State{};
            return;
        default:
            return;
        }
    }
}
