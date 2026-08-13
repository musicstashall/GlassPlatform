#pragma once
#include "framework.h"

// ---------------------------------------------------------------------------
// Константы LogViewer — контролы, ленты рябара, размеры
// ---------------------------------------------------------------------------

// Контролы главного окна
static constexpr UINT IDC_LIST = 100;
static constexpr UINT IDC_SEARCH = 101;   // Edit поиска (скрыт; поверх кнопки IDC_SEARCH_BTN)
static constexpr UINT IDC_FIND_NEXT = 102;   // используется F3
static constexpr UINT IDC_FIND_PREV = 103;   // используется Shift+F3
static constexpr UINT IDC_CLEAR = 104;
static constexpr UINT IDC_INVERT = 111;
// ComboBox фильтров заменены на popup + ListView (см. Ui/MainWindow/LogViewerFilterPopup.cpp)
static constexpr UINT IDC_STATUS = 110;
static constexpr UINT IDC_TABCTRL = 112;

// SysTabControl32: единый стиль главного окна и диалога «Параметры».
static constexpr DWORD LOGVIEWER_TABCTRL_STYLE = 0x54010440;

// Рябар и тулбары (3 ленты)
static constexpr UINT IDC_REBAR = 113;
static constexpr UINT IDC_CTRL_TB = 114;  // Band 0: Start/Stop + Pause (BAND_H)
static constexpr UINT IDC_FILTER_TB = 115;  // Band 1: Proc, Mod, Clear, Invert, Levels (CTRL_TB_H, Rebar центрирует)
/// Контейнер среднего тулбара в ленте Rebar (дочерний HWND — сам тулбар фильтров).
static constexpr UINT IDC_FILTER_TB_HOST = 135;
static constexpr UINT IDC_SEARCH_TB = 130;  // Band 2: кнопка поиска (растягивается, BAND_H)

// Кнопки Band 0
static constexpr UINT IDC_START_STOP = 118;
static constexpr UINT IDC_PAUSE_BTN = 119;
static constexpr UINT IDC_GLASS_SETTINGS = 134; // Параметры отрисовки Glass (реестр)

// Кнопки Band 1 (фильтры)
static constexpr UINT IDC_PROC_BTN = 128;
static constexpr UINT IDC_MOD_BTN = 129;
static constexpr UINT IDC_SUB_BTN = 133;
static constexpr UINT IDC_LVL_BASE = 120;  // 120..124 — N/I/W/E/C

// Кнопка поиска Band 2
static constexpr UINT IDC_SEARCH_BTN = 131;

// Контекстное меню
static constexpr UINT IDM_COPY = 200;
static constexpr UINT IDM_COPY_ALL = 201;
static constexpr UINT IDM_CLEAR = 202;
static constexpr UINT IDM_INVERT_SEL = 203;
static constexpr UINT IDM_COPY_CELL = 205;
static constexpr UINT IDM_SAVE_PROCESS_DUMP = 206;

// ---------------------------------------------------------------------------
// Индексы лент Rebar
// ---------------------------------------------------------------------------
static constexpr int BAND_CTRL = 0;  // Start/Stop + Pause
static constexpr int BAND_FILTER = 1;  // Proc, Mod, Clear, Invert, Levels (центрируется)
static constexpr int BAND_SEARCH = 2;  // кнопка поиска (растягивается)

// ---------------------------------------------------------------------------
// Размеры
// ---------------------------------------------------------------------------
static constexpr int BAND_H = 42;  // высота ленты рябара (запас по вертикали для подписей тулбара)
static constexpr int CTRL_TB_H = 28;  // высота кнопок Band 1 (вертикально центрируется в BAND_H)
static constexpr int STATUS_H = 22; // msctls_statusbar32 — типичная высота по шрифту
static constexpr int MIN_SEARCH_W = 60;  // минимальная ширина ленты поиска
/// Добавка к минимальной ширине средней ленты (к оценке TB_GETMAXSIZE + 4).
static constexpr int BAND_FILTER_MIN_EXTRA_PX = 10;
/// Ширина Edit поиска относительно прямоугольника кнопки: уменьшение на эту величину.
static constexpr int SEARCH_EDIT_WIDTH_TRIM_PX = 5;

// Ширина кнопок-триггеров и выпадающих списков фильтров (текст на кнопках — ellipsis, см. UiRebarSetup)
static constexpr int PROC_CBX_W = 200;
static constexpr int MOD_CBX_W = 160;
static constexpr int SUB_CBX_W = 140;

// ---------------------------------------------------------------------------
// Таймеры
// ---------------------------------------------------------------------------
static constexpr UINT_PTR IDT_STOP_POLL = 1;      // опрос завершения при нажатии Stop (500 мс)
static constexpr UINT_PTR IDT_PLATFORM_POLL = 3;  // периодический опрос состояния платформы (1 сек)
static constexpr UINT_PTR IDT_SEARCH = 2;         // задержка поиска при вводе (1 сек)
static constexpr UINT_PTR IDT_LOGVIEWER_WINEVENT_PING = 4; // NotifyWinEvent на главное окно после старта хоста
/// Бывший IDT_FILTER_DROPDOWN_POST: KillTimer в CloseFilterPopup / WM_DESTROY (таймер больше не ставится).
static constexpr UINT_PTR IDT_FILTER_TOOLBAR_LEGACY = 5;
/// Коалесцируемый дренаж очереди LogEntry (не цепочка PostMessage — окно для ввода между тиками).
static constexpr UINT_PTR IDT_LOG_DRAIN = 6;
/// Интервал между тиками дренажа логов (мс); SetTimer с тем же id схлопывает предыдущий вызов.
static constexpr UINT kLogDrainCoalesceMs = 16;