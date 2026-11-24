#pragma once
#include <windows.h>
#include <WndCompat.h>
#include <wil.h>

namespace WndCompat
{
    // A VCL hint window (exact THintWindow) is attached like a tooltip, but it
    // is painted by VCL, with code of the EXE: the hint colour at the erase, an
    // edge around the window, the tooltip face and the caption in WM_PAINT.
    // GlassCore is asked before VCL sees the message. It answers the erase and
    // the frame itself (the transparent base) - then the message does not go
    // on - and opens a paint session around VCL's own WM_PAINT. When it
    // declines, the message is VCL's as before.
    //
    // Narrow optional bridge, as for the panels: no peer import library and no
    // GlassCoreProviders ABI change. Both callbacks are snapshotted before the
    // paint can re-enter.
    [[nodiscard]] inline bool TryHandleDelphiHintPaint(HWND hwnd, UINT message,
        WPARAM wParam, LPARAM lParam, LRESULT& result)
    {
        if ((message != WM_PAINT && message != WM_ERASEBKGND && message != WM_NCPAINT) || IsShuttingDown())
            return false;

        using BeginFn = BOOL(WINAPI*)(HWND, UINT, HDC) noexcept;
        using EndFn = void(WINAPI*)() noexcept;
        const HMODULE core = GetModuleHandleW(L"GlassCore.dll");
        const auto begin = core ? reinterpret_cast<BeginFn>(GetProcAddress(core, "GlassCore_BeginDelphiHintPaint")) : nullptr;
        const auto end = core ? reinterpret_cast<EndFn>(GetProcAddress(core, "GlassCore_EndDelphiHintPaint")) : nullptr;
        // wParam: a DC for the erase and for a WM_PAINT that is a print, the
        // update region for the frame.
        const HDC dc = message == WM_NCPAINT ? nullptr : reinterpret_cast<HDC>(wParam);
        if (!begin || !end || !begin(hwnd, message, dc))
            return false;
        auto finish = wil::scope_exit([end]() noexcept { end(); });
        // WM_ERASEBKGND: erased. WM_NCPAINT: processed.
        result = message == WM_PAINT ? DefSubclassProc(hwnd, message, wParam, lParam)
            : message == WM_ERASEBKGND ? 1 : 0;
        return true;
    }
}
