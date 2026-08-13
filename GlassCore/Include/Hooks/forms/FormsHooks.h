#pragma once

namespace GlassCore::FormsHooks
{
    void Startup() noexcept;
    void Shutdown() noexcept;

    // BWM FormsHooks::ColorBrushAttach — глобальные Detours при загрузке System.Windows.Forms.ni.dll.
    void ColorBrushAttach(bool enable) noexcept;

    // BWM FormsHooks::DrawTextExAttach — ref-count, Detour user32!DrawTextExW.
    void DrawTextExAttach(bool enable) noexcept;

    // Единый Detour uxtheme!DrawThemeBackground — ref-count, диспетчер политик контролов.
    void DrawThemeBackgroundAttach(bool enable) noexcept;
}