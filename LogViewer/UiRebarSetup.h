#pragma once
#include "framework.h"
#include <commctrl.h>
#include <Uxtheme.h>

// ---------------------------------------------------------------------------
// Вспомогательные функции для создания рябара и тулбаров LogViewer
// Все тулбары: стиль 0x5600B849, SetWindowTheme(L"Explorer").
// ---------------------------------------------------------------------------

// Иконки 16×16: старт/стоп, «Параметры отрисовки», фильтры (круги/квадраты).
HIMAGELIST CreateLogViewerToolbarImages(int cx, int cy);

void AddRebarBand(HWND hRebar, HWND hChild,
    const wchar_t* pszText,
    int cx,
    int cyChild,
    int cyBand,
    DWORD extraStyle);

// Band 0: Start/Stop + Pause. Кнопки BAND_H.
HWND CreateCtrlToolbar(HWND hwndParent, HIMAGELIST hImgList, HFONT hFont);

// Band 1: Proc, Mod, Clear, Invert, Levels. Rebar центрирует по вертикали.
HWND CreateFilterToolbar(HWND hwndParent, HIMAGELIST hImgList, HFONT hFont);

// Band 2: одна кнопка «Поиск в документе», BAND_H (растягивается).
HWND CreateSearchToolbar(HWND hwndParent, HFONT hFont);