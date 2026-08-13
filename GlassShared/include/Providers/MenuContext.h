#pragma once
#include <dwmapi.h>

namespace WndCompat::Menu
{
    enum class MenuType
    {
        Unknown = 0,
        /// Плейсхолдер до PrintWindow + WM_UAHINITMENU (как BWM ClassicOrNativeTheme).
        ClassicOrNativeTheme,
        Classic,
        NativeTheme,
        Immersive,
        TraverseLog,
        ThirdParty
    };

    struct MenuCustomRendering
    {
        bool enable = false;

        bool separator_disabled = false;
        DWORD separator_cornerRadius = 0;
        DWORD separator_color = 0x30D9D9D9;
        DWORD separator_width = 1000;

        bool focusing_disabled = false;
        DWORD focusing_cornerRadius = 8;
        DWORD focusing_color = 0xFFFFFFFF;
        DWORD focusing_width = 1000;

        bool disabledHot_disabled = false;
        DWORD disabledHot_cornerRadius = 8;
        DWORD disabledHot_color = 0x00000000;

        bool hot_disabled = false;
        DWORD hot_cornerRadius = 8;
        DWORD hot_color = 0x41808080;
    };

    struct IconBackgroundColorRemoval
    {
        bool enable = false;
        DWORD colorTreatAsTransparent = 0;
        int colorTreatAsTransparentThreshold = 10;
    };

    struct BorderContext
    {
        bool colorUseNone = false;
        bool colorUseDefault = false;
        /// Реестр AARRGGBB (как BWM); GDI/DWM — через ARGBToCOLORREF в месте применения.
        DWORD color = 0;
        DWM_WINDOW_CORNER_PREFERENCE cornerType = DWMWCP_DONOTROUND;
    };

    // Структура данных для Immersive меню
    struct ContextMenuRenderingData
    {
        wchar_t* text;
        size_t length;
        size_t reserved;
        UINT menuFlags;
        UINT unknown1;
        HBITMAP hbmpItem;
        HBITMAP hbmpChecked;
        HBITMAP hbmpUnchecked;
        UINT cmpt;
        UINT scaleType;
        UINT dpi;
        bool useDarkTheme;
        bool useSystemPadding;
    };

    struct MenuContext
    {
        MenuType type{ MenuType::Unknown };
        bool useDarkMode{ false };
        bool useUxTheme{ false };
        bool noSystemDropShadow{ false };
        bool immersiveStyle{ false };
        MenuCustomRendering customRendering;
        IconBackgroundColorRemoval iconBackgroundColorRemoval;
        BorderContext border;
    };

    extern thread_local MenuContext g_MenuContext;
    inline thread_local const DRAWITEMSTRUCT* g_drawItemStruct{ nullptr };
    inline thread_local bool g_uxhooksEnabled{ false };

    void ResetContext();
}