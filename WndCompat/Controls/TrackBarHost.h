#pragma once
#include <windows.h>

namespace WndCompat
{
    // msctls_trackbar32: маркер GlassMarkWindow + TBS_TRANSPARENTBKGND + (opacity-субкласс
    // TrackBarSubclassProc для WM_ERASEBKGND); TBCD_TICS — parent NM_CUSTOMDRAW (ViewControlSubclassProc).
    void TrackBar_Attach(HWND hWnd);
    void TrackBar_Detach(HWND hWnd);
    void TrackBar_DetachAll();
    void TrackBar_ApplyBackdropPolicyToTracked() noexcept;
}
