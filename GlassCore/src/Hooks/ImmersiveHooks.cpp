// GlassCore/src/Hooks/ImmersiveHooks.cpp
//
// Хуки для Immersive-меню (owner-draw контекстные меню Explorer/Shell).
// Перехватываем DrawTextW, DrawThemeBackground, DwmSetWindowAttribute
// в IAT целевого модуля (shell32.dll / explorer.exe).
// DrawTextExW / DrawThemeTextEx (ink, shell32) — в UxThemeHooks (IAT/Detour), не в этом файле.
//
// Активируются только пока GlassCore::Menu::IsInsideOwnerDraw() == true,
// т.е. строго внутри WM_DRAWITEM → DefSubclassProc → owner-draw код.
// Guard устанавливается через OnOwnerDrawBegin/End-колбеки MenuEventProvider,
// что позволяет избежать проблемы DLL-границы с thread_local g_drawItemStruct.
//
#include "CorePch.h"

#include <Hooks/ImmersiveHooks.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Subsystems/MenuHandler.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/MenuRendering.h>
#include <SystemHelper.h>
#include <MenuContext.h>
#include <MenuUtils.h>
#include <ColorUtils.h>
#include <wil.h>
#include <Core/Intercept/HookHelper.h>
#include <WinApiUtils.h>
#include <DebugLog.h>

using namespace std::literals;

namespace GlassCore::Menu
{
    // Forward-declaration guard'а, определённого в MenuEvents.cpp (тот же DLL)
    bool IsInsideOwnerDraw() noexcept;
}

namespace GlassCore::Hooks::Immersive
{
    // =====================================================================
    // Прототипы хук-функций
    // =====================================================================

    int WINAPI MyDrawTextW(
        HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format);

    HRESULT WINAPI MyDrawThemeBackground(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, LPCRECT pClipRect);

    HRESULT WINAPI MyDwmSetWindowAttribute(
        HWND hwnd, DWORD dwAttribute, LPCVOID pvAttribute, DWORD cbAttribute);

    // =====================================================================
    // Таблица зависимостей хуков — одна, разделяется между диспетчерами.
    // =====================================================================

    enum HookIndex : size_t
    {
        HK_DRAWTEXT = 0,
        HK_DRAWTHEMEBACKGROUND,
        HK_SETWINATTRIBUTE,
    };

    inline const HookDispatcherDependency<3, 3> g_hookDependency
    { {
            // 0: DrawTextW — user32 или форварды
            std::tuple
            {
                std::array<std::string_view, 3>
                {
                    "user32.dll"sv,
                    "ext-ms-win-ntuser-draw-l1-1-0.dll"sv,
                    "ext-ms-win-ntuser-misc-l1-1-0.dll"sv
                },
                "DrawTextW",
                reinterpret_cast<PVOID>(MyDrawTextW)
            },
        // 1: DrawThemeBackground — uxtheme или форвард
        std::tuple
        {
            std::array<std::string_view, 3>
            {
                "uxtheme.dll"sv,
                "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv,
                ""sv
            },
            "DrawThemeBackground",
            reinterpret_cast<PVOID>(MyDrawThemeBackground)
        },
        // 2: DwmSetWindowAttribute — для переопределения скруглений и цвета рамки
        std::tuple
        {
            std::array<std::string_view, 3>
            {
                "dwmapi.dll"sv,
                ""sv,
                ""sv
            },
            "DwmSetWindowAttribute",
            reinterpret_cast<PVOID>(MyDwmSetWindowAttribute)
        }
    } };

    // =====================================================================
    // Карта диспетчеров: один диспетчер на модуль (shell32, explorer, …)
    // =====================================================================

    static wil::srwlock g_lock;
    using DispatcherType = HookDispatcher<3, 3>;
    static std::unordered_map<PVOID, DispatcherType> g_hookDispatcherMap;

    // =====================================================================
    // Вспомогательная функция: включить/выключить хуки в конкретном модуле
    // =====================================================================

    static bool EnableHooksInternal(PVOID baseAddress, bool enable)
    {
        if (!baseAddress)
            return false;

        auto it = g_hookDispatcherMap.find(baseAddress);
        if (it == g_hookDispatcherMap.end())
        {
            // Создаём диспетчер для этого модуля
            auto [inserted, ok] = g_hookDispatcherMap.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(baseAddress),
                std::forward_as_tuple(g_hookDependency, baseAddress));
            (void)ok;
            it = inserted;
        }

        auto& dispatcher = it->second;
        dispatcher.moduleAddress = baseAddress;

        dispatcher.EnableHook(HK_DRAWTEXT, enable); // рисует только стрелку подменю.
        dispatcher.EnableHook(HK_DRAWTHEMEBACKGROUND, enable);

        // DwmSetWindowAttribute — только на Windows 11+
        if (Shared::SystemHelper::g_buildNumber >= 22000)
            dispatcher.EnableHook(HK_SETWINATTRIBUTE, enable);

