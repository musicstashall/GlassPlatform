// GlassCore/src/Subsystems/MenuHandler.cpp
#include "CorePch.h"
#include <Subsystems/MenuHandler.h>

namespace GlassCore::Menu
{
    WndCompat::Menu::MenuContext MenuHandler::s_context{};
    bool MenuHandler::s_uxhooksEnabled = false;

    void MenuHandler::Prepare()
    {
        DebugLog(L"[GlassCore][MenuHandler] Prepare");
    }

    void MenuHandler::Startup()
    {
        DebugLog(L"[GlassCore][MenuHandler] Startup");
    }

    void MenuHandler::Shutdown()
    {
        DebugLog(L"[GlassCore][MenuHandler] Shutdown");
    }

    void MenuHandler::SetContext(const WndCompat::Menu::MenuContext& ctx)
    {
        s_context = ctx;
    }

    const WndCompat::Menu::MenuContext& MenuHandler::GetContext()
    {
        return s_context;
    }

    // -------------------------------------------------------------------------
    void MenuHandler::SetBorderContext(const Settings::BorderContext& border)
    {
        s_context.border.cornerType = border.cornerType;
        s_context.border.colorUseNone = border.colorUseNone;
        s_context.border.colorUseDefault = border.colorUseDefault;
        // color — ARGB 0xAARRGGBB (как BWM g_menuContext.border.color).
        s_context.border.color = border.color;
    }

    void MenuHandler::SetDarkMode(bool dark)
    {
        s_context.useDarkMode = dark;
    }

    void MenuHandler::SetUseUxTheme(bool value)
    {
        s_context.useUxTheme = value;
    }

    void MenuHandler::SetUxHooksEnabled(bool enabled)
    {
        s_uxhooksEnabled = enabled;
    }

    bool MenuHandler::IsUxHooksEnabled()
    {
        return s_uxhooksEnabled;
    }
}