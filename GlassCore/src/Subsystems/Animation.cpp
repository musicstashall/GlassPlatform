// GlassCore/src/Subsystems/Animation.cpp
//
// PORTING LOG (from BlackWindowManager/FlyoutAnimation):
//
// Fix 1: Added animation styles 1-6 in PopupIn::Start().
//        Only style 0 was implemented; styles 1-6 were TODO stubs.
//
// Fix 2: PopupIn::Start() catch block now properly cleans up all
//        resources on failure: sends AnimationFinished message,
//        posts WM_CLOSE to helper windows, unregisters thumbnails.
//        Previously it only called Finish() + logged, leaving the menu
//        window permanently cloaked and leaking DWM thumbnails.
//
// Fix 3: SubclassProc shadow detection changed from IsPopupMenu(shadow)
//        to an explicit "SysShadow" window-class check, matching BWM.
//        IsPopupMenu() can match unrelated popup windows.
//
// Fix 4: MenuPopupIn::SetBackdropEffectForAnimation() now:
//        - Skips rounding below build 22000 (Win10), same as BWM.
//        - Reads the actual theme border colour from
//          ImmersiveStart::Menu when border.colorUseDefault is true.
//        - Тёмный режим для оверлея анимации — из MenuHandler (как DTB для меню),
//          а не ThemeHelper::IsSystemDarkMode() (иначе рассинхрон с OnPopupSized).
//
// Fix 5: DropDownPopupIn::SetBackdropEffectForAnimation() now guards
//        corner-radius and border-effect code behind build >= 22000,
//        matching BWM behaviour on Windows 10.
//
// Fix 6: Menu #32768 — после ctor PopupIn меню остаётся DWM-cloaked до Start().
//        Start() вызывался только из WM_WINDOWPOSCHANGED + SWP_SHOWWINDOW.
//        Если SHOW уже обработан до сабкласса анимации или приходит только
//        SWP_FRAMECHANGED (MN_SIZEWINDOW со стрелками), Start() не вызывался —
// Fix 7: устарело (см. Fix 8).
//
// Fix 8: Паритет MenuPopupIn / BWM MenuPopupIn:
//   Menu и DropDown — на m_backdropWindow через ApplyEffect (backdrop + border), как MenuEvents::OnPopupSized.

#include "CorePch.h"
#include <Subsystems/Animation.h>
#include <Subsystems/Settings.h>
#include <Subsystems/FlyoutFrame.h>
#include <GlassMessages.h>
#include <Core/Utils.h>
#include <Subsystems/RenderText.h>
#include <MenuUtils.h>
#include <Helpers/ThemeHelper.h>
#include <DXHelper.h>
#include <Subsystems/EffectHelper.h>
#include <Subsystems/MenuHandler.h>
#include <Subsystems/MenuEvents.h>
#include <Hooks/DropDown.h>
#include <DwmThumbnailAPI.h>
#include <DropDownContext.h>

// Windows API
#include <dwmapi.h>
#include <dcomp.h>
#include <UIAnimation.h>
#include <d2d1.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <algorithm>


#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "uxtheme.lib")

using namespace std::chrono_literals;

namespace GlassCore::Animation
{
    // ---------- Вспомогательные утилиты ----------

    static DWORD GetBuildNumber()
    {
        static DWORD build = []() -> DWORD
            {
                HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
                if (!ntdll) return 0;
                using Fn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
                auto fn = reinterpret_cast<Fn>(GetProcAddress(ntdll, "RtlGetVersion"));
                if (!fn) return 0;
                RTL_OSVERSIONINFOW v{};
                v.dwOSVersionInfoSize = sizeof(v);
                fn(&v);
                return v.dwBuildNumber;
            }();
        return build;
    }

    static bool IsWindowClass(HWND hWnd, const wchar_t* className)
    {
        if (!hWnd) return false;
        wchar_t buf[64]{};
        GetClassNameW(hWnd, buf, static_cast<int>(std::size(buf)));
        return _wcsicmp(buf, className) == 0;
    }

    // ---------- Вспомогательная функция для получения сообщения об окончании анимации ----------
    static UINT GetAnimationFinishedMsg()
    {
        static UINT msg = RegisterWindowMessageW(L"GlassCore.Animation.Finished");
        return msg;
    }

    // Отложенный вызов PopupIn::Start (см. Fix 6 в заголовке файла).
    [[nodiscard]] static UINT GetDeferredPopupInStartMsg() noexcept
    {
        static UINT msg = RegisterWindowMessageW(L"GlassCore.Animation.PopupIn.TryStart");
        return msg;
    }

    // ---------- Базовый класс анимации ----------
    struct AnimationInfo
    {
        AnimationInfo() = default;
        AnimationInfo(std::chrono::milliseconds animDuration)
            : duration(animDuration)
            , startTimeStamp(GetTickCount64())
            , endTimeStamp(startTimeStamp + duration.count())
        {
        }

        virtual ~AnimationInfo() noexcept = default;

        void SetDuration(std::chrono::milliseconds animDuration)
        {
            duration = animDuration;
            endTimeStamp = startTimeStamp + duration.count();
        }

        void Restart(std::chrono::milliseconds animDuration)
        {
            startTimeStamp = GetTickCount64();
            duration = animDuration;
            endTimeStamp = startTimeStamp + duration.count();
        }

        virtual void Animator(ULONGLONG /*currentTimeStamp*/) {}

        HWND window = nullptr;
        std::chrono::milliseconds duration{ 0 };
        ULONGLONG startTimeStamp = GetTickCount64();
        ULONGLONG endTimeStamp = startTimeStamp + duration.count();
    };

    // ---------- Поток-обработчик анимаций (синглтон) ----------
    class AnimationWorker
    {
    public:
        friend class wil::shutdown_aware_object<AnimationWorker>;
        static AnimationWorker& GetInstance()
        {
            static wil::shutdown_aware_object<AnimationWorker> instance;
            return instance.get();
        }

        AnimationWorker(const AnimationWorker&) = delete;
        AnimationWorker& operator=(const AnimationWorker&) = delete;

        void ProcessShutdown() {}
        void Schedule(std::shared_ptr<AnimationInfo> info);

        void ShutdownWithTimeout(DWORD maxWaitMs) noexcept
        {
            HANDLE hThread = nullptr;
            {
                auto lock = m_lock.lock_exclusive();
                m_shutdownFlag.store(true, std::memory_order_release);
                hThread = m_threadHandle;
            }

            if (hThread)
            {
                DWORD waitResult = WaitForSingleObject(hThread, maxWaitMs);
                DebugLog(
                    L"[GlassCore][Animation] Shutdown wait result=%lu (0=OBJECT, 258=TIMEOUT)",
                    waitResult);
                CloseHandle(hThread);
                m_threadHandle = nullptr;
            }
        }

        void Shutdown() noexcept
        {
            ShutdownWithTimeout(10000);
        }

        AnimationWorker() = default;
        ~AnimationWorker() noexcept = default;

        // Event-based shutdown: GUI-thread signals when all subclasses are removed
        HANDLE m_hAnimDetachDone = nullptr;
        std::atomic<int> g_pendingAnimDetaches{0};

    private:
        static DWORD WINAPI ThreadProc(LPVOID param);

        wil::srwlock m_lock;
        DWORD m_threadId = 0;
        HANDLE m_threadHandle = nullptr;
        std::atomic<bool> m_shutdownFlag{false};
        std::vector<std::shared_ptr<AnimationInfo>> m_animationStorage;
    };

    // ---------- Анимация исчезновения (FadeOut) ----------
    class FadeOut : public AnimationInfo
    {
    public:
        FadeOut(HWND hWnd, const MENUBARINFO& mbi, std::chrono::milliseconds animDuration)
            : AnimationInfo(animDuration)
        {
            POINT source = { 0, 0 };
            SIZE size = { mbi.rcBar.right - mbi.rcBar.left, mbi.rcBar.bottom - mbi.rcBar.top };
            POINT destination = { mbi.rcBar.left, mbi.rcBar.top };
            RECT paintRect = { destination.x, destination.y, destination.x + size.cx, destination.y + size.cy };

            RECT windowRect{};
            THROW_LAST_ERROR_IF(!GetWindowRect(hWnd, &windowRect));

            window = Utils::SHCreateWorkerWindowW(
                DefWindowProcW,
                Shared::MenuUtils::GetCurrentMenuOwner(),
                WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                WS_POPUP,
                nullptr,
                0
            );
            THROW_LAST_ERROR_IF_NULL(window);

            auto menuDC = wil::GetWindowDC(hWnd);
            THROW_LAST_ERROR_IF_NULL(menuDC);
            auto screenDC = wil::GetDC(nullptr);
            THROW_LAST_ERROR_IF_NULL(screenDC);

            auto drawFunc = [&](HDC memoryDC, HPAINTBUFFER, RGBQUAD*, int)
                {
                    THROW_LAST_ERROR_IF(!BitBlt(memoryDC,
                        paintRect.left, paintRect.top,
                        size.cx, size.cy,
                        menuDC.get(),
                        destination.x - windowRect.left,
                        destination.y - windowRect.top,
                        SRCCOPY));

                    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
                    THROW_LAST_ERROR_IF(!UpdateLayeredWindow(window,
                        screenDC.get(),
                        &destination,
                        &size,
                        memoryDC,
                        &source,
                        0,
                        &blend,
                        ULW_ALPHA));

                    ShowWindow(window, SW_SHOWNA);
                };

            THROW_IF_FAILED(Text::DrawThemeContent(screenDC.get(), paintRect, nullptr, nullptr, BPPF_NOCLIP, drawFunc));
            THROW_IF_FAILED(DwmFlush());
        }

        ~FadeOut() noexcept
        {
            if (window)
            {
                SendNotifyMessageW(window, WM_CLOSE, 0, 0);
                window = nullptr;
            }
        }

        void Animator(ULONGLONG currentTimeStamp) override
        {
            BYTE alpha = static_cast<BYTE>(
                255 - static_cast<int>(
                    std::round((static_cast<float>(currentTimeStamp - startTimeStamp) / static_cast<float>(duration.count())) * 255.f)
                    )
                );
            BLENDFUNCTION blend = { AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
            UpdateLayeredWindow(window, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &blend, ULW_ALPHA);
        }
    };

    // ---------- Базовый класс анимации появления (PopupIn) ----------
    class PopupIn : public AnimationInfo
    {
    protected:
        bool m_reverse = false;
        bool m_started = false;
        bool m_immediateInterupting = false;
        bool m_useSysDropShadow = false;
        float m_cornerRadius = 0.f;
        float m_ratio = 0.f;
        DWORD m_animationStyle = 0;
        POINT m_cursorPos{};

        HWND m_backdropWindow = nullptr;
        HWND m_menuWindow = nullptr;
        /// Целевой flyout — не обнуляется в Detach(); Finish/uncloak/refresh используют его.
        HWND m_flyoutHwnd = nullptr;

        std::chrono::milliseconds m_fadeInDuration{ 0ms };
        std::chrono::milliseconds m_popupInDuration{ 0ms };
        std::chrono::milliseconds m_totDuration{ 0ms };

        wil::com_ptr<IDCompositionVisual2> m_rootVisual;
        wil::com_ptr<IDCompositionTarget> m_dcompTarget;
        HTHUMBNAIL m_thumbnail = nullptr;
        wil::com_ptr<IDCompositionVisual2> m_thumbnailVisual;
        HTHUMBNAIL m_backdropThumbnail = nullptr;
        wil::com_ptr<IDCompositionVisual2> m_backdropThumbnailVisual;

        static bool EnsureDwmApi()
        {
            static bool inited = []() {
                return SUCCEEDED(Shared::DwmThumbnailAPI::Initialize());
                }();
            return inited;
        }

        virtual void SetBackdropEffectForAnimation() = 0;

        /// После uncloak real flyout: лёгкий NC refresh (паритет TF show для ET 6–8 / явный corner).
        virtual void RefreshFlyoutFrameAfterUncloak() noexcept {}

        /// Prime DWM thumbnail: WM_PRINT PRF_CLIENT до первого кадра DComp (defer + client-edge).
        void PrimeDeferredFlyoutThumbnailForAnimation() noexcept
        {
            HWND const flyout = m_flyoutHwnd ? m_flyoutHwnd : m_menuWindow;
            if (!flyout || !IsWindow(flyout))
                return;
            if (!GlassCore::FlyoutFrame::IsHostBorderDeferred(flyout))
                return;

            RECT clientRect{};
            if (!GetClientRect(flyout, &clientRect))
                return;

            const int width = clientRect.right - clientRect.left;
            const int height = clientRect.bottom - clientRect.top;
            if (width <= 0 || height <= 0)
                return;

            HDC hdcScreen = GetDC(nullptr);
            if (!hdcScreen)
                return;
            auto releaseScreen = wil::scope_exit([&] { ReleaseDC(nullptr, hdcScreen); });

            wil::unique_hdc hdcMem{ CreateCompatibleDC(hdcScreen) };
            if (!hdcMem)
                return;

            wil::unique_hbitmap bmp{ CreateCompatibleBitmap(hdcScreen, width, height) };
            if (!bmp)
                return;

            HGDIOBJ const oldBmp = SelectObject(hdcMem.get(), bmp.get());
            auto restoreBmp = wil::scope_exit([&] {
                if (oldBmp)
                    SelectObject(hdcMem.get(), oldBmp);
            });

            constexpr LPARAM printFlags = PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND;
            (void)SendMessageW(flyout, WM_PRINT, reinterpret_cast<WPARAM>(hdcMem.get()), printFlags);
            (void)DwmFlush();

            DebugLog(
                DBG_IMPORTANT,
                L"[GlassCore][Animation] PrimeDeferredFlyoutThumbnail hwnd=0x%p %dx%d",
                flyout,
                width,
                height);
        }

        // ================================================================
        // Start() — инициализация DComposition анимации
        // Fix 2: правильная очистка ресурсов в catch-блоке
        // Fix 3: проверка тени по классу "SysShadow"
        // Fix 1: реализованы стили 1-6
        // ================================================================
        void Start()
        {
            try
            {
                if (m_started)
                    return;

                auto context = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
                auto dpiCleanup = wil::scope_exit([&] { SetThreadDpiAwarenessContext(context); });

                auto& dcomp = Shared::DXHelper::LazyDComposition::GetInstance();
                dcomp.EnsureResources();
                auto dcompDevice = dcomp.GetDCompositionDevice();
                THROW_HR_IF_NULL(E_FAIL, dcompDevice);

                THROW_HR_IF_NULL(E_INVALIDARG, m_dcompTarget);
                THROW_HR_IF_NULL(E_INVALIDARG, m_thumbnailVisual);
                THROW_HR_IF_NULL(E_INVALIDARG, m_backdropThumbnailVisual);

                SetBackdropEffectForAnimation();

                THROW_LAST_ERROR_IF(!GetCursorPos(&m_cursorPos));

                RECT windowRect{};
                THROW_LAST_ERROR_IF(!GetWindowRect(m_menuWindow, &windowRect));

                SIZE size = { windowRect.right - windowRect.left, windowRect.bottom - windowRect.top };
                m_reverse = (abs(m_cursorPos.y - windowRect.top) > abs(m_cursorPos.y - windowRect.bottom));

                if (m_useSysDropShadow)
                    SetClassLongPtr(window, GCL_STYLE, GetClassLongPtr(window, GCL_STYLE) | CS_DROPSHADOW);

                THROW_LAST_ERROR_IF(!SetWindowPos(m_backdropWindow, nullptr,
                    windowRect.left, windowRect.top, size.cx, size.cy,
                    SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING));

                THROW_LAST_ERROR_IF(!SetWindowPos(window, nullptr,
                    windowRect.left, windowRect.top, size.cx, size.cy,
                    SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOSENDCHANGING));

                Utils::CloakWindow(window, FALSE);

                if (m_useSysDropShadow)
                    SetClassLongPtr(window, GCL_STYLE, GetClassLongPtr(window, GCL_STYLE) & ~CS_DROPSHADOW);

                // --- WAM + DComposition анимация ---
                Utils::RoInit comInit;

                wil::com_ptr<IUIAnimationManager2> manager;
                THROW_IF_FAILED(CoCreateInstance(CLSID_UIAnimationManager2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager)));

                wil::com_ptr<IUIAnimationTransitionLibrary2> library;
                THROW_IF_FAILED(CoCreateInstance(CLSID_UIAnimationTransitionLibrary2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&library)));

