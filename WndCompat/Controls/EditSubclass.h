#pragma once
#include "WndPch.h"
#include <GlassControlKind.h>

namespace WndCompat
{
    // Edit (Edit, TEdit, TMemo, .EDIT, ATL:EDIT)
    void Edit_Attach(HWND hWnd);
    void Edit_Detach(HWND hWnd);
    void Edit_DetachAll();

    // RichEdit (RichEdit20W, RICHEDIT50W, WinForms-варианты)
    void RichEdit_Attach(HWND hWnd, GlassShared::GlassControlKind kind);
    void RichEdit_Detach(HWND hWnd);
    void RichEdit_DetachAll();
}
