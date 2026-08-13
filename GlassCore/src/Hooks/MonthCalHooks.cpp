#include "CorePch.h"

#include <Hooks/MonthCal.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/Settings.h>
#include <Subsystems/Animation.h>
#include <Helpers/ThemeHelper.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/PaintDispatcher.h>
#include <GlassControlKind.h>
#include <wil.h>
#include <uxtheme.h>
#include <CommCtrl.h>
#include <atomic>

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::MonthCal
{
    thread_local int g_paintRef = 0;
    thread_local int g_animRef  = 0;

    static std::atomic<int> g_enableRef{ 0 };

    inline bool IsMonthCalActive() noexcept
    {
        return g_paintRef > 0 || g_animRef > 0;
    }

    HRESULT WINAPI MyDrawThemeBackground(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, LPCRECT pClipRect);

    HRESULT WINAPI MyDrawThemeText(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCWSTR pszText, int cchText, DWORD dwTextFlags,
        DWORD dwTextFlags2, LPCRECT pRect);

    HRESULT WINAPI MyDrawThemeTextEx(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCWSTR pszText, int cchText, DWORD dwTextFlags,
        LPRECT pRect, const DTTOPTS* pOptions);

    HBRUSH WINAPI MyCreateSolidBrush(COLORREF color);

    enum HookIndex : size_t
    {
        HK_DRAWTHEMEBACKGROUND = 0,
        HK_DRAWTHEMETEXT,
        HK_DRAWTHEMETEXTEX,
        HK_CREATESOLIDBRUSH,
        HK_COUNT
    };

    inline const HookDispatcherDependency<HK_COUNT, 3> g_hookDependency
    { {
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
        std::tuple
        {
            std::array<std::string_view, 3>
            {
                "uxtheme.dll"sv,
                "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv,
                ""sv
            },
            "DrawThemeText",
            reinterpret_cast<PVOID>(MyDrawThemeText)
        },
        std::tuple
        {
            std::array<std::string_view, 3>
            {
                "uxtheme.dll"sv,
                "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv,
                ""sv
            },
            "DrawThemeTextEx",
            reinterpret_cast<PVOID>(MyDrawThemeTextEx)
        },
        std::tuple
        {
            std::array<std::string_view, 3>
            {
                "gdi32.dll"sv,
                "ext-ms-win-gdi-desktop-l1-1-0.dll"sv,
                ""sv
            },
            "CreateSolidBrush",
            reinterpret_cast<PVOID>(MyCreateSolidBrush)
        },
    } };

    static HookDispatcher<HK_COUNT, 3> g_dispatcher{ g_hookDependency, nullptr };
    static wil::srwlock                g_lock;
    static bool                        g_hookDataCached = false;

    [[nodiscard]] static void* ResolveComctlModuleAddress() noexcept
    {
        if (GlassCore::ModuleHandler::g_comctl32New)
            return GlassCore::ModuleHandler::g_comctl32New;
        return GlassCore::ModuleHandler::g_comctl32Legacy;
    }

    static void EnsureDispatcherModuleLocked() noexcept
    {
        g_dispatcher.moduleAddress = ResolveComctlModuleAddress();
        if (!g_dispatcher.moduleAddress)
            return;
        if (!g_hookDataCached)
        {
            g_dispatcher.CacheHookData();
            g_hookDataCached = true;
        }
    }

    static HRESULT BlackRect(HDC hdc, LPCRECT pRect, LPCRECT pClipRect)
    {
        RECT clip = *pRect;
        if (pClipRect)
            IntersectRect(&clip, &clip, pClipRect);

        PatBlt(hdc,
            clip.left, clip.top,
            clip.right - clip.left,
            clip.bottom - clip.top,
            BLACKNESS);
        return S_OK;
    }

    [[nodiscard]] static bool IsMonthCalThemeClass(HTHEME hTheme) noexcept
    {
        WCHAR themeClass[MAX_PATH + 1]{};
        return SUCCEEDED(Shared::ThemeHelper::GetThemeClass(hTheme, themeClass, MAX_PATH))
            && _wcsicmp(themeClass, L"MonthCal") == 0;
    }

    HRESULT WINAPI MyDrawThemeBackground(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, LPCRECT pClipRect)
    {
        auto orig = g_dispatcher.GetOrg<HK_DRAWTHEMEBACKGROUND,
            decltype(&DrawThemeBackground)>();
        if (!orig)
            return DrawThemeBackground(hTheme, hdc, iPartId, iStateId,
                pRect, pClipRect);

        if (!IsMonthCalActive())
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        WCHAR themeClass[MAX_PATH + 1]{};
        if (FAILED(Shared::ThemeHelper::GetThemeClass(
            hTheme, themeClass, MAX_PATH)))
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        if (_wcsicmp(themeClass, L"MonthCal") == 0)
        {
            if (iPartId == 1 || iPartId == 3)
                return BlackRect(hdc, pRect, pClipRect);
        }
        else if (_wcsicmp(themeClass, L"DatePicker") == 0)
        {
            if (iPartId == 0 || iPartId == 2)
                return BlackRect(hdc, pRect, pClipRect);
        }

        return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
    }

    HRESULT WINAPI MyDrawThemeText(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCWSTR pszText, int cchText, DWORD dwTextFlags,
        DWORD dwTextFlags2, LPCRECT pRect)
    {
        auto orig = g_dispatcher.GetOrg<HK_DRAWTHEMETEXT,
            decltype(&DrawThemeText)>();
        if (!orig)
            return DrawThemeText(hTheme, hdc, iPartId, iStateId,
                pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

        if (!IsMonthCalActive())
            return orig(hTheme, hdc, iPartId, iStateId,
                pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

        if (!IsMonthCalThemeClass(hTheme))
            return orig(hTheme, hdc, iPartId, iStateId,
                pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

        if (!pszText || cchText == 0 || !pRect || IsRectEmpty(pRect))
            return orig(hTheme, hdc, iPartId, iStateId,
                pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

        DTTOPTS options{ sizeof(DTTOPTS), DTT_COMPOSITED };
        return GlassCore::Text::DrawThemeContent(
            hdc,
            *pRect,
            nullptr,
            nullptr,
            0,
            [&](HDC memoryDC, HPAINTBUFFER, RGBQUAD*, int)
            {
                DrawThemeTextEx(
                    hTheme,
                    memoryDC,
                    iPartId,
                    iStateId,
                    pszText,
                    cchText,
                    dwTextFlags,
                    const_cast<LPRECT>(pRect),
                    &options);
            });
    }

    HRESULT WINAPI MyDrawThemeTextEx(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCWSTR pszText, int cchText, DWORD dwTextFlags,
        LPRECT pRect, const DTTOPTS* pOptions)
    {
        auto orig = g_dispatcher.GetOrg<HK_DRAWTHEMETEXTEX,
            decltype(&DrawThemeTextEx)>();
        if (!orig)
            return DrawThemeTextEx(hTheme, hdc, iPartId, iStateId,
                pszText, cchText, dwTextFlags, pRect, pOptions);

        if (!IsMonthCalActive())
            return orig(hTheme, hdc, iPartId, iStateId,
                pszText, cchText, dwTextFlags, pRect, pOptions);

        if (!IsMonthCalThemeClass(hTheme))
            return orig(hTheme, hdc, iPartId, iStateId,
                pszText, cchText, dwTextFlags, pRect, pOptions);

        if (!pRect || IsRectEmpty(pRect))
            return orig(hTheme, hdc, iPartId, iStateId,
                pszText, cchText, dwTextFlags, pRect, pOptions);

        const DTTOPTS* effectiveOpts = pOptions;
        DTTOPTS defaultOpts{ sizeof(DTTOPTS) };
        if (!effectiveOpts)
            effectiveOpts = &defaultOpts;

        if (effectiveOpts->dwFlags & DTT_CALCRECT)
        {
            DTTOPTS measure = *effectiveOpts;
            if (measure.dwSize < sizeof(DTTOPTS))
                measure.dwSize = sizeof(DTTOPTS);
            return orig(hTheme, hdc, iPartId, iStateId,
                pszText, cchText, dwTextFlags, pRect, &measure);
        }

        DTTOPTS work = *effectiveOpts;
        if (work.dwSize < sizeof(DTTOPTS))
            work.dwSize = sizeof(DTTOPTS);
        work.dwFlags |= DTT_COMPOSITED;

        return GlassCore::Text::DrawThemeContent(
            hdc,
            *pRect,
            nullptr,
            nullptr,
            0,
            [&](HDC memoryDC, HPAINTBUFFER, RGBQUAD*, int)
            {
                orig(hTheme, memoryDC, iPartId, iStateId,
                    pszText, cchText, dwTextFlags, pRect, &work);
            });
    }

    HBRUSH WINAPI MyCreateSolidBrush(COLORREF color)
    {
        auto orig = g_dispatcher.GetOrg<HK_CREATESOLIDBRUSH,
            decltype(&CreateSolidBrush)>();
        if (!orig)
            return CreateSolidBrush(color);

        if (!IsMonthCalActive())
            return orig(color);

        if (color == GetSysColor(COLOR_WINDOW)
            || color == GetSysColor(COLOR_BTNFACE)
            || color == GetSysColor(COLOR_HIGHLIGHT)
            || color == RGB(255, 255, 255))
        {
            color = RGB(0, 0, 0);
        }

        return orig(color);
    }

    static void EnableHooks(bool enable)
    {
        auto lock = g_lock.lock_exclusive();
        EnsureDispatcherModuleLocked();
        if (!g_dispatcher.moduleAddress)
            return;

        g_dispatcher.EnableHook(HK_DRAWTHEMEBACKGROUND, enable);
        g_dispatcher.EnableHook(HK_DRAWTHEMETEXT, enable);
        g_dispatcher.EnableHook(HK_DRAWTHEMETEXTEX, enable);
        g_dispatcher.EnableHook(HK_CREATESOLIDBRUSH, enable);
    }

    static void OnPaintBeginGlobal(HWND hWnd, HMODULE /*callerMod*/)
    {
        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) != GlassControlKind::MonthCal)
            return;

        ++g_paintRef;
        if (g_enableRef.fetch_add(1, std::memory_order_relaxed) == 0)
            EnableHooks(true);
    }

    static void OnPaintEndGlobal(HWND hWnd, HMODULE /*callerMod*/)
    {
        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) != GlassControlKind::MonthCal)
            return;

        if (g_enableRef.fetch_sub(1, std::memory_order_relaxed) == 1)
            EnableHooks(false);

        if (--g_paintRef < 0)
            g_paintRef = 0;
    }

    static void OnAnimBeginGlobal(HWND hWnd, HMODULE /*callerMod*/)
    {
        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) != GlassControlKind::MonthCal)
            return;

        ++g_animRef;
        if (g_enableRef.fetch_add(1, std::memory_order_relaxed) == 0)
            EnableHooks(true);

        DebugLog(L"[GlassCore][MonthCal] AnimBeginGlobal: hWnd=0x%p animRef=%d", hWnd, g_animRef);
    }

    static void OnAnimEndGlobal(HWND hWnd, HMODULE /*callerMod*/)
    {
        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) != GlassControlKind::MonthCal)
            return;

        if (g_enableRef.fetch_sub(1, std::memory_order_relaxed) == 1)
            EnableHooks(false);

        if (--g_animRef < 0)
            g_animRef = 0;

        DebugLog(L"[GlassCore][MonthCal] AnimEndGlobal: hWnd=0x%p animRef=%d", hWnd, g_animRef);
    }

    void OnMonthCalAttach(HWND /*hWndMonthCal*/, HWND hWndDropDown)
    {
        if (!hWndDropDown)
            return;

        const bool darkMode = (GetSysColor(COLOR_WINDOW) < 0xFFFFFF);

        GlassCore::Settings::WindowBackdropEffectContext backdrop{};
        GlassCore::Settings::BorderContext               border{};
        GlassCore::Settings::FlyoutAnimationContext      animation{};

        GlassCore::Settings::QueryBackdropEffectContext(L"DropDown", darkMode, backdrop);
        GlassCore::Settings::QueryBorderContext(L"DropDown", darkMode, border);
        GlassCore::Settings::QueryFlyoutAnimationContext(L"DropDown", animation);

        GlassCore::Settings::ApplyEffect(hWndDropDown, darkMode, backdrop, border);

        if (animation.enable)
        {
            GlassCore::Animation::CreateDropDownPopupIn(
                hWndDropDown,
                static_cast<float>(animation.startRatio) / 100.f,
                std::chrono::milliseconds{ animation.popInTime },
                std::chrono::milliseconds{ animation.fadeInTime },
                animation.popInStyle,
                animation.immediateInterupting);
        }
    }

    void OnMonthCalPrintClientBegin(HWND /*hWnd*/)
    {
        ++g_paintRef;
        if (g_enableRef.fetch_add(1, std::memory_order_relaxed) == 0)
            EnableHooks(true);
    }

    void OnMonthCalPrintClientEnd(HWND /*hWnd*/)
    {
        if (g_enableRef.fetch_sub(1, std::memory_order_relaxed) == 1)
            EnableHooks(false);

        if (--g_paintRef < 0)
            g_paintRef = 0;
    }

    void Prepare() {}

    void Startup()
    {
        {
            auto lock = g_lock.lock_exclusive();
            EnsureDispatcherModuleLocked();
        }

        GlassCore::PaintDispatcher::RegisterPaintProvider(
            { OnPaintBeginGlobal, OnPaintEndGlobal });

        GlassCore::PaintDispatcher::RegisterGetDCProvider(
            { OnAnimBeginGlobal, OnAnimEndGlobal });

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][MonthCal] Startup: Paint + GetDC providers registered");
    }

    void Shutdown()
    {
        g_enableRef.store(0, std::memory_order_seq_cst);

        g_paintRef = 0;
        g_animRef  = 0;

        auto lock = g_lock.lock_exclusive();
        g_dispatcher.DisableAllHooks();
        g_dispatcher.moduleAddress = nullptr;
        g_hookDataCached = false;
    }
}
