#pragma once
#include <windows.h>
#include <GlassDelphiPanel.h>
#include <GlassBackdropMark.h>
#include <Tracker/WinEventHandler.h>
#include <InjectionMode.h>
#include <WndCompat.h>
#include <wil.h>

namespace WndCompat
{
    // Narrow optional bridge: no peer import library or GlassCoreProviders ABI
    // change. Both callbacks are snapshotted before native paint can re-enter.
    [[nodiscard]] inline bool TryHandleDelphiPanelPaint(HWND hwnd, UINT message,
        WPARAM wParam, LPARAM lParam, LRESULT& result)
    {
        if ((message != WM_PAINT && message != WM_ERASEBKGND && message != WM_PRINTCLIENT)
            || IsShuttingDown() || WinEventHook_GetMode() != Shared::InjectionMode::Full
            || !GlassShared::GlassIsBackdropAllowedForHwnd(hwnd)
            || !GlassShared::IsDelphiPanelWindow(hwnd))
            return false;

        using BeginFn = BOOL(WINAPI*)(HWND, UINT, HDC) noexcept;
        using EndFn = void(WINAPI*)() noexcept;
        const HMODULE core = GetModuleHandleW(L"GlassCore.dll");
        const auto begin = core ? reinterpret_cast<BeginFn>(GetProcAddress(core, "GlassCore_BeginDelphiPanelPaint")) : nullptr;
        const auto end = core ? reinterpret_cast<EndFn>(GetProcAddress(core, "GlassCore_EndDelphiPanelPaint")) : nullptr;
        if (!begin || !end || !begin(hwnd, message, reinterpret_cast<HDC>(wParam)))
            return false;
        auto finish = wil::scope_exit([end]() noexcept { end(); });
        result = DefSubclassProc(hwnd, message, wParam, lParam);
        return true;
    }
}
