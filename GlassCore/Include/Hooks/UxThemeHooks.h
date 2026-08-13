// GlassCore/Include/Hooks/UxThemeHooks.h
#pragma once

namespace GlassCore::Hooks::UxThemeHooks
{
    // Чтение оффсетов из реестра, подготовка внутренних структур
    void Prepare();

    // Инициализация хуков (установка Detours для ExtTextOutW)
    void Startup();

    // Полное снятие хуков и очистка состояния
    void Shutdown();

    // Включение/выключение общих хуков (StretchBlt, DwmSetWindowAttribute)
    void EnableHooks(bool enable);

    // Включение/выключение UxTheme хуков (DrawThemeBackground/Text) - для NativeTheme
    void EnableThemeHooks(bool enable);

    // WM_UAHDRAWMENU / WM_UAHDRAWMENUITEM на root: как BWM — флаг MenuHandler,
    // EnableThemeHooks и (для Classic) ExtTextOutW вокруг DefSubclassProc.
    void BeginMenuBarUxThemeDraw(HWND menuOwnerHwnd) noexcept;
    void EndMenuBarUxThemeDraw(HWND menuOwnerHwnd) noexcept;

    // Включение/выключение ExtTextOutW хука - для Classic меню
    // (На самом деле ExtTextOutW всегда установлен, управляется через IsUxHooksEnabled)
    void EnableExtTextOutHook(bool enable);
    void SetCurrentMenuHwnd(HWND hWnd) noexcept;
    HWND GetCurrentMenuHwnd() noexcept;
    // E8 patches для внутренних методов uxtheme
    void ResetE8PatchState();
    void RevertAllE8Patches();
    void PrepareE8Patches();
    bool ApplyAllE8Patches();
}