        return true;
    }

    // =====================================================================
    // Реализация хук-функций
    // =====================================================================

    // DrawTextW: рисуем текст через buffered paint с правильной альфой,
    // чтобы текст был виден поверх DWM blur (альфа-канал сохраняется).
    int WINAPI MyDrawTextW(
        HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format)
    {
        auto callerModule = DetourGetContainingModule(_ReturnAddress());
        auto it = g_hookDispatcherMap.find(callerModule);
        if (it == g_hookDispatcherMap.end())
            return DrawTextW(hdc, lpchText, cchText, lprc, format);

        auto& dispatcher = it->second;
        auto orig = dispatcher.GetOrg<HK_DRAWTEXT, decltype(&DrawTextW)>();
        
        // Перехватываем только внутри WM_DRAWITEM (guard в GlassCore, не пересекает DLL-границу)
        if (!GlassCore::Menu::IsInsideOwnerDraw())
            return orig(hdc, lpchText, cchText, lprc, format);

        // Флаги-исключения: расчётные и специальные вызовы не перехватываем
        if ((format & DT_CALCRECT) || (format & DT_INTERNAL) || (format & DT_NOCLIP))
            return orig(hdc, lpchText, cchText, lprc, format);
        
        int result = 0;
        if (FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
            result = orig(hdc, lpchText, cchText, lprc, format);

        return result;
    }

    // DrawThemeBackground: чернение фона + custom D2D рендеринг пунктов.
    HRESULT WINAPI MyDrawThemeBackground(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, LPCRECT pClipRect)
    {
        auto callerModule = DetourGetContainingModule(_ReturnAddress());
        auto it = g_hookDispatcherMap.find(callerModule);
        if (it == g_hookDispatcherMap.end())
            return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        auto& dispatcher = it->second;
        auto orig = dispatcher.GetOrg<HK_DRAWTHEMEBACKGROUND, decltype(&DrawThemeBackground)>();

        // Разделители (MENU_POPUPSEPARATOR) рисуются ВНЕ WM_DRAWITEM — обрабатываем их всегда
        const bool isSeparator = (iPartId == MENU_POPUPSEPARATOR);

        // Перехватываем внутри WM_DRAWITEM или для разделителей
        if (!isSeparator && !GlassCore::Menu::IsInsideOwnerDraw())
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        if (GlassCore::Menu::Rendering::HandleDrawThemeBackground(
            hTheme, hdc, iPartId, iStateId, pRect, pClipRect, orig))
        {
            return S_OK;
        }

        return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
    }

    // DwmSetWindowAttribute: переопределяем скругления углов и цвет рамки
    // согласно настройкам GlassProject (border context).
    HRESULT WINAPI MyDwmSetWindowAttribute(
        HWND hwnd, DWORD dwAttribute, LPCVOID pvAttribute, DWORD cbAttribute)
    {
        auto callerModule = DetourGetContainingModule(_ReturnAddress());
        auto it = g_hookDispatcherMap.find(callerModule);
        if (it == g_hookDispatcherMap.end())
            return DwmSetWindowAttribute(hwnd, dwAttribute, pvAttribute, cbAttribute);

        auto& dispatcher = it->second;
        auto orig = dispatcher.GetOrg<HK_SETWINATTRIBUTE, decltype(&DwmSetWindowAttribute)>();

        if (Shared::MenuUtils::IsPopupMenu(hwnd))
        {
            const auto& border = GlassCore::Menu::MenuHandler::GetContext().border;

            if (dwAttribute == DWMWA_WINDOW_CORNER_PREFERENCE)
            {
                if (border.cornerType != DWMWCP_DEFAULT)
                {
                    pvAttribute = &border.cornerType;
                    cbAttribute = sizeof(border.cornerType);
                }
            }
            else if (dwAttribute == DWMWA_BORDER_COLOR)
            {
                if (border.colorUseNone)
                {
                    static const COLORREF noColor = DWMWA_COLOR_NONE;
                    pvAttribute = &noColor;
                    cbAttribute = sizeof(noColor);
                }
                else if (!border.colorUseDefault)
                {
                    static COLORREF customColor;
                    customColor = Shared::ColorUtils::ARGBToCOLORREF(border.color);
                    pvAttribute = &customColor;
                    cbAttribute = sizeof(customColor);
                }
            }
        }

        return orig(hwnd, dwAttribute, pvAttribute, cbAttribute);
    }

    // =====================================================================
    // Публичный API
    // =====================================================================

    void Prepare() {}

    void Startup() {}

    void Shutdown()
    {
        DisableHooks();
    }

    // EnableHooks: зеркальная копия BWM-логики.
    //
    // Для Explorer-контекстных меню (explorerframe.dll как инициатор):
    //   - хукаем shell32.dll + explorer.exe (тут реализован owner-draw код)
    // Для Windows 11 Taskbar (twinui.pcshell.dll):
    //   - дополнительно хукаем Taskbar.dll
    // Для любого другого модуля — хукаем напрямую.
    void EnableHooks(PVOID baseAddress, bool enable)
    {
        auto lock = g_lock.lock_exclusive();

        HMODULE explorerFrame = GetModuleHandleW(L"explorerframe.dll");
        HMODULE twinui = GetModuleHandleW(L"twinui.pcshell.dll");
        HMODULE explorerExe = GetModuleHandleW(L"explorer.exe");

        if (explorerFrame && explorerFrame == baseAddress)
        {
            if (explorerExe)
            {
                EnableHooksInternal(GetModuleHandleW(L"shell32.dll"), enable);
                EnableHooksInternal(explorerExe, enable);
                return;
            }
            baseAddress = GetModuleHandleW(L"shell32.dll");
        }
        else if (twinui && twinui == baseAddress)
        {
            if (explorerExe && Shared::SystemHelper::g_buildNumber >= 22000)
            {
                HMODULE taskbar = GetModuleHandleW(L"Taskbar.dll");
                if (taskbar)
                    EnableHooksInternal(taskbar, enable);
            }
        }

        EnableHooksInternal(baseAddress, enable);
    }

    void DisableHooks()
    {
        auto lock = g_lock.lock_exclusive();

        for (auto& [addr, dispatcher] : g_hookDispatcherMap)
        {
            dispatcher.DisableAllHooks();
            dispatcher.moduleAddress = nullptr;
        }
    }
}