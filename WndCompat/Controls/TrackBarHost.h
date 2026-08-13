#pragma once
#include <windows.h>

namespace WndCompat
{
    // msctls_trackbar32: без субкласса — GlassMarkWindow + TBS_TRANSPARENTBKGND; TBCD_TICS — parent NM_CUSTOMDRAW.
    void TrackBar_Attach(HWND hWnd);
    void TrackBar_Detach(HWND hWnd);
    void TrackBar_DetachAll();
    void TrackBar_ApplyBackdropPolicyToTracked() noexcept;
}
