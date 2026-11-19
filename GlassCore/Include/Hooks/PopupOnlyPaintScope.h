// PopupOnly-scoping для IAT/detour‑подмен в сессиях PaintDispatcher.
//
// Проблема: IAT-хуки (comctl32/COMDLG32/entrypoint) при активной paint-сессии
// во всём потоке/процессе — чужие окна, переотрисованные ВЛОЖЕННО во время
// нашей сессии (например, адресное поле ComboBoxEx32 при открытии списка),
// получали Black/alpha-кисти и чёрную заливку, хотя сами неприаттачены.
//
// Правила (Full — всегда true; PopupOnly — по совокупности):
//  * HdcSubAllowed(hdc, caller): подмены С HDC (BitBlt/FillRect/PatBlt/SetBkColor/
//      SelectObject/ExtTextOut/DrawText/DrawThemeBackground[Ex]):
//      - нет активной сессии → false (подмен нет),
//      - WindowFromDC(hdc) отвечает host-окном сессии → true,
//      - иначе (чужой оконный DC) → false,
//      - memory-DC без оконного владельца: владелец из стека PaintDispatcher;
//        если известен — обязан совпадать с host; если неизвестен (buffered
//        paint вне нашего CreateCompatibleDC) — true (подмены в общем теле
//        хуков и так темо-класс-скоупнуты).
//  * CallerGlobalSubAllowed(caller): подмены БЕЗ HDC (GetStockObject/
//    GetSysColorBrush/CreateSolidBrush/CreateHatchBrush): в PopupOnly разрешены
//    только owner-draw-модули списка (COMDLG32 / MFC42u / dmdskmgr / dmview /
//    charmap / sysmon) — в отличие от comctl32/user32/gdi32, чьими IAT пользуется
//    и владелец списка, и вложенно-перерисованный чужой контрол (см. Issue 2).
#pragma once

#include <Windows.h>
#include <cwchar>
#include <wchar.h>
#include <GlassControlKind.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/RootSwcaGradient.h>

namespace GlassCore::PopupOnlyPaintScope
{
    // ── Базовые предикаты ────────────────────────────────────────────────────

    [[nodiscard]] inline bool CallerIsComboLBoxOwnerDrawModule(HMODULE caller) noexcept
    {
        if (!caller)
            return false;
        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(caller, path, MAX_PATH) == 0)
            return false;
        const wchar_t* p = wcsrchr(path, L'\\');
        const wchar_t* name = p ? p + 1 : path;
        return _wcsicmp(name, L"COMDLG32.dll") == 0
            || _wcsicmp(name, L"MFC42u.dll") == 0
            || _wcsicmp(name, L"dmdskmgr.dll") == 0
            || _wcsicmp(name, L"dmview.ocx") == 0
            || _wcsicmp(name, L"charmap.exe") == 0
            || _wcsicmp(name, L"sysmon.ocx") == 0;
    }

    // Подмены БЕЗ HDC: в Full — безусловно; в PopupOnly — owner-draw-стеки,
    // но НЕ comctl32/user32/gdi32 (через них перерисовываются и чужие контролы).
    [[nodiscard]] inline bool CallerGlobalSubAllowed(HMODULE caller) noexcept
    {
        if (GlassCore::Root::IsFullInjectionMode())
            return true;
        if (!GlassCore::PaintDispatcher::GetPaintSessionHostHwnd())
            return false;
        return CallerIsComboLBoxOwnerDrawModule(caller);
    }

    // Подмены с HDC: Full — безусловно; PopupOnly — свой оконный DC сессии
    // либо любой memory-DC под активной сессией (внутренности буфера
    // класса «ListBox/ScrollBar/Combobox» — display/ood рисунок списка).
    [[nodiscard]] inline bool HdcSubAllowed(HMODULE /*caller*/, HDC hdc) noexcept
    {
        if (GlassCore::Root::IsFullInjectionMode())
            return true;
        HWND const host = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
        if (!host || !IsWindow(host))
            return false;
        if (!hdc)
            return false;
        if (HWND const dcWnd = ::WindowFromDC(hdc))
            return dcWnd == host;
        // memory-DC: owner из стека сессии (если известен) должен совпадать с host,
        // либо owner отсутствует (buffered paint внутри uxtheme с NULL src) — тогда
        // полагаемся на то, что подмены в общем теле хуков темо-класс-скоупнуты
        // (ListBox part=0 / ScrollBar / Combobox-passthrough) и чужим контролам
        // они не наносят вреда.
        if (HWND const owner = GlassCore::PaintDispatcher::GetStackOwnerForDc(hdc))
            return owner == host;
        return true;
    }

    [[nodiscard]] inline bool HdcSubAllowed(HDC hdc) noexcept
    {
        return HdcSubAllowed(nullptr, hdc);
    }
}
