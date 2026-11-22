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
        OHK_DELPHI_DRAWTHEMETEXTEX,
        OHK_DELPHI_GRID_TEXT,
        OHK_DELPHI_GRID_FILL,
        OHK_DELPHI_GRID_THEME,
        OHK_GETSYSCOLOR,
        OHK_DELPHI_GROUP_TEXT,
        OHK_DELPHI_GROUP_FILL,
        OHK_DELPHI_GROUP_GRADIENT,
        OHK_DELPHI_GROUP_GDIGRADIENT,
        OHK_BOME_MEMO_TEXT,
        OHK_BOME_MEMO_BRUSH,
        OHK_BOME_MEMO_COLOR,
        OHK_BOME_MEMO_FILL,
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
        OHK_VCLPROBE_FILL, OHK_VCLPROBE_PATBLT, OHK_VCLPROBE_RECT,
        OHK_VCLPROBE_ROUNDRECT, OHK_VCLPROBE_BITBLT, OHK_VCLPROBE_TEXT,
        OHK_VCLPROBE_DRAWTEXT, OHK_VCLPROBE_THEME,
        OHK_VCLPROBE_CREATEBRUSH, OHK_VCLPROBE_SELECT,
        OHK_VCLPROBE_ELLIPSE, OHK_VCLPROBE_DRAWTEXTEX,
        OHK_VCLPROBE_CREATEBRUSHINDIRECT, OHK_VCLPROBE_THEMETEXTEX,
        OHK_VCLPROBE_STRETCHBLT, OHK_VCLPROBE_FOCUSRECT,
#endif
        OHK_COUNT
    };

#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
    // Сессионные passthrough-пробы EXE для точных VCL классов (opt-in).
    void BeginVclDrawProbe(HWND hWnd, bool paint) noexcept;
    void EndVclDrawProbe(HWND hWnd, bool paint) noexcept;
    void ClearVclDrawProbe() noexcept;
#endif

    // Включить/выключить один IAT-хук на модуле (ref-counted).
    bool EnableRootHook(void* baseAddress, size_t index, bool enable) noexcept;

    // Four dedicated EXE imports are acquired transactionally. A partial mask
    // is diagnostic only: all refs acquired by the failed attempt are undone.
    unsigned AcquireBomeMemoHooks() noexcept;
    void ReleaseBomeMemoHooks() noexcept;

    // Предпрогрев IAT-кеша модуля (только чтение: emplace диспетчера + CacheHookData,
    // без записей и без refcount). Убирает холодный скан PE из первого paint-кадра.
    void PrewarmRootHook(void* baseAddress) noexcept;

    // Безусловно снять все root-хуки (shutdown).
    void DisableAllRootHooks() noexcept;

    // Регистрация BeginPaint/EndPaint-провайдера реконструкции фона таблиц.
    void RegisterDelphiGridPaintProvider() noexcept;

    // Установить флаг shutdown для тел хуков (вызывается из RootWindowHooks).
    void SetRootGdiShutdown(bool value) noexcept;
}
