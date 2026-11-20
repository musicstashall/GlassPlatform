#pragma once
#include <windows.h>

namespace GlassCore::Button
{
    // Lifecycle
    void Prepare();
    void Startup();
    void Shutdown();

    /// Переарм IAT-хуков текста с glow после reload GlobalTextGlow из реестра.
    void SyncTextGlowHooksFromRegistry() noexcept;

    // Узкая сессия WM_DRAWITEM exact TBitBtn; начало/конец симметричны.
    bool BeginDelphiBitBtnOwnerDraw(HWND parent, const DRAWITEMSTRUCT* dis) noexcept;
    void EndDelphiBitBtnOwnerDraw() noexcept;

    // Для проверки контекста отрисовки кнопок из других хуков
    bool IsInsideButtonPaint();
    bool IsInsideWinFormsButtonPaint();
}
