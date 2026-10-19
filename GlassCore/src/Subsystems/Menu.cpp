#include "CorePch.h"
#include <Subsystems/Menu.h>
#include <Subsystems/MenuEvents.h>

#include <Subsystems/MenuHandler.h>
#include <Hooks/UxThemeHooks.h>
#include <Hooks/MenuHooks.h>

namespace GlassCore::Menu
{
    void Prepare()
    {
        // Подготовка состояния меню (контекст, флаги)
        MenuHandler::Prepare();

        // Подготовка UxTheme-хуков (Detours/IAT, без PDB-оффсетов).
        Hooks::UxThemeHooks::Prepare();
        Hooks::MenuHooks::Prepare();

        DebugLog(L"[GlassCore][Menu] Prepare");
    }

    void Startup()
    {
        // Инициализация состояния меню
        MenuHandler::Startup();

        // Инициализация хуков (без включения)
        Hooks::UxThemeHooks::Startup();
        Hooks::MenuHooks::Startup();

        DebugLog(L"[GlassCore][Menu] Startup");
    }

    void Shutdown()
    {
        // Удержания NC (HTMENU / MSGF_MENU) — снять до UxThemeHooks::Shutdown.
        ShutdownClassicMenuBarNcExtTextOut();

        // Сначала снимаем хуки
        Hooks::UxThemeHooks::Shutdown();
        Hooks::MenuHooks::Shutdown();

        // Затем чистим состояние меню
        MenuHandler::Shutdown();

        DebugLog(L"[GlassCore][Menu] Shutdown");
    }

    void Update()
    {
        Hooks::MenuHooks::SyncIconHooksFromRegistry();
    }

    void OnPopupCreated(HWND hWnd)
    {
        // Вызывается из WndCompat при создании popup‑меню
        // На этом этапе окно ещё может не иметь финальных размеров
        UNREFERENCED_PARAMETER(hWnd);

        DebugLog(L"[GlassCore][Menu] OnPopupCreated: 0x%p", hWnd);
    }

    void OnPopupDestroyed(HWND hWnd)
    {
        UNREFERENCED_PARAMETER(hWnd);

        DebugLog(L"[GlassCore][Menu] OnPopupDestroyed: 0x%p", hWnd);
    }
}