                DCOMPOSITION_FRAME_STATISTICS stats{};
                THROW_IF_FAILED(dcompDevice->GetFrameStatistics(&stats));

                UI_ANIMATION_SECONDS timeNow =
                    static_cast<double>(stats.nextEstimatedFrameTime.QuadPart) /
                    static_cast<double>(stats.timeFrequency.QuadPart);

                // Вспомогательная лямбда: создаёт WAM-переменную + storyboard,
                // применяет переход и возвращает IDCompositionAnimation для DComp.
                auto MakeAnimation = [&](std::function<wil::com_ptr<IUIAnimationTransition2>(wil::com_ptr<IUIAnimationVariable2>)> callback)
                    -> wil::com_ptr<IDCompositionAnimation>
                    {
                        if (!callback) return nullptr;

                        wil::com_ptr<IUIAnimationVariable2> variable;
                        wil::com_ptr<IUIAnimationStoryboard2> storyboard;
                        THROW_IF_FAILED(manager->CreateAnimationVariable(0.0, &variable));
                        THROW_IF_FAILED(manager->CreateStoryboard(&storyboard));

                        auto transition = callback(variable);
                        THROW_IF_FAILED(storyboard->AddTransition(variable.get(), transition.get()));
                        THROW_IF_FAILED(manager->Update(timeNow));
                        THROW_IF_FAILED(storyboard->Schedule(timeNow));

                        wil::com_ptr<IDCompositionAnimation> dcompAnim;
                        THROW_IF_FAILED(dcompDevice->CreateAnimation(&dcompAnim));
                        THROW_IF_FAILED(variable->GetCurve(dcompAnim.get()));
                        return dcompAnim;
                    };

                const float width = static_cast<float>(size.cx);
                const float height = static_cast<float>(size.cy);
                const double popSec = static_cast<double>(m_popupInDuration.count()) / 1000.0;

                // ================================================================
                // Стиль 0: вертикальный слайд (Y-смещение)
                // Эквивалентен BWM style 0
                // ================================================================
                if (m_animationStyle == 0)
                {
                    constexpr double endValue = 0.0;
                    double beginValue = m_reverse
                        ? static_cast<double>(size.cy) * (1.0 - static_cast<double>(m_ratio))
                        : static_cast<double>(size.cy) * (-1.0 + static_cast<double>(m_ratio));

                    auto anim = MakeAnimation([&](auto) -> wil::com_ptr<IUIAnimationTransition2>
                        {
                            wil::com_ptr<IUIAnimationTransition2> t;
                            THROW_IF_FAILED(library->CreateCubicBezierLinearTransition(popSec, endValue, 0, 0, 0, 1, &t));
                            THROW_IF_FAILED(t->SetInitialValue(beginValue));
                            return t;
                        });

                    THROW_IF_FAILED(m_thumbnailVisual->SetOffsetX(0.f));
                    THROW_IF_FAILED(m_thumbnailVisual->SetOffsetY(anim.get()));
                    THROW_IF_FAILED(m_backdropThumbnailVisual->SetOffsetX(0.f));
                    THROW_IF_FAILED(m_backdropThumbnailVisual->SetOffsetY(anim.get()));
                }

                // ================================================================
                // Стиль 1: круговое раскрытие от курсора (IDCompositionRectangleClip
                //          с анимированными радиусами и краями)
                // Эквивалентен BWM style 1
                // ================================================================
                else if (m_animationStyle == 1)
                {
                    POINT clientPoint = m_cursorPos;
                    THROW_LAST_ERROR_IF(!ScreenToClient(m_menuWindow, &clientPoint));

                    D2D1_POINT_2F startPoint
                    {
                        static_cast<float>(clientPoint.x),
                        static_cast<float>(clientPoint.y)
                    };

                    double maxRadius = 0.0;
                    maxRadius = std::max(maxRadius, static_cast<double>(sqrt(static_cast<double>(startPoint.x) * static_cast<double>(startPoint.x) + static_cast<double>(startPoint.y) * static_cast<double>(startPoint.y))));
                    maxRadius = std::max(maxRadius, static_cast<double>(sqrt(static_cast<double>(width - startPoint.x) * static_cast<double>(width - startPoint.x) + static_cast<double>(startPoint.y) * static_cast<double>(startPoint.y))));
                    maxRadius = std::max(maxRadius, static_cast<double>(sqrt(static_cast<double>(startPoint.x) * static_cast<double>(startPoint.x) + static_cast<double>(height - startPoint.y) * static_cast<double>(height - startPoint.y))));
                    maxRadius = std::max(maxRadius, static_cast<double>(sqrt(static_cast<double>(width - startPoint.x) * static_cast<double>(width - startPoint.x) + static_cast<double>(height - startPoint.y) * static_cast<double>(height - startPoint.y))));

                    wil::com_ptr<IDCompositionRectangleClip> clip;
                    THROW_IF_FAILED(dcompDevice->CreateRectangleClip(&clip));

                    THROW_IF_FAILED(clip->SetTop(
                        MakeAnimation([&](auto) -> wil::com_ptr<IUIAnimationTransition2>
                            {
                                wil::com_ptr<IUIAnimationTransition2> t;
                                THROW_IF_FAILED(library->CreateCubicBezierLinearTransition(
                                    popSec, static_cast<double>(startPoint.y) - maxRadius, 0, 0, 0, 1, &t));
                                THROW_IF_FAILED(t->SetInitialValue(static_cast<double>(startPoint.y) - maxRadius * m_ratio));
                                return t;
                            }).get()));

                    THROW_IF_FAILED(clip->SetBottom(
                        MakeAnimation([&](auto) -> wil::com_ptr<IUIAnimationTransition2>
                            {
                                wil::com_ptr<IUIAnimationTransition2> t;
                                THROW_IF_FAILED(library->CreateCubicBezierLinearTransition(
                                    popSec, static_cast<double>(startPoint.y) + maxRadius, 0, 0, 0, 1, &t));
                                THROW_IF_FAILED(t->SetInitialValue(static_cast<double>(startPoint.y) + maxRadius * m_ratio));
                                return t;
                            }).get()));

                    THROW_IF_FAILED(clip->SetLeft(
                        MakeAnimation([&](auto) -> wil::com_ptr<IUIAnimationTransition2>
                            {
                                wil::com_ptr<IUIAnimationTransition2> t;
                                THROW_IF_FAILED(library->CreateCubicBezierLinearTransition(
                                    popSec, static_cast<double>(startPoint.x) - maxRadius, 0, 0, 0, 1, &t));
                                THROW_IF_FAILED(t->SetInitialValue(static_cast<double>(startPoint.x) - maxRadius * m_ratio));
                                return t;
                            }).get()));

                    THROW_IF_FAILED(clip->SetRight(
                        MakeAnimation([&](auto) -> wil::com_ptr<IUIAnimationTransition2>
                            {
                                wil::com_ptr<IUIAnimationTransition2> t;
                                THROW_IF_FAILED(library->CreateCubicBezierLinearTransition(
                                    popSec, static_cast<double>(startPoint.x) + maxRadius, 0, 0, 0, 1, &t));
                                THROW_IF_FAILED(t->SetInitialValue(static_cast<double>(startPoint.x) + maxRadius * m_ratio));
                                return t;
                            }).get()));

                    // Анимированный радиус скругления (от 0 до maxRadius)
                    auto radiusAnim = MakeAnimation([&](auto) -> wil::com_ptr<IUIAnimationTransition2>
                        {
                            wil::com_ptr<IUIAnimationTransition2> t;
                            THROW_IF_FAILED(library->CreateCubicBezierLinearTransition(popSec, maxRadius, 0, 0, 0, 1, &t));
                            THROW_IF_FAILED(t->SetInitialValue(0.0));
                            return t;
                        });

                    THROW_IF_FAILED(clip->SetBottomLeftRadiusX(radiusAnim.get()));
                    THROW_IF_FAILED(clip->SetBottomLeftRadiusY(radiusAnim.get()));
                    THROW_IF_FAILED(clip->SetBottomRightRadiusX(radiusAnim.get()));
                    THROW_IF_FAILED(clip->SetBottomRightRadiusY(radiusAnim.get()));
                    THROW_IF_FAILED(clip->SetTopLeftRadiusX(radiusAnim.get()));
                    THROW_IF_FAILED(clip->SetTopLeftRadiusY(radiusAnim.get()));
                    THROW_IF_FAILED(clip->SetTopRightRadiusX(radiusAnim.get()));
                    THROW_IF_FAILED(clip->SetTopRightRadiusY(radiusAnim.get()));

                    THROW_IF_FAILED(m_thumbnailVisual->SetClip(clip.get()));
                    THROW_IF_FAILED(m_backdropThumbnailVisual->SetClip(clip.get()));
                }

