#pragma once
#include <windows.h>
#include <vector>

namespace GlassCore::Root
{
    // Спецификация одного IAT-хука: модуль + индекс в RootHookIndex.
    struct HookSpec { void* module; size_t index; };

    // Регистрация провайдеров PaintDispatcher (вызывать до PaintDispatcher::Startup).
    void Startup() noexcept;

    // Классификация окна и кеширование спецификаций хуков (без включения).
    void OnRootHooksAttach(HWND hRoot) noexcept;

    // Удаление из кеша (хуки уже выключены — per-session модель).
    void OnRootHooksDetach(HWND hRoot) noexcept;

    void Shutdown() noexcept;

    // Добавить paint-спеки к кешу окна (DirectUI — DUser+wdc).
    void AppendRootPaintSpecs(HWND hRoot, std::vector<HookSpec> specs) noexcept;

    // DUser.dll + wdc: LineTo / Polyline / Polygon / PatBlt (вызывается из DirectUIHooks).
    void GetDUserWdcPaintSpecs(void* duserModule, std::vector<HookSpec>& out) noexcept;

    // IEUI.dll + DirectUIHWND → IEFRAME (GetSysColorBrush / DrawTextEx / DrawText), как BWM PaintHandler.
    void GetIeUiDirectUiPaintSpecs(std::vector<HookSpec>& out) noexcept;

    // DirectUI_Attach: наполнить s_rootCache для HWND без Root-сабкласса (IEUI→IEFRAME или DUser+wdc).
    void AppendDirectUiRootPaintSpecsForHwnd(HWND hWnd) noexcept;
}
