#pragma once
#include "WndPch.h"

namespace WndCompat
{
    // ── Жизненный цикл ───────────────────────────────────────────────────────

    // Подключить сабкласс к окну календаря.
    // parent / parentIsDropDown вычисляются в WinEventHandler и передаются готовыми,
    // чтобы избежать повторного вызова IsWindowClass.
    // Если parentIsDropDown == true, дополнительно выполняется DropDown_Attach(parent)
    // и уведомляется провайдер OnMonthCalAttach.
    void MonthCal_Attach(HWND hWnd, HWND parent, bool parentIsDropDown);
    // Снять сабкласс
    void MonthCal_Detach(HWND hWnd);
    // Снять сабклассы со всех отслеживаемых окон.
    void MonthCal_DetachAll();

    // Проверить, что hWnd уже сабклассирован MonthCalSubclassProc.
    bool MonthCal_IsSubclassed(HWND hWnd);
}