                // ================================================================
                // Стиль 2: горизонтальный слайд для thumbnail (только thumbnail,
                //          backdrop остаётся на месте) — BWM style 2
                // ================================================================
                else if (m_animationStyle == 2)
                {
                    POINT clientPoint = m_cursorPos;
                    THROW_LAST_ERROR_IF(!ScreenToClient(m_menuWindow, &clientPoint));

                    // Начальная сторона X: та, что дальше от курсора
                    float startX = (abs(width - static_cast<float>(clientPoint.x)) >
                        abs(0.f - static_cast<float>(clientPoint.x)))
                        ? 0.f : width;

                    auto anim = MakeAnimation([&](auto) -> wil::com_ptr<IUIAnimationTransition2>
                        {
                            wil::com_ptr<IUIAnimationTransition2> t;
                            THROW_IF_FAILED(library->CreateCubicBezierLinearTransition(popSec, 0.0, 0, 0, 0, 1, &t));
                            THROW_IF_FAILED(t->SetInitialValue(
                                static_cast<double>(-width) * (1.0 - static_cast<double>(m_ratio)) +
                                static_cast<double>(startX)));
                            return t;
                        });

                    THROW_IF_FAILED(m_thumbnailVisual->SetOffsetX(anim.get()));
                    // backdrop не смещается в этом стиле (только thumbnail)
                }

                // ================================================================
                // Стиль 3: масштабирование от центра окна (ScaleTransform3D)
                //          Эквивалентен BWM style 3
                // ================================================================
                else if (m_animationStyle == 3)
                {
                    auto scaleAnim = MakeAnimation([&](auto) -> wil::com_ptr<IUIAnimationTransition2>
                        {
                            wil::com_ptr<IUIAnimationTransition2> t;
                            THROW_IF_FAILED(library->CreateCubicBezierLinearTransition(popSec, 1.0, 0, 0, 0, 1, &t));
                            THROW_IF_FAILED(t->SetInitialValue(static_cast<double>(m_ratio)));
                            return t;
                        });

                    wil::com_ptr<IDCompositionScaleTransform3D> transform;
                    THROW_IF_FAILED(dcompDevice->CreateScaleTransform3D(&transform));
                    THROW_IF_FAILED(transform->SetCenterX(width / 2.f));
                    THROW_IF_FAILED(transform->SetCenterY(height / 2.f));
                    THROW_IF_FAILED(transform->SetScaleX(scaleAnim.get()));
                    THROW_IF_FAILED(transform->SetScaleY(scaleAnim.get()));

                    THROW_IF_FAILED(m_thumbnailVisual.query<IDCompositionVisual3>()->SetTransform(transform.get()));
                    THROW_IF_FAILED(m_backdropThumbnailVisual.query<IDCompositionVisual3>()->SetTransform(transform.get()));
                }

                // ================================================================
                // Стиль 4: горизонтальный слайд для обоих visual (thumbnail + backdrop)
                //          Эквивалентен BWM style 4
                // ================================================================
                else if (m_animationStyle == 4)
                {
                    POINT clientPoint = m_cursorPos;
                    THROW_LAST_ERROR_IF(!ScreenToClient(m_menuWindow, &clientPoint));

                    float startX = (abs(width - static_cast<float>(clientPoint.x)) >
                        abs(0.f - static_cast<float>(clientPoint.x)))
                        ? 0.f : width;

                    auto anim = MakeAnimation([&](auto) -> wil::com_ptr<IUIAnimationTransition2>
                        {
                            wil::com_ptr<IUIAnimationTransition2> t;
                            THROW_IF_FAILED(library->CreateCubicBezierLinearTransition(popSec, 0.0, 0, 0, 0, 1, &t));
                            THROW_IF_FAILED(t->SetInitialValue(
                                static_cast<double>(-width) * (1.0 - static_cast<double>(m_ratio)) +
                                static_cast<double>(startX)));
                            return t;
                        });

                    THROW_IF_FAILED(m_thumbnailVisual->SetOffsetX(anim.get()));
                    THROW_IF_FAILED(m_backdropThumbnailVisual->SetOffsetX(anim.get()));
                }

                // ================================================================
                // Стиль 5: прямоугольный clip-reveal от ближайшего угла к курсору
                //          (с фиксированным cornerRadius от темы)
                //          Эквивалентен BWM style 5
                // ================================================================
                else if (m_animationStyle == 5)
                {
                    POINT clientPoint = m_cursorPos;
                    THROW_LAST_ERROR_IF(!ScreenToClient(m_menuWindow, &clientPoint));

                    D2D1_POINT_2F startPoint
                    {
                        (abs(width - static_cast<float>(clientPoint.x)) >
                         abs(0.f - static_cast<float>(clientPoint.x))) ? 0.f : width,
                        (abs(height - static_cast<float>(clientPoint.y)) >
                         abs(0.f - static_cast<float>(clientPoint.y))) ? 0.f : height
                    };

                    wil::com_ptr<IDCompositionRectangleClip> clip;
                    THROW_IF_FAILED(dcompDevice->CreateRectangleClip(&clip));

                    THROW_IF_FAILED(clip->SetTop(
                        MakeAnimation([&](auto) -> wil::com_ptr<IUIAnimationTransition2>
                            {
                                wil::com_ptr<IUIAnimationTransition2> t;
                                THROW_IF_FAILED(library->CreateCubicBezierLinearTransition(
                                    popSec, static_cast<double>(startPoint.y) - height, 0, 0, 0, 1, &t));
                                THROW_IF_FAILED(t->SetInitialValue(
                                    static_cast<double>(startPoint.y) - static_cast<double>(height) * m_ratio));
                                return t;
                            }).get()));

                    THROW_IF_FAILED(clip->SetBottom(
                        MakeAnimation([&](auto) -> wil::com_ptr<IUIAnimationTransition2>
                            {
                                wil::com_ptr<IUIAnimationTransition2> t;
                                THROW_IF_FAILED(library->CreateCubicBezierLinearTransition(
                                    popSec, static_cast<double>(startPoint.y) + height, 0, 0, 0, 1, &t));
                                THROW_IF_FAILED(t->SetInitialValue(
                                    static_cast<double>(startPoint.y) + static_cast<double>(height) * m_ratio));
                                return t;
                            }).get()));

                    THROW_IF_FAILED(clip->SetLeft(
                        MakeAnimation([&](auto) -> wil::com_ptr<IUIAnimationTransition2>
                            {
                                wil::com_ptr<IUIAnimationTransition2> t;
                                THROW_IF_FAILED(library->CreateCubicBezierLinearTransition(
                                    popSec, static_cast<double>(startPoint.x) - width, 0, 0, 0, 1, &t));
                                THROW_IF_FAILED(t->SetInitialValue(
                                    static_cast<double>(startPoint.x) - static_cast<double>(width) * m_ratio));
                                return t;
                            }).get()));

                    THROW_IF_FAILED(clip->SetRight(
                        MakeAnimation([&](auto) -> wil::com_ptr<IUIAnimationTransition2>
                            {
                                wil::com_ptr<IUIAnimationTransition2> t;
                                THROW_IF_FAILED(library->CreateCubicBezierLinearTransition(
                                    popSec, static_cast<double>(startPoint.x) + width, 0, 0, 0, 1, &t));
                                THROW_IF_FAILED(t->SetInitialValue(
                                    static_cast<double>(startPoint.x) + static_cast<double>(width) * m_ratio));
                                return t;
                            }).get()));

                    // Фиксированный радиус из темы (0, 4 или 8)
                    THROW_IF_FAILED(clip->SetBottomLeftRadiusX(m_cornerRadius));
                    THROW_IF_FAILED(clip->SetBottomLeftRadiusY(m_cornerRadius));
                    THROW_IF_FAILED(clip->SetBottomRightRadiusX(m_cornerRadius));
                    THROW_IF_FAILED(clip->SetBottomRightRadiusY(m_cornerRadius));
                    THROW_IF_FAILED(clip->SetTopLeftRadiusX(m_cornerRadius));
                    THROW_IF_FAILED(clip->SetTopLeftRadiusY(m_cornerRadius));
                    THROW_IF_FAILED(clip->SetTopRightRadiusX(m_cornerRadius));
                    THROW_IF_FAILED(clip->SetTopRightRadiusY(m_cornerRadius));

                    THROW_IF_FAILED(m_thumbnailVisual->SetClip(clip.get()));
                    THROW_IF_FAILED(m_backdropThumbnailVisual->SetClip(clip.get()));
                }

