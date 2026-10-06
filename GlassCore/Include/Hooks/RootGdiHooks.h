#pragma once
// GlassCore/include/Hooks/RootGdiHooks.h — внутренний заголовок для root GDI-диспетчера.
// Тела хуков Root_My* и HookDispatcher живут в RootGdiHooks.cpp.
// Классификация окон и провайдеры PaintDispatcher — в RootWindowHooks.cpp.

#include <windows.h>
#include <cstddef>

namespace GlassCore::Root
{
    // Индексы root-хуков (соответствие g_rootHookDep в RootGdiHooks.cpp).
    enum RootHookIndex : size_t
    {
        OHK_DRAWTEXT = 0,
        OHK_DRAWTEXTEX,
        OHK_EXTTEXTOUT,
        OHK_SETBKCOLOR,
        OHK_GETSYSCOLORBR,
        OHK_CREATESOLIDBRUSH,
        OHK_SELECTOBJECT,
        OHK_PATBLT,
        OHK_LINETO,
        OHK_POLYLINE,
        OHK_POLYGON,
        OHK_RECTANGLE,
        OHK_FRAMERECT,
        OHK_BITBLT,
        OHK_GRADIENTFILL,
        OHK_TEXTOUT,
        OHK_FILLRECT,
        OHK_COUNT
    };

    // Включить/выключить один IAT-хук на модуле (ref-counted).
    bool EnableRootHook(void* baseAddress, size_t index, bool enable) noexcept;

    // Безусловно снять все root-хуки (shutdown).
    void DisableAllRootHooks() noexcept;

    // Установить флаг shutdown для тел хуков (вызывается из RootWindowHooks).
    void SetRootGdiShutdown(bool value) noexcept;
}
