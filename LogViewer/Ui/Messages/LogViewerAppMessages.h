#pragma once

#include <windows.h>
#include <commctrl.h>

// Пользовательские сообщения главного окна LogViewer
#define WM_APP_SHOW_EDIT    (WM_APP + 1)
#define WM_APP_HIDE_EDIT    (WM_APP + 2)
#define WM_APP_CLOSE_FILTER (WM_APP + 3)
/// Переключить открытый фильтр на другой kind (0..2) при наведении на кнопку тулбара (как адресная строка проводника).
#define WM_APP_SWITCH_FILTER (WM_APP + 4)
/// После возврата из TBN_DROPDOWN отложенный сброс pressed/hot (PostMessage), если comctl32 ещё донастроит состояние.
#define WM_APP_FILTER_TOOLBAR_POST_SYNC (WM_APP + 5)
/// Вторая фаза после hot-switch: открыть дроп kind после возврата TBDDRET_DEFAULT из TBN_DROPDOWN (как WorkerW.au3 после WM_CLOSE дропа).
#define WM_APP_FILTER_RESUME_DROPDOWN (WM_APP + 7)
/// Слияние очереди разрешённых имён PID из фонового потока в m_pidLabels.
#define WM_APP_PID_LABELS_FLUSH (WM_APP + 8)
/// Завершение фоновой записи mini dump (lParam → LogViewerProcessDumpResult*, освободить в обработчике).
#define WM_APP_PROCESS_DUMP_DONE (WM_APP + 9)

#ifndef LVN_ENDSCROLL
#define LVN_ENDSCROLL (LVN_FIRST - 81)
#endif

#ifndef WM_DWMCOLORIZATIONCHANGED
#define WM_DWMCOLORIZATIONCHANGED 0x0320
#endif
