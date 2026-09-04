#pragma once

namespace GlassCore::FormsHooks
{
    void Startup() noexcept;
    void Shutdown() noexcept;

    // BWM FormsHooks::ColorBrushAttach — глобальные Detours при загрузке System.Windows.Forms.ni.dll.
    void ColorBrushAttach(bool enable) noexcept;

    // BWM FormsHooks::DrawTextExAttach — ref-count, Detour user32!DrawTextExW.
    void DrawTextExAttach(bool enable) noexcept;

    // BWM FormsHooks::DrawTextAttach — ref-count, Detour user32!DrawTextW (AMCCustomTab + MFC42u.dll).
    void DrawTextAttach(bool enable) noexcept;

    // Единый Detour uxtheme!DrawThemeBackground — ref-count, диспетчер политик контролов.
    void DrawThemeBackgroundAttach(bool enable) noexcept;

    // Internal: PaintDispatcher провайдер для AMCCustomTab (вызывается из Startup).
    void RegisterCustomTabPaintProvider() noexcept;

    // Internal: принудительное снятие DrawTextW детура при Shutdown.
    void ForceDetachDrawText() noexcept;

    // ── mmc.exe Detours (always-on, entire process lifetime) ─────────────────
    // BWM: mmcHooks::MySetBkColor, MyFillRect, MyExtTextOutW — глобальные Detours,
    // активные для всего процесса mmc.exe.

    // BWM mmcHooks::MySetBkColor — перехват SetBkColor: COLOR_WINDOW/COLOR_BTNFACE → black.
    void MmcSetBkColorAttach(bool enable) noexcept;

    // BWM mmcHooks::MyFillRect — перехват FillRect: dmdskmgr.dll/dmview.ocx → alpha brushes.
    void MmcFillRectAttach(bool enable) noexcept;

    // BWM mmcHooks::MyExtTextOutW — перехват ExtTextOutW:mmc snap-in модули → alpha text.
    void MmcExtTextOutAttach(bool enable) noexcept;

    // Принудительное снятие всех mmc.exe детуров при Shutdown.
    void ForceDetachAllMmcHooks() noexcept;
}