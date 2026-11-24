#pragma once
#include <windows.h>

namespace GlassCore::DelphiHint
{
    // Exact THintWindow, the window VCL shows a control's Hint in. The tooltip
    // subsystem already gives it the backdrop; VCL paints it itself, with code
    // of the EXE: the hint colour at the erase, an edge around the window, the
    // themed tooltip face and the caption in WM_PAINT. Here the erase and the
    // edge are answered with the transparent base, and the caption is drawn
    // with alpha in the tooltip text colour over a client cleared once per
    // paint. Nothing is changed for another window, DC or call.
    void Startup() noexcept;
    void Shutdown() noexcept;

    // The tooltip subclass was attached to `hwnd`. For a VCL hint window the
    // imports of the EXE are taken, once, and held until Shutdown.
    void OnAttached(HWND hwnd) noexcept;

    // A message of a VCL hint window, before VCL sees it.
    //   WM_PAINT (dc == nullptr): true - a paint session is open; the caller
    //     lets VCL paint and then calls EndPaint.
    //   WM_ERASEBKGND (dc - the DC of the message), WM_NCPAINT: true - drawn
    //     here; the caller answers the message itself and calls EndPaint.
    // false: the message is VCL's, and EndPaint is not to be called.
    [[nodiscard]] bool BeginPaint(HWND hwnd, UINT message, HDC dc) noexcept;
    void EndPaint() noexcept;
}
