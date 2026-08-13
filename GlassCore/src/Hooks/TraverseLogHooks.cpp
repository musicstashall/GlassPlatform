// GlassCore/src/Hooks/TraverseLogHooks.cpp
//
// Хуки для TraverseLog-меню (Explorer TraverseLog owner-draw,
// e.g. панель навигации проводника).
// Перехватываем DrawTextW и DrawThemeBackground в IAT explorerframe.dll.
//
// Отличие от BWM: guard — только g_drawItemStruct != nullptr.
// Убрана избыточная проверка g_uxhooksEnabled, которой нет в BWM и
// которая ломала перехват, когда uxhooks не активны (TraverseLog-меню).
//
#include "CorePch.h"
#include <Hooks/TraverseLogHooks.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Subsystems/MenuEvents.h>      // IsInsideOwnerDraw() — GlassCore-side guard
#include <Subsystems/MenuRendering.h>
#include <Subsystems/RenderText.h>
#include <MenuContext.h>
#include <wil.h>

using namespace std::literals;

namespace GlassCore::Hooks::TraverseLog
{
    // =====================================================================
    // Прототипы
    // =====================================================================

    int WINAPI MyDrawTextW(
        HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format);

    HRESULT WINAPI MyDrawThemeBackground(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, LPCRECT pClipRect);

    // =====================================================================
    // Таблица зависимостей
    // =====================================================================

    inline const HookDispatcherDependency<2, 3> g_hookDependency
    { {
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
        }
    } };

    // Единственный диспетчер — explorerframe.dll
    inline HookDispatcher<2, 3> g_hookDispatcher{ g_hookDependency, nullptr };

    static wil::srwlock g_lock;
    static size_t g_hookRef = 0;

    // =====================================================================
    // Хук-функции
    // =====================================================================

    int WINAPI MyDrawTextW(
        HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format)
    {
        auto orig = g_hookDispatcher.GetOrg<0, decltype(&MyDrawTextW)>();

        // Guard: проверяем GlassCore-internal thread_local, установленный
        // WndCompat через OnOwnerDrawBegin/End.
        // WndCompat::Menu::g_drawItemStruct нельзя использовать здесь —
        // thread_local не разделяется между DLL, GlassCore видит только свою копию.
        if (!GlassCore::Menu::IsInsideOwnerDraw())
            return orig(hdc, lpchText, cchText, lprc, format);

        if ((format & DT_CALCRECT) || (format & DT_INTERNAL) || (format & DT_NOCLIP))
            return orig(hdc, lpchText, cchText, lprc, format);

        int result = 0;
        if (FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
            result = orig(hdc, lpchText, cchText, lprc, format);

        return result;
    }

    HRESULT WINAPI MyDrawThemeBackground(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, LPCRECT pClipRect)
    {
        auto orig = g_hookDispatcher.GetOrg<1, decltype(&MyDrawThemeBackground)>();

        // Разделители (MENU_POPUPSEPARATOR) рисуются ВНЕ WM_DRAWITEM — обрабатываем их всегда
        const bool isSeparator = (iPartId == MENU_POPUPSEPARATOR);

        // Guard: GlassCore-internal thread_local (см. MyDrawTextW)
        if (!isSeparator && !GlassCore::Menu::IsInsideOwnerDraw())
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        if (GlassCore::Menu::Rendering::HandleDrawThemeBackground(
            hTheme, hdc, iPartId, iStateId, pRect, pClipRect, orig))
        {
            return S_OK;
        }

        return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
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

    void EnableHooks(bool enable)
    {
        auto lock = g_lock.lock_exclusive();

        // Ref-counting: меняем состояние только на границе 0↔1
        bool changed = false;
        if (enable && g_hookRef == 0) changed = true;
        if (!enable && g_hookRef == 1) changed = true;

        if (enable)
            ++g_hookRef;
        else if (g_hookRef > 0)
            --g_hookRef;

        if (!changed)
            return;

        HMODULE targetModule = GetModuleHandleW(L"explorerframe.dll");
        if (!targetModule)
            return;

        g_hookDispatcher.moduleAddress = targetModule;

        if (enable)
        {
            g_hookDispatcher.EnableHook(0, true);   // DrawTextW
            g_hookDispatcher.EnableHook(1, true);   // DrawThemeBackground
        }
        else
        {
            g_hookDispatcher.DisableAllHooks();
            g_hookDispatcher.moduleAddress = nullptr;
        }
    }

    void DisableHooks()
    {
        EnableHooks(false);
    }
}