#pragma once
#include <windows.h>
#include <cpprt.h>

namespace GlassCore::Animation
{
    // Константы длительности анимаций (из BWM/FlyoutAnimation.hpp)
    inline constexpr std::chrono::milliseconds StandardFadeoutDuration{ 350 };
    inline constexpr std::chrono::milliseconds StandardPopupInDuration{ 250 };
    inline constexpr std::chrono::milliseconds StandardFadeInDuration{ 87 };
    inline constexpr float StandardStartPosRatio{ 0.5f };

    // Создание анимации исчезновения меню
    HRESULT CreateMenuFadeOut(
        HWND hWnd,
        const MENUBARINFO& mbi,
        std::chrono::milliseconds duration = StandardFadeoutDuration
    );

    // Создание анимации появления меню (для обычных меню)
    HRESULT CreateMenuPopupIn(
        HWND hWnd,
        float startPosRatio = StandardStartPosRatio,
        std::chrono::milliseconds popInDuration = StandardPopupInDuration,
        std::chrono::milliseconds fadeInDuration = StandardFadeInDuration,
        DWORD animationStyle = 0
    );

    // Создание анимации появления выпадающего списка (вызывающий задаёт immediateInterupting из QueryFlyoutAnimationContext)
    HRESULT CreateDropDownPopupIn(
        HWND hWnd,
        float startPosRatio = StandardStartPosRatio,
        std::chrono::milliseconds popInDuration = StandardPopupInDuration,
        std::chrono::milliseconds fadeInDuration = StandardFadeInDuration,
        DWORD animationStyle = 0,
        bool immediateInterupting = false
    );

    // Остановка AnimationWorker: сбрасывает очередь анимаций и ждёт выхода потока.
    // Вызывается из Engine::Shutdown() до FreeLibrary.
    void Shutdown();

    // Host UI завершает процесс — короткое ожидание потока анимации.
    void ShutdownHostExit() noexcept;
}