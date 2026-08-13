#pragma once
#include <windows.h>
#include <cstdint>
#include <Subsystems/Settings.h>

namespace GlassCore::FlyoutFrame
{
    enum class FlyoutHostBorderFlush : uint8_t
    {
        None = 0,
        FrameOnly,
        Full,
    };

    void PaintHostBorder(
        HWND hwnd,
        bool darkMode,
        const Settings::BorderContext& border,
        unsigned effectType,
        FlyoutHostBorderFlush flush = FlyoutHostBorderFlush::Full) noexcept;

    /// Per-popup HWND: NC-рамка отложена до uncloak flyout-анимации (Menu, DropDown, ComboLBox, ViewControl).
    void SetHostBorderDeferred(HWND hWnd, bool deferred) noexcept;
    [[nodiscard]] bool IsHostBorderDeferred(HWND hWnd) noexcept;
    void ClearHostBorderDeferred(HWND hWnd) noexcept;

    /// Синхронный black NC до cloak / перед SWCA (GUI-поток flyout).
    void SyncPrimeFlyoutNcBlack(HWND hWnd) noexcept;

    /// Post-uncloak: однократная отрисовка рамки (sync, FrameOnly по умолчанию).
    void PaintDeferredHostBorder(
        HWND hWnd,
        bool darkMode,
        const Settings::BorderContext& border,
        unsigned effectType,
        FlyoutHostBorderFlush flush = FlyoutHostBorderFlush::FrameOnly) noexcept;
}
