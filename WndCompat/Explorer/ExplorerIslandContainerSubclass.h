#pragma once
#include <windows.h>

namespace WndCompat
{
    // Устанавливает сабкласс на контейнер WinUI острова (SHELLDLL_DefView, ShellTabWindowClass).
    // Выполняет GDI-подготовку фона в WM_PAINT, WM_PRINT, WM_PRINTCLIENT.
    void ExplorerIslandContainer_Attach(HWND hWnd);

    // Принудительное снятие сабклассов (опционально, при shutdown).
    void ExplorerIslandContainer_DetachAll();

    void ExplorerIslandContainer_ApplyBackdropPolicyToTracked() noexcept;
}
