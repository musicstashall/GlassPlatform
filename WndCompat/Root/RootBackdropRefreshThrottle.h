#pragma once

#include <Windows.h>

namespace WndCompat::RootBackdropRefreshThrottle
{
    constexpr UINT kDefaultThrottleMs = 100;

    /// Сброс троттлинга при detach root (WM_NCDESTROY / Detach).
    void Erase(HWND hRoot) noexcept;

    /// true — пропустить повторный SWCA refresh (слишком рано после предыдущего).
    [[nodiscard]] bool ShouldSkip(HWND hRoot, UINT throttleMs = kDefaultThrottleMs) noexcept;

    /// Отметить успешное применение (вызывать после OnRootSwca*).
    void Stamp(HWND hRoot) noexcept;

    /// Одноразовая метка reason= для следующего ApplyRootSwcaBackdrop в GlassCore.dll.
    void SetRootSwcaApplyReasonForNextCall(const wchar_t* reason) noexcept;
}
