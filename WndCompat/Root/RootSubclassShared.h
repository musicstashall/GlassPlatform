#pragma once
#include <Windows.h>
#include <CommCtrl.h>

// Диспетчер в RootSubclassProcShared (только обычный root). Проводник — ExplorerRootSubclass.cpp.

namespace WndCompat::Root::Detail
{
    LRESULT CALLBACK RootSubclassSharedDispatch(
        HWND hWnd,
        UINT uMsg,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR uIdSubclass,
        DWORD_PTR dwRefData,
        SUBCLASSPROC selfProc);
}
