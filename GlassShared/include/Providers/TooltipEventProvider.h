#pragma once
#include <windows.h>

namespace WndCompat::Tooltip
{
    // Коллбэки GlassCore для субклассов тултипа / KbxLabel (граница как у MenuEventProvider).
    struct TooltipEventProvider
    {
        bool (*IsTooltipIatReady)() = nullptr;
        bool (*IsTooltipCustomizationDisabled)() = nullptr;

        void (*OnTooltipSubclassAttached)(HWND hWnd) = nullptr;
        void (*OnTooltipSubclassDetached)(HWND hWnd, BOOL isShutdown) = nullptr;

        // WM_PAINT / WM_PRINTCLIENT: Begin до DefSubclassProc, End после.
        // Begin возвращает true — WndCompat рассылает WM_THEMECHANGED/TTM_UPDATE остальным тултипам.
        bool (*OnTooltipPaintSessionBegin)(HWND hWnd) = nullptr;
        void (*OnTooltipPaintSessionEnd)(HWND hWnd) = nullptr;

        void (*OnKbxLabelSubclassAttached)(HWND hWnd) = nullptr;
        void (*OnKbxLabelSubclassDetached)(HWND hWnd, BOOL isShutdown) = nullptr;

        void (*OnKbxLabelPaintSessionBegin)(HWND hWnd) = nullptr;
        void (*OnKbxLabelPaintSessionEnd)(HWND hWnd) = nullptr;

        void (*OnSubsystemUpdate)() = nullptr;
    };

    void SetTooltipEventProvider(const TooltipEventProvider& provider);
    const TooltipEventProvider* GetTooltipEventProvider();
}
