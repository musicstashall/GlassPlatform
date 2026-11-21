#pragma once
#include "WndPch.h"

namespace WndCompat
{
    // Единая точная проверка для классификатора, attach/catch-up и рескана.
    // TCoolBar подтверждён зондом GetClassNameW; CoolBar — прежний точный алиас.
    [[nodiscard]] bool CoolBar_IsClassName(const wchar_t* className) noexcept;

    // Сабкласс охватывает native WM_PAINT/WM_PRINTCLIENT целиком, включая
    // печать в переданный HDC без BeginPaint/GetDC. Маркера Host недостаточно.
    void CoolBar_Attach(HWND hWnd);
    void CoolBar_Detach(HWND hWnd);
    void CoolBar_DetachAll();
    [[nodiscard]] bool CoolBar_IsTracked(HWND hWnd) noexcept;

    // Догоняющий проход по поддереву root-окна: окна TCoolBar / CoolBar,
    // созданные до инжекции, иначе остаются без сабкласса.
    void CoolBar_CatchUp(HWND hRoot) noexcept;
}