                // ================================================================
                // Стиль 6: масштабирование от ближайшего угла к курсору
                //          Эквивалентен BWM style 6
                // ================================================================
                else if (m_animationStyle == 6)
                {
                    POINT clientPoint = m_cursorPos;
                    THROW_LAST_ERROR_IF(!ScreenToClient(m_menuWindow, &clientPoint));

                    D2D1_POINT_2F startPoint
                    {
                        (abs(width - static_cast<float>(clientPoint.x)) >
                         abs(0.f - static_cast<float>(clientPoint.x))) ? 0.f : width,
                        (abs(height - static_cast<float>(clientPoint.y)) >
                         abs(0.f - static_cast<float>(clientPoint.y))) ? 0.f : height
                    };

                    auto scaleAnim = MakeAnimation([&](auto) -> wil::com_ptr<IUIAnimationTransition2>
                        {
                            wil::com_ptr<IUIAnimationTransition2> t;
                            THROW_IF_FAILED(library->CreateCubicBezierLinearTransition(popSec, 1.0, 0, 0, 0, 1, &t));
                            THROW_IF_FAILED(t->SetInitialValue(static_cast<double>(m_ratio)));
                            return t;
                        });

                    wil::com_ptr<IDCompositionScaleTransform3D> transform;
                    THROW_IF_FAILED(dcompDevice->CreateScaleTransform3D(&transform));
                    THROW_IF_FAILED(transform->SetCenterX(startPoint.x));
                    THROW_IF_FAILED(transform->SetCenterY(startPoint.y));
                    THROW_IF_FAILED(transform->SetScaleX(scaleAnim.get()));
                    THROW_IF_FAILED(transform->SetScaleY(scaleAnim.get()));

                    THROW_IF_FAILED(m_thumbnailVisual.query<IDCompositionVisual3>()->SetTransform(transform.get()));
                    THROW_IF_FAILED(m_backdropThumbnailVisual.query<IDCompositionVisual3>()->SetTransform(transform.get()));
                }

                HWND const flyoutForNc = m_flyoutHwnd ? m_flyoutHwnd : m_menuWindow;
                if (flyoutForNc && GlassCore::FlyoutFrame::IsHostBorderDeferred(flyoutForNc))
                    GlassCore::FlyoutFrame::SyncPrimeFlyoutNcBlack(flyoutForNc);

                PrimeDeferredFlyoutThumbnailForAnimation();

                THROW_IF_FAILED(dcompDevice->Commit());
                (void)DwmFlush();
                Restart(m_totDuration);
                m_started = true;
                Animator(startTimeStamp);
            }
            catch (...)
            {
                // Fix 2: полная очистка ресурсов при ошибке (как в BWM)
                Finish();
                if (m_menuWindow)
                {
                    SendNotifyMessageW(m_menuWindow, GetAnimationFinishedMsg(), 0, 0);
                    m_menuWindow = nullptr;
                }
                if (window)
                {
                    SendNotifyMessageW(window, WM_CLOSE, 0, 0);
                    window = nullptr;
                }
                if (m_backdropWindow)
                {
                    SendNotifyMessageW(m_backdropWindow, WM_CLOSE, 0, 0);
                    m_backdropWindow = nullptr;
                }
                if (m_thumbnail)
                {
                    DwmUnregisterThumbnail(m_thumbnail);
                    m_thumbnail = nullptr;
                }
                if (m_backdropThumbnail)
                {
                    DwmUnregisterThumbnail(m_backdropThumbnail);
                    m_backdropThumbnail = nullptr;
                }
                LOG_CAUGHT_EXCEPTION();
            }
        }

        void Finish()
        {
            if (m_flyoutHwnd && IsWindow(m_flyoutHwnd))
                Utils::CloakWindow(m_flyoutHwnd, FALSE);
            RefreshFlyoutFrameAfterUncloak();
            Utils::CloakWindow(window, TRUE);
            DwmFlush();
        }

        void Attach()
        {
            THROW_LAST_ERROR_IF(!SetWindowSubclass(m_menuWindow, SubclassProc, 0, reinterpret_cast<DWORD_PTR>(this)));
        }

        void Detach()
        {
            if (m_menuWindow)
            {
                if (RemoveWindowSubclass(m_menuWindow, SubclassProc, 0))
                    m_menuWindow = nullptr;
            }
        }

