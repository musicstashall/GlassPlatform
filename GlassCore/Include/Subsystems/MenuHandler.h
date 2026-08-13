// GlassCore/Include/Subsystems/MenuHandler.h
//
// FIX (Bug 2): добавлен SetBorderContext — позволяет OnPopupSized
// синхронизировать Settings::BorderContext (с DWMWCP_ROUND) в MenuHandler,
// чтобы ImmersiveHooks::MyDwmSetWindowAttribute видел правильный cornerType
// когда перехватывает вызов shell32.

#pragma once
#include <MenuEventProvider.h>
#include <Subsystems/Settings.h>

namespace GlassCore::Menu
{
    class MenuHandler
    {
    public:
        static void Prepare();
        static void Startup();
        static void Shutdown();

        static void SetContext(const WndCompat::Menu::MenuContext& ctx);
        static const WndCompat::Menu::MenuContext& GetContext();

        // FIX: обновляет только поле border внутри s_context,
        // используя значения из Settings::BorderContext, прочитанные
        // в OnPopupSized. Нужно для корректной работы перехватчика
        // ImmersiveHooks::MyDwmSetWindowAttribute.
        static void SetBorderContext(const Settings::BorderContext& border);

        static void SetDarkMode(bool dark);
        static void SetUseUxTheme(bool value);

        static void SetUxHooksEnabled(bool enabled);
        static bool IsUxHooksEnabled();

    private:
        static WndCompat::Menu::MenuContext s_context;
        static bool s_uxhooksEnabled;
    };
}