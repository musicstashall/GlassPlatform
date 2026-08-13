#pragma once
// Единая точка «аттача» LogViewer к GlassCore (как WndCompat *_Attach, без сабклассов):
// SysTabControl32 — GlassMarkWindow + Glass.TabUseTheme (визуальные стили не отключаем, GCLP_HBRBACKGROUND не трогаем);
// Button — GlassMarkWindow + списки и Detach*;
// остальные контролы — только GlassMarkWindow.

#include <Windows.h>
#include <GlassControlKind.h>

namespace LogViewerGlassAttach
{
    void AttachTabControl(HWND hWnd) noexcept;
    // После AttachTabControl: сброс отрисовки (до аттача comctl32 мог нарисовать таб без сессии TabHooks).
    void RequestTabControlGlassRepaint(HWND hTabCtrl) noexcept;
    void AttachButton(HWND hWnd, GlassShared::GlassControlKind kind = GlassShared::GlassControlKind::Button) noexcept;

    void AttachRebar(HWND hWnd) noexcept;
    void AttachToolbar(HWND hWnd) noexcept;
    void AttachEdit(HWND hWnd) noexcept;
    void AttachListView(HWND hWnd) noexcept;
    // SysHeader32 дочерний у ListView — GlassCore HeaderHooks; обычно вызывается из AttachListView.
    void AttachHeader(HWND hWnd) noexcept;
    void AttachStatic(HWND hWnd) noexcept;
    void AttachStatusBar(HWND hWnd) noexcept;
    void AttachComboBox(HWND hWnd) noexcept;
    void AttachPreviewEdit(HWND hWnd) noexcept;

    void DetachTabControl(HWND hWnd) noexcept;
    void DetachButton(HWND hWnd) noexcept;

    void DetachChildrenInDialog(HWND hwndParent, HWND hPreview) noexcept;

    void ShutdownCleanup() noexcept;
}
