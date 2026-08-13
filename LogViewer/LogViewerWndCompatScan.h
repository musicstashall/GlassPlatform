#pragma once
#include <Windows.h>

namespace LogViewerWndCompatScan
{
    /// Синтетический обход дерева (EVENT_OBJECT_CREATE), как GlassSettingsWndCompatScan.
    /// @param hInkPreviewHostPageOpt Опционально: дочерняя страница с Edit под ink-превью (как страница «Текст» в GlassSettings).
    void AttachLogViewerWindowTree(HWND hRoot, HWND hInkPreviewHostPageOpt) noexcept;

    /// Новые HWND после первичного скана (например ListView на новой вкладке).
    void AttachLogViewerControlSubtree(HWND hControlRoot, HWND hInkPreviewHostPageOpt) noexcept;
}
