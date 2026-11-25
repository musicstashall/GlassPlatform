#pragma once
#include <windows.h>

namespace GlassCore::BomeMemo
{
    // The root dispatcher owns these four EXE slots as one paint/GetDC route.
    inline constexpr unsigned kReadyMask = 0xF; // Text=1, BrushIndirect=2, SysColor=4, Fill=8.
    // Exact TBomeMemo / TBMGutter whose HINSTANCE is the process EXE.
    // Not an alias for Edit/TMemo and not tied to a process name.
    bool IsWindow(HWND hwnd) noexcept;
    // Record a native barrier too when acquisition failed. IAT is process-wide,
    // whereas readiness belongs to this thread's sessions. Sessions of the memo
    // and of its gutter stay open side by side (VCL keeps canvas DCs until the
    // message ends): a call is served by the DC it draws on, not by the session
    // that opened last.
    void BeginSession(HWND hwnd, bool paint, unsigned mask) noexcept;
    bool EndSession(HWND hwnd, bool paint) noexcept; // true: release the four acquired refs.
    void ClearSessions() noexcept;
    bool AfterBeginPaint(HWND hwnd, HDC dc, LPPAINTSTRUCT ps, HMODULE caller) noexcept;
    int DrawAnsi(decltype(&::DrawTextA) original, HDC dc, LPCSTR text,
        int count, LPRECT rect, UINT flags);
    HBRUSH CreateBrush(decltype(&::CreateBrushIndirect) original, const LOGBRUSH* brush);
    DWORD SystemColor(decltype(&::GetSysColor) original, int index);
    int Fill(decltype(&::FillRect) original, HDC dc, const RECT* rect, HBRUSH brush);
}
