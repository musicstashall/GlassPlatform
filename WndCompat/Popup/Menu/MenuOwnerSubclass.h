#pragma once
#include "WndPch.h"

namespace WndCompat
{
    LRESULT MenuOwnerSubclass(
        HWND hWnd,
        UINT uMsg,
        WPARAM wParam,
        LPARAM lParam
    );

    // BWM WinHandler: classic menu bar — ExtTextOut вокруг WM_NC* (см. MenuOwnerSubclass.cpp).
    [[nodiscard]] bool MenuOwner_HandleClassicMenuBarNcActivate(
        HWND hWnd, WPARAM wParam, LPARAM lParam, LRESULT& outResult) noexcept;

    [[nodiscard]] bool MenuOwner_HandleClassicMenuBarNcPaint(
        HWND hWnd, WPARAM wParam, LPARAM lParam, LRESULT& outResult) noexcept;

    [[nodiscard]] bool MenuOwner_HandleClassicMenuBarNcHitTest(
        HWND hWnd, WPARAM wParam, LPARAM lParam, LRESULT& outResult) noexcept;

    void MenuOwner_OnClassicMenuBarEnterIdle(HWND hWnd, WPARAM wParam, LPARAM lParam) noexcept;
    void MenuOwner_OnClassicMenuBarNcMouseLeave(HWND hWnd) noexcept;
    void MenuOwner_ReleaseClassicMenuBarHold(HWND hWnd) noexcept;
}
