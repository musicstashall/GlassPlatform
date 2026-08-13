#pragma once

#include <windows.h>



namespace GlassCore::TreeViewControl

{

    void Prepare();

    void Startup();

    void Shutdown();



    // Глубина сессии themed-текста меню (MyDrawThemeText → DrawThemeText orig).

    [[nodiscard]] int MenuThemedTextDrawSessionDepth() noexcept;

    void MenuThemedTextDrawSessionPush() noexcept;

    void MenuThemedTextDrawSessionPop() noexcept;

}

