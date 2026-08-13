#pragma once
#include <windows.h>

namespace GlassCore::Tooltip
{
    void Prepare();
    void Startup();
    void Shutdown();
    void Update();

    void OnTooltipCreated(HWND hWnd);
    void OnTooltipDestroyed(HWND hWnd);

    // Коллбэки для WndCompat::TooltipEventProvider / Exports.cpp
    [[nodiscard]] bool IsTooltipIatReady() noexcept;
    [[nodiscard]] bool IsTooltipCustomizationDisabledForAttach() noexcept;

    void OnTooltipSubclassAttached(HWND hWnd);
    void OnTooltipSubclassDetached(HWND hWnd, BOOL isShutdown);

    [[nodiscard]] bool OnTooltipPaintSessionBegin(HWND hWnd);
    void OnTooltipPaintSessionEnd(HWND hWnd);

    void OnKbxLabelSubclassAttached(HWND hWnd);
    void OnKbxLabelSubclassDetached(HWND hWnd, BOOL isShutdown);

    void OnKbxLabelPaintSessionBegin(HWND hWnd);
    void OnKbxLabelPaintSessionEnd(HWND hWnd);

    void OnSubsystemUpdate();
}
