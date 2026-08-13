#pragma once
#include <windows.h>

namespace WndCompat
{
    // NetUIHWND / UIRibbonCommandBar: маркер GlassControlKind::NetUI — UIRibbon IAT в сессии BeginPaint/EndPaint.
    // NetUI_Attach — из EVENT_OBJECT_CREATE (WindowRole::Control); при родителе «Net UI Tool Window» / Layered вызывает NetUiToolWindow_Attach на оболочке.
    void NetUI_Attach(HWND hWnd);
    void NetUI_Detach(HWND hWnd);
    void NetUI_DetachAll();

    // «Net UI Tool Window» / Layered: маркер NetUiToolWindow + DropDownEventProvider (OnNetUiToolWindow*) → GlassCore::UiRibbon.
    // Не использовать DropDown_Attach/OnDropDownShow для этих HWND — ломает видимость. Attach вызывается с NetUIHWND (родитель/GA_ROOT).
    void NetUiToolWindow_Attach(HWND hWnd);
    void NetUiToolWindow_Detach(HWND hWnd);
    void NetUiToolWindow_DetachAll();
}
