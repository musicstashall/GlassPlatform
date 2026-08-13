#pragma once
#include <windows.h>

namespace GlassCore::UiRibbon
{
    // NetUIHWND → GlassControlKind::NetUI; оболочка ленты → NetUiToolWindow (маркер в NetUISubclass, attach с NetUIHWND в WinEvent).
    // IAT только UIRibbon.dll (имя файла проверяется), одна сессия BeginPaint/EndPaint для обоих kind.
    // Без хука SelectObject в UxTheme.dll (ломал шрифты процесса).
    //
    // «Net UI Tool Window» / Layered: ApplyEffect через OnNetUiToolWindow* — не DropDown_Attach (OnDropDownShow ломает видимость).
    void OnNetUiToolWindowHooksAttach(HWND hWnd) noexcept;
    void OnNetUiToolWindowAttach(HWND hWnd) noexcept;
    void OnNetUiToolWindowDetach(HWND hWnd) noexcept;

    void Startup() noexcept;
    void Shutdown() noexcept;
}
