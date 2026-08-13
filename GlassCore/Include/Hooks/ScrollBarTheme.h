#pragma once

#include <Windows.h>

// Состояние «внутри цепочного вызова orig DrawThemeBackground для темы ScrollBar» (comctl32 IAT).
// Нужно ListViewHooks (BitBlt/StretchBlt): при активной LV-сессии не обрабатывать перенос из comctl32 как иконки.
// Отрисовка класса темы ScrollBar: ScrollBarDrawTheme.cpp; IAT comctl32 — ScrollBarThemeHooks.

namespace GlassCore::ScrollBarTheme
{
    BOOL WINAPI ComctlScrollBarMyBitBlt(
        HDC hdc, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD rop);

    [[nodiscard]] bool IsInsideScrollBarDrawThemeOrig() noexcept;

    struct DrawThemeOrigCallDepth
    {
        DrawThemeOrigCallDepth() noexcept;
        ~DrawThemeOrigCallDepth() noexcept;
        DrawThemeOrigCallDepth(const DrawThemeOrigCallDepth&)            = delete;
        DrawThemeOrigCallDepth& operator=(const DrawThemeOrigCallDepth&) = delete;
    };

    // Карта HMODULE comctl32 → IAT DrawThemeBackground + BitBlt (лениво из GetDCEx / BeginPaint).
    // callerModule: comctl32 или uxtheme (возврат из GetDCEx в цепочке темизации); IAT ставится на comctl32.
    void PrepareComctlScrollBarIat() noexcept;
    void EnsureComctlScrollBarIatIfComctlCaller(HWND hWnd, HMODULE callerModule) noexcept;
    void OnComctl32ModuleUnload(HMODULE hModule) noexcept;
    void ShutdownComctlScrollBarIat() noexcept;

    [[nodiscard]] decltype(&BitBlt) GetComctlScrollBarBitBltOrig(void* returnAddress) noexcept;
    [[nodiscard]] bool ShouldInstallScrollBarIatForHwnd(HWND hWnd) noexcept;

    // Следующий DrawThemeBackground после ComctlScrollBarMyDrawThemeBackground в карте ScrollBar на comctl32.
    // Overall (TreeView/ListView): для темы ScrollBar использовать вместо chainOrig — иначе звено LV с пустым стеком
    // обходит политику ScrollBarDrawTheme.
    [[nodiscard]] decltype(&DrawThemeBackground) GetComctlScrollBarDrawThemeOrig(HMODULE comctl32Module) noexcept;
}