        static LRESULT CALLBACK SubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR dwRefData)
        {
            auto& self = *reinterpret_cast<PopupIn*>(dwRefData);

            if (uMsg == GetDeferredPopupInStartMsg())
            {
                if (IsWindow(hWnd) && !self.m_started)
                {
                    HWND shadow = GetWindow(hWnd, GW_HWNDNEXT);
                    if (shadow && IsWindowClass(shadow, L"SysShadow") && IsWindowVisible(shadow))
                        self.m_useSysDropShadow = true;
                    self.Start();
                }
                return DefSubclassProc(hWnd, uMsg, wParam, lParam);
            }

            if (uMsg == WM_WINDOWPOSCHANGED)
            {
                auto& wp = *reinterpret_cast<WINDOWPOS*>(lParam);
                if (wp.flags & SWP_SHOWWINDOW)
                {
                    LRESULT lr = DefSubclassProc(hWnd, uMsg, wParam, lParam);

                    // Fix 3: проверяем класс окна тени ("SysShadow"), а не IsPopupMenu
                    HWND shadow = GetWindow(hWnd, GW_HWNDNEXT);
                    if (shadow && IsWindowClass(shadow, L"SysShadow") && IsWindowVisible(shadow))
                        self.m_useSysDropShadow = true;

                    self.Start();
                    return lr;
                }
                if (wp.flags & SWP_HIDEWINDOW)
                {
                    self.SetDuration(0ms);
                    self.Detach();
                }
            }

            if (uMsg == MN_SELECTITEM || uMsg == WM_MOUSEHOVER)
            {
                int pos = static_cast<int>(wParam);
                if (pos != -1 && self.m_immediateInterupting)
                    self.SetDuration(0ms);
            }

            if (uMsg == WM_DESTROY)
            {
                self.SetDuration(0ms);
                self.Detach();
            }

        if (uMsg == GetAnimationFinishedMsg())
        {
            // Post-uncloak: однократная отрисовка рамки на real flyout (sync, без WM_GLASS).
            GlassCore::Menu::PaintDeferredFlyoutBorderIfNeeded(hWnd);
            GlassCore::DropDown::OnFlyoutAnimationFinished(hWnd);
            RemoveWindowSubclass(hWnd, SubclassProc, 0);
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

        // Event-based shutdown: GUI-thread removes subclass and signals completion
        if (uMsg == GlassCoreMsg::WM_GLASS_ANIM_DETACH)
        {
            self.Detach();

            if (AnimationWorker::GetInstance().g_pendingAnimDetaches.fetch_sub(1) == 1)
            {
                SetEvent(AnimationWorker::GetInstance().m_hAnimDetachDone);
            }
            return 0;
        }

            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

    public:
        [[nodiscard]] HWND FlyoutHwnd() const noexcept { return m_flyoutHwnd; }
        [[nodiscard]] HWND MenuWindow() const noexcept { return m_menuWindow; }

        PopupIn(HWND hWnd, HWND ownerWindow, float startPosRatio,
            std::chrono::milliseconds popupInDuration,
            std::chrono::milliseconds fadeInDuration,
            DWORD animationStyle, bool immediateInterupting)
            : AnimationInfo(std::chrono::milliseconds::max())
            , m_totDuration(std::max(popupInDuration, fadeInDuration))
            , m_menuWindow(hWnd)
            , m_flyoutHwnd(hWnd)
            , m_fadeInDuration(fadeInDuration)
            , m_popupInDuration(popupInDuration)
            , m_ratio(startPosRatio)
            , m_animationStyle(animationStyle)
            , m_immediateInterupting(immediateInterupting)
        {
            if (!EnsureDwmApi())
                THROW_HR(E_FAIL);

            auto context = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            auto dpiCleanup = wil::scope_exit([&] { SetThreadDpiAwarenessContext(context); });

            auto& dcomp = Shared::DXHelper::LazyDComposition::GetInstance();
            dcomp.EnsureResources();
            auto dcompDevice = dcomp.GetDCompositionDevice();
            THROW_HR_IF_NULL(E_FAIL, dcompDevice);

            Attach();
            Utils::CloakWindow(m_menuWindow, TRUE);

            m_backdropWindow = Utils::SHCreateWorkerWindowW(
                DefWindowProcW,
                ownerWindow,
                WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                WS_POPUP,
                nullptr,
                0
            );
            THROW_LAST_ERROR_IF_NULL(m_backdropWindow);

            window = Utils::SHCreateWorkerWindowW(
                DefWindowProcW,
                ownerWindow,
                WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                WS_POPUP,
                nullptr,
                0
            );
            THROW_LAST_ERROR_IF_NULL(window);

            THROW_LAST_ERROR_IF(!SetLayeredWindowAttributes(window, 0, 0, LWA_ALPHA));
            THROW_LAST_ERROR_IF(!SetLayeredWindowAttributes(m_backdropWindow, 0, 0, LWA_ALPHA));
            Utils::CloakWindow(window, TRUE);
            THROW_LAST_ERROR_IF(!SetWindowPos(m_backdropWindow, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS | SWP_NOSENDCHANGING));

            THROW_IF_FAILED(dcompDevice->CreateTargetForHwnd(window, TRUE, &m_dcompTarget));

            DWM_THUMBNAIL_PROPERTIES thumbProps = {
                DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION | DWM_TNP_SOURCECLIENTAREAONLY | Shared::DwmThumbnailAPI::DWM_TNP_ENABLE3D,
                {0,0,0,0}, {}, 255, TRUE, TRUE
            };
            THROW_IF_FAILED(Shared::DwmThumbnailAPI::DwmpCreateSharedThumbnailVisual(
                window, m_menuWindow, 0, &thumbProps,
                dcompDevice.get(), m_thumbnailVisual.put_void(), &m_thumbnail));

            thumbProps.fSourceClientAreaOnly = FALSE;
            THROW_IF_FAILED(Shared::DwmThumbnailAPI::DwmpCreateSharedThumbnailVisual(
                window, m_backdropWindow, 0, &thumbProps,
                dcompDevice.get(), m_backdropThumbnailVisual.put_void(), &m_backdropThumbnail));

            THROW_IF_FAILED(dcompDevice->CreateVisual(&m_rootVisual));
            THROW_IF_FAILED(m_rootVisual->AddVisual(m_backdropThumbnailVisual.get(), FALSE, nullptr));
            THROW_IF_FAILED(m_rootVisual->AddVisual(m_thumbnailVisual.get(), FALSE, nullptr));
            THROW_IF_FAILED(m_dcompTarget->SetRoot(m_rootVisual.get()));
            THROW_IF_FAILED(dcompDevice->Commit());
            THROW_IF_FAILED(DwmFlush());

            // См. Fix 6: гарантируем попытку Start после полной инициализации, если SWP_SHOWWINDOW
            // уже не придёт в сабкласс анимации (или пришёл только SWP_FRAMECHANGED).
            (void)PostMessageW(m_menuWindow, GetDeferredPopupInStartMsg(), 0, 0);
        }

        virtual ~PopupIn() noexcept
        {
            Finish();
            if (m_menuWindow)
            {
                HWND menuWnd = m_menuWindow;
                m_menuWindow = nullptr; // Обнуляем до Send, чтобы повторный вход в Detach() был no-op

                // Fallback: если event-based shutdown не отработал (GUI-thread мёртв),
                // шлём GetAnimationFinishedMsg через SendMessageTimeout.
                // В нормальном случае m_menuWindow уже nullptr (Detach() вызван
                // из WM_GLASS_ANIM_DETACH handler на GUI-thread).
                DWORD_PTR sendResult = 0;
                SendMessageTimeoutW(
                    menuWnd, GetAnimationFinishedMsg(), 0, 0,
                    SMTO_ABORTIFHUNG, 1000, &sendResult);
            }
            if (window)
            {
                SendNotifyMessageW(window, WM_CLOSE, 0, 0);
                window = nullptr;
            }
            if (m_backdropWindow)
            {
                SendNotifyMessageW(m_backdropWindow, WM_CLOSE, 0, 0);
                m_backdropWindow = nullptr;
            }
            if (m_thumbnail)
            {
                DwmUnregisterThumbnail(m_thumbnail);
                m_thumbnail = nullptr;
            }
            if (m_backdropThumbnail)
            {
                DwmUnregisterThumbnail(m_backdropThumbnail);
                m_backdropThumbnail = nullptr;
            }
        }

        void Animator(ULONGLONG currentTimeStamp) override
        {
            try
            {
                if (!m_started) return;
                THROW_HR_IF(E_INVALIDARG, !IsWindow(window) || !IsWindow(m_menuWindow) || !IsWindow(m_backdropWindow));

                BYTE opacity = static_cast<BYTE>(
                    static_cast<int>(
                        std::round((static_cast<float>(std::min(currentTimeStamp - startTimeStamp, static_cast<ULONGLONG>(m_fadeInDuration.count()))) /
                            static_cast<float>(m_fadeInDuration.count())) * 255.f)
                        )
                    );
                THROW_LAST_ERROR_IF(!SetLayeredWindowAttributes(window, 0, opacity, LWA_ALPHA));

                RECT rc{};
                THROW_LAST_ERROR_IF(!GetWindowRect(m_menuWindow, &rc));
                SIZE size = { rc.right - rc.left, rc.bottom - rc.top };

                THROW_LAST_ERROR_IF(!SetWindowPos(m_backdropWindow, window, rc.left, rc.top, size.cx, size.cy,
                    SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS | SWP_NOSENDCHANGING));
                THROW_LAST_ERROR_IF(!SetWindowPos(window, nullptr, rc.left, rc.top, size.cx, size.cy,
                    SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS | SWP_NOSENDCHANGING));

                if (m_thumbnail)
                {
                    DWM_THUMBNAIL_PROPERTIES props = {
                        DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION | DWM_TNP_SOURCECLIENTAREAONLY | Shared::DwmThumbnailAPI::DWM_TNP_ENABLE3D,
                        {0,0,size.cx,size.cy}, {0,0,0,0}, 255, TRUE, TRUE
                    };
                    THROW_IF_FAILED(DwmUpdateThumbnailProperties(m_thumbnail, &props));
                }
                if (m_backdropThumbnail)
                {
                    DWM_THUMBNAIL_PROPERTIES props = {
                        DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION | Shared::DwmThumbnailAPI::DWM_TNP_ENABLE3D,
                        {0,0,size.cx,size.cy}, {0,0,0,0}, 255, TRUE, TRUE
                    };
                    THROW_IF_FAILED(DwmUpdateThumbnailProperties(m_backdropThumbnail, &props));
                }
            }
            CATCH_LOG()
        }
    };

    namespace
    {
        void RefreshFlyoutNcAfterUncloak(
            HWND hwnd,
            bool darkMode,
            const GlassCore::Settings::BorderContext& border,
            unsigned effectType,
            std::wstring_view part) noexcept
        {
            if (!hwnd || !IsWindow(hwnd))
                return;

            GlassCore::FlyoutFrame::PaintHostBorder(hwnd, darkMode, border, effectType);

            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][Animation] RefreshFlyoutFrameAfterUncloak: hwnd=0x%p part=%.*s corner=%d ET=%u",
                hwnd,
                static_cast<int>(part.size()),
                part.data(),
                static_cast<int>(border.cornerType),
                effectType);
        }
    } // namespace

    // ---------- Специализация для меню ----------
    class MenuPopupIn : public PopupIn
    {
    public:
        MenuPopupIn(HWND hWnd, HWND ownerWindow, float startPosRatio,
            std::chrono::milliseconds popupInDuration,
            std::chrono::milliseconds fadeInDuration,
            DWORD animationStyle, bool immediateInterupting)
            : PopupIn(hWnd, ownerWindow, startPosRatio, popupInDuration, fadeInDuration, animationStyle, immediateInterupting)
        {
        }

    protected:
        // Post-uncloak NC: один WM_GLASS из AnimationFinishedMsg на GUI-потоке меню.
        void RefreshFlyoutFrameAfterUncloak() noexcept override {}

        // Fix 4: backdrop без видимого DWM-контура во время menu pop-in.
        void SetBackdropEffectForAnimation() override
        {
            // Совпадает с merged.useDarkMode в MenuEvents::OnPopupSized (DTB / UAHINITMENU).
            const bool dark = GlassCore::Menu::MenuHandler::GetContext().useDarkMode;
            GlassCore::Settings::BorderContext border;
            GlassCore::Settings::WindowBackdropEffectContext backdrop;

            GlassCore::Settings::QueryBorderContext(L"Menu", dark, border);
            GlassCore::Settings::QueryBackdropEffectContext(L"Menu", dark, backdrop);

            DebugLog(DBG_IMPORTANT, L"[GlassCore][Animation] MenuPopupIn SetBackdropEffectForAnimation: dark=%d effectType=%u enableDropShadow=%u gradientColor=0x%08X cornerType=%d",
                dark, backdrop.effectType, backdrop.enableDropShadow, backdrop.gradientColor, border.cornerType);

            const DWORD build = GetBuildNumber();

            // Без скруглённого DComp-контура во время анимации — иначе видна «пустая рамка».
            m_cornerRadius = 0.f;

            if (build < 22000 || border.cornerType == DWMWCP_DONOTROUND)
            {
                if (backdrop.enableDropShadow &&
                    (backdrop.effectType == 4 || backdrop.effectType == 5))
                {
                    EffectHelper::SetWindowBackdrop(window, TRUE, 0, EffectHelper::EffectType::AcrylicBlur);
                }
            }
            else
            {
                const DWM_WINDOW_CORNER_PREFERENCE noRound = DWMWCP_DONOTROUND;
                DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &noRound, sizeof(noRound));
                const COLORREF borderNone = DWMWA_COLOR_NONE;
                DwmSetWindowAttribute(window, DWMWA_BORDER_COLOR, &borderNone, sizeof(borderNone));
            }

            EffectHelper::EnableWindowDarkMode(window, dark);

            // Backdrop: только blur/glass, без border (ApplyPopupSwcaEffect → ApplyBorderEffect убран).
            GlassCore::Settings::ApplyBackdropEffect(m_backdropWindow, dark, backdrop, false, true);

            if (build >= 22000)
            {
                const DWM_WINDOW_CORNER_PREFERENCE noRound = DWMWCP_DONOTROUND;
                DwmSetWindowAttribute(m_backdropWindow, DWMWA_WINDOW_CORNER_PREFERENCE, &noRound, sizeof(noRound));
                const COLORREF borderNone = DWMWA_COLOR_NONE;
                DwmSetWindowAttribute(m_backdropWindow, DWMWA_BORDER_COLOR, &borderNone, sizeof(borderNone));
            }

            if (backdrop.effectType >= 6 && backdrop.effectType <= 8)
            {
                DefWindowProcW(m_backdropWindow, WM_NCACTIVATE, TRUE, 0);
                SetWindowPos(m_backdropWindow, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                    SWP_DRAWFRAME | SWP_NOACTIVATE);
            }
        }
    };

    // ---------- Специализация для выпадающих списков ----------
    class DropDownPopupIn : public PopupIn
    {
    public:
        DropDownPopupIn(HWND hWnd, HWND ownerWindow, float startPosRatio,
            std::chrono::milliseconds popupInDuration,
            std::chrono::milliseconds fadeInDuration,
            DWORD animationStyle, bool immediateInterupting)
            : PopupIn(hWnd, ownerWindow, startPosRatio, popupInDuration, fadeInDuration, animationStyle, immediateInterupting)
        {
        }

    protected:
        void RefreshFlyoutFrameAfterUncloak() noexcept override {}

        // Используем снимок g_dropDownCtx, заполненный в OnListviewPopupAttach / OnDropDownShow
        void SetBackdropEffectForAnimation() override
        {
            const auto& ctx = WndCompat::g_dropDownCtx;
            const bool dark = ctx.useDarkMode;
            const auto& border = ctx.border;
            const auto& backdrop = ctx.backdropEffect;

            const DWORD build = GetBuildNumber();

            m_cornerRadius = 0.f;

            if (build < 22000 || border.cornerType == DWMWCP_DONOTROUND)
            {
                if (backdrop.enableDropShadow &&
                    (backdrop.effectType == 4 || backdrop.effectType == 5))
                {
                    EffectHelper::SetWindowBackdrop(window, TRUE, 0, EffectHelper::EffectType::AcrylicBlur);
                }
            }
            else
            {
                const DWM_WINDOW_CORNER_PREFERENCE noRound = DWMWCP_DONOTROUND;
                DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &noRound, sizeof(noRound));
                const COLORREF borderNone = DWMWA_COLOR_NONE;
                DwmSetWindowAttribute(window, DWMWA_BORDER_COLOR, &borderNone, sizeof(borderNone));
            }

            EffectHelper::EnableWindowDarkMode(window, dark);

            GlassCore::Settings::ApplyBackdropEffect(m_backdropWindow, dark, backdrop, false, true);

            if (build >= 22000)
            {
                const DWM_WINDOW_CORNER_PREFERENCE noRound = DWMWCP_DONOTROUND;
                DwmSetWindowAttribute(m_backdropWindow, DWMWA_WINDOW_CORNER_PREFERENCE, &noRound, sizeof(noRound));
                const COLORREF borderNone = DWMWA_COLOR_NONE;
                DwmSetWindowAttribute(m_backdropWindow, DWMWA_BORDER_COLOR, &borderNone, sizeof(borderNone));
            }

            if (backdrop.effectType >= 6 && backdrop.effectType <= 8)
            {
                DefWindowProcW(m_backdropWindow, WM_NCACTIVATE, TRUE, 0);
                SetWindowPos(m_backdropWindow, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                    SWP_DRAWFRAME | SWP_NOACTIVATE);
            }
        }
    };

    // ---------- Реализация AnimationWorker ----------
    void AnimationWorker::Schedule(std::shared_ptr<AnimationInfo> info)
    {
        auto lock = m_lock.lock_exclusive();
        m_animationStorage.push_back(info);

        if (m_threadId == 0)
        {
            // Закрываем handle завершившегося потока перед созданием нового
            if (m_threadHandle)
            {
                CloseHandle(m_threadHandle);
                m_threadHandle = nullptr;
            }

            HANDLE hRaw = CreateThread(nullptr, 0, ThreadProc, this, 0, &m_threadId);
            m_threadHandle = hRaw; // владеем handle; закроем в Shutdown() или следующем Schedule()
        }
    }

    DWORD WINAPI AnimationWorker::ThreadProc(LPVOID param)
    {
        auto& self = *static_cast<AnimationWorker*>(param);
        auto moduleRef = wil::get_module_reference_for_thread();

        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        SetThreadDescription(GetCurrentThread(), L"GlassCore Animation Worker");

        constexpr ULONGLONG maxDelay = 1000;
        ULONGLONG idleStart = 0;

        std::vector<std::shared_ptr<AnimationInfo>> localStorage;

        while (true)
        {
            bool shouldShutdown = false;
            {
                auto lock = self.m_lock.lock_exclusive();
                shouldShutdown = self.m_shutdownFlag.load(std::memory_order_acquire);
                localStorage.insert(localStorage.end(), self.m_animationStorage.begin(), self.m_animationStorage.end());
                self.m_animationStorage.clear();
            }

            if (shouldShutdown)
            {
                DebugLog(L"[GlassCore][Animation] ThreadProc: shutdown, clearing %zu animations",
                    localStorage.size());

                // Event-based shutdown: post WM_GLASS_ANIM_DETACH to all active menu windows
                // GUI-threads will remove subclasses and signal completion
                int pendingCount = 0;
                for (auto& anim : localStorage)
                {
                    if (auto* popup = dynamic_cast<PopupIn*>(anim.get()))
                    {
                        HWND menuWnd = popup->MenuWindow();
                        if (menuWnd && IsWindow(menuWnd))
                        {
                            ++pendingCount;
                        }
                    }
                }

                if (pendingCount > 0)
                {
                    // Initialize event if needed
                    if (!self.m_hAnimDetachDone)
                    {
                        self.m_hAnimDetachDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                    }

                    ResetEvent(self.m_hAnimDetachDone);
                    self.g_pendingAnimDetaches.store(pendingCount);

                    int actualPending = 0;
                    for (auto& anim : localStorage)
                    {
                        if (auto* popup = dynamic_cast<PopupIn*>(anim.get()))
                        {
                            HWND menuWnd = popup->MenuWindow();
                            if (menuWnd && IsWindow(menuWnd))
                            {
                                BOOL posted = PostMessageW(menuWnd,
                                    GlassCoreMsg::WM_GLASS_ANIM_DETACH, 0, 0);
                                if (posted)
                                {
                                    ++actualPending;
                                }
                                else
                                {
                                    // Окно уже уничтожено — не считаем его.
                                    DebugLog(L"[GlassCore][Animation] PostMessageW FAILED for menu wnd=0x%p",
                                        menuWnd);
                                }
                            }
                        }
                    }

                    if (actualPending == 0)
                    {
                        // Все окна уже уничтожены — сигналим сразу.
                        DebugLog(L"[GlassCore][Animation] All menu windows already destroyed, signaling immediately");
                        if (self.m_hAnimDetachDone)
                        {
                            CloseHandle(self.m_hAnimDetachDone);
                            self.m_hAnimDetachDone = nullptr;
                        }
                        self.g_pendingAnimDetaches.store(0, std::memory_order_release);
                    }
                    else
                    {
                        // Корректируем счётчик если some PostMessageW failed.
                        self.g_pendingAnimDetaches.store(actualPending, std::memory_order_release);

                        DebugLog(L"[GlassCore][Animation] Waiting for %d GUI detachments", actualPending);
                        WaitForSingleObject(self.m_hAnimDetachDone, INFINITE);
                        DebugLog(L"[GlassCore][Animation] All GUI detachments complete");

                        // ОчищаемHANDLE и сбрасываем счётчик для следующего цикла.
                        if (self.m_hAnimDetachDone)
                        {
                            CloseHandle(self.m_hAnimDetachDone);
                            self.m_hAnimDetachDone = nullptr;
                        }
                        self.g_pendingAnimDetaches.store(0, std::memory_order_release);
                    }
                }

                // Now safe to destroy PopupIn objects (subclasses already removed)
                localStorage.clear();
                self.m_threadId = 0;
                break;
            }

            ULONGLONG now = GetTickCount64();
            if (localStorage.empty())
            {
                if (now - idleStart >= maxDelay)
                {
                    self.m_threadId = 0;
                    break;
                }
            }
            else
            {
                idleStart = now;
                for (auto it = localStorage.begin(); it != localStorage.end();)
                {
                    auto info = *it;
                    now = GetTickCount64();
                    if (now <= info->endTimeStamp)
                    {
                        info->Animator(now);
                        ++it;
                    }
                    else
                    {
                        if (auto* menuAnim = dynamic_cast<MenuPopupIn*>(info.get()))
                        {
                            DebugLog(DBG_IMPORTANT,
                                L"[GlassCore][Animation] MenuPopupIn finished hwnd=0x%p",
                                menuAnim->FlyoutHwnd());
                        }
                        it = localStorage.erase(it);
                    }
                }
            }

            // Пропускаем DwmFlush если уже запрошен shutdown — иначе поток может
            // заблокироваться в DwmFlush на непредсказуемое время (при нагрузке GPU,
            // переключении сессии и т.п.) и задержать выгрузку GlassCore.
            if (self.m_shutdownFlag.load(std::memory_order_relaxed))
                continue;

            if (FAILED(DwmFlush()))
                Sleep(10);
        }
        return 0;
    }

    // ---------- Публичные функции (экспортируемые) ----------

    HRESULT CreateMenuFadeOut(HWND hWnd, const MENUBARINFO& mbi, std::chrono::milliseconds duration)
    {
        try
        {
            auto anim = std::make_shared<FadeOut>(hWnd, mbi, duration);
            AnimationWorker::GetInstance().Schedule(anim);
            return S_OK;
        }
        CATCH_RETURN()
    }

    HRESULT CreateMenuPopupIn(HWND hWnd, float startPosRatio,
        std::chrono::milliseconds popInDuration,
        std::chrono::milliseconds fadeInDuration,
        DWORD animationStyle)
    {
        try
        {
            GlassCore::Settings::FlyoutAnimationContext ctx;
            GlassCore::Settings::QueryFlyoutAnimationContext(L"Menu", ctx);
            if (!ctx.enable)
                return S_FALSE;

            auto anim = std::make_shared<MenuPopupIn>(
                hWnd,
                Shared::MenuUtils::GetCurrentMenuOwner(),
                startPosRatio,
                popInDuration,
                fadeInDuration,
                animationStyle,
                ctx.immediateInterupting
            );
            AnimationWorker::GetInstance().Schedule(anim);
            return S_OK;
        }
        CATCH_RETURN()
    }

    HRESULT CreateDropDownPopupIn(HWND hWnd, float startPosRatio,
        std::chrono::milliseconds popInDuration,
        std::chrono::milliseconds fadeInDuration,
        DWORD animationStyle,
        bool immediateInterupting)
    {
        try
        {
            auto anim = std::make_shared<DropDownPopupIn>(
                hWnd,
                reinterpret_cast<HWND>(GetWindowLongPtrW(hWnd, GWLP_HWNDPARENT)),
                startPosRatio,
                popInDuration,
                fadeInDuration,
                animationStyle,
                immediateInterupting
            );
            AnimationWorker::GetInstance().Schedule(anim);
            return S_OK;
        }
        CATCH_RETURN()
    }

    void Shutdown()
    {
        AnimationWorker::GetInstance().Shutdown();
    }

    void ShutdownHostExit() noexcept
    {
        AnimationWorker::GetInstance().ShutdownWithTimeout(50);
    }
}