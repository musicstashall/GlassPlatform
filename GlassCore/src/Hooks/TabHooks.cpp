// Хуки для отрисовки SysTabControl32.
//
// Архитектура: PaintDispatcher (паттерн Button) — глобальные BeginPaint/EndPaint хуки.
// GlassMarkWindow помечает SysTabControl32, PaintDispatcher вызывает наши коллбэки.
//
// Опционально Prop «Glass.TabUseTheme»; без него — themed TAB (GlassMarkWindow).
// Подключение хуков как в BWM/PaintHandler (SysTabControl32 + comctl32):
//   (TCS_VERTICAL || !theme) → ExtTextOutW;
//   theme && !TCS_VERTICAL → DrawThemeText + DrawThemeTextEx (+ SetBKColor в BWM не дублируем здесь).
// Themed DrawThemeBackground: все части Tab — DrawThemeBackground с той же альфой, что язычки;
//   TABP_PANE + state0: после фона (в т.ч. при альфе 255 — чистый orig) — PatBlt(BLACKNESS) внутри
//   области страниц с отступом (полоса «стекла»). Язычки (TABP_TABITEM*), TABP_BODY и др. — без PatBlt.
// ExtTextOut: при themed+vertical — без PatBlt под текстом (DrawTextWithAlpha).
// GetSysColorBrush: comctl32 для части пути (вложенные табы host UI) берёт COLOR_BTNFACE/COLOR_WINDOW —
//   в themed-сессии подменяем на BLACK_BRUSH, иначе под полупрозрачными язычками проступает серая кисть.

#include "CorePch.h"

#include <Hooks/Tab.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/RenderText.h>
#include <Helpers/ThemeHelper.h>
#include <Helpers/TabChroma.h>
#include <Subsystems/ThemePaintRegistry.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <GlassControlKind.h>
#include <WinApiUtils.h>
#include <wil.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <mutex>
#include <vector>

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::TabControl
{
    namespace
    {
        const wchar_t kGlassTabUseThemeProp[] = L"Glass.TabUseTheme";

        [[nodiscard]] bool TabUseThemeFromProp(HWND hWnd) noexcept
        {
            // GlassMarkWindow(TabControl), без SetProp — по умолчанию themed TAB.
            const HANDLE h = GetPropW(hWnd, kGlassTabUseThemeProp);
            if (!h)
                return true;
            return h == reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(1u));
        }

        static bool StrEq(LPCWSTR a, LPCWSTR b) noexcept
        {
            return _wcsicmp(a, b) == 0;
        }

        // Отступ PatBlt от всех краёв TABP_PANE (включая верх у границы с полосой вкладок).
        constexpr int kTabPaneBlackInset = 1;

        // vsstyle.h TABPARTS
        constexpr int kTabPartTabItemFirst = 1;   // TABP_TABITEM
        constexpr int kTabPartTabItemLast  = 8;   // TABP_TOPTABITEMBOTHEDGE
        constexpr int kTabPartPane         = 9;   // TABP_PANE
        constexpr int kTabPartBody         = 10;  // TABP_BODY
        constexpr int kTabPartAeroWizardBody = 11; // TABP_AEROWIZARDBODY

        [[nodiscard]] bool IsTabTonguePartId(int partId) noexcept
        {
            return partId >= kTabPartTabItemFirst && partId <= kTabPartTabItemLast;
        }

        // Чёрное «окно» под контент — только клиентская панель TABP_PANE (не язычки, не TABP_BODY /
        // TABP_AEROWIZARDBODY). В vsstyle для TABP_PANE нет enum состояний; тема передаёт iStateId == 0.
        [[nodiscard]] bool ShouldPatBltTabClientPane(int partId, int stateId) noexcept
        {
            return partId == kTabPartPane && stateId == 0;
        }

        // vsstyle: TIS_HOT=2, TIS_SELECTED=3; хромакей заливки — для прочих состояний (normal/disabled/focused).
        constexpr int kTisHot      = 2;
        constexpr int kTisSelected = 3;

        // TIS: comctl рисует 1/4/5; прочие id (в т.ч. 0) — ключ как у NORMAL.
        [[nodiscard]] int Tis1BasedForChromaKey(int iStateIdFromComctl) noexcept
        {
            if (iStateIdFromComctl == 4)
                return 4; // TIS_DISABLED
            if (iStateIdFromComctl == 5)
                return 5; // TIS_FOCUSED
            return 1;     // TIS_NORMAL и остальное
        }

        // Ленивый сэмпл при пустом кэше (Root шлёт Refresh через GlassCore::OnRootWmThemeChangedForTabChroma).
        static std::mutex g_tabChromaLazyMutex;

        // true + outKey: есть реальный сэмпл. Нет сэмпла (и после lazy) — false, хрома не делаем.
        [[nodiscard]] bool TryGetTabChromaKeyForTis1Based(
            int tis1Based, COLORREF& outKey, UINT32& outSerial, HWND tabHw) noexcept
        {
            static thread_local UINT32 s_loggedSerial = 0xFFFFFFFFu;
            outSerial = 0;
            outKey    = 0;
            COLORREF  rgb{};

            const auto readSlot = [&](int tis) {
                return GlassCore::TabChroma::TryReadChromaForTisState(tis, rgb, &outSerial);
            };

            const auto logNewSerial = [&](int tis) {
                if (outSerial == s_loggedSerial)
                    return;
                s_loggedSerial = outSerial;
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][Tab] TabChroma: serial=%u tis=%d COLORREF=0x%06X R=%u G=%u B=%u",
                    outSerial,
                    tis,
                    (unsigned)(rgb & 0xFFFFFFu),
                    (unsigned)GetRValue(rgb),
                    (unsigned)GetGValue(rgb),
                    (unsigned)GetBValue(rgb));
            };

            if (readSlot(tis1Based))
            {
                logNewSerial(tis1Based);
                outKey = rgb & 0xFFFFFFu;
                return true;
            }
            if (tis1Based != 1 && readSlot(1))
            {
                outKey = rgb & 0xFFFFFFu;
                return true;
            }

            {
                std::lock_guard<std::mutex> lock(g_tabChromaLazyMutex);
                if (readSlot(tis1Based) || (tis1Based != 1 && readSlot(1)))
                {
                    logNewSerial(tis1Based);
                    outKey = rgb & 0xFFFFFFu;
                    return true;
                }
                (void)GlassCore::TabChroma::RefreshFromTheme(tabHw);
                if (readSlot(tis1Based) || (tis1Based != 1 && readSlot(1)))
                {
                    logNewSerial(tis1Based);
                    outKey = rgb & 0xFFFFFFu;
                    return true;
                }
                (void)GlassCore::TabChroma::RefreshFromTheme(nullptr);
                if (readSlot(tis1Based) || (tis1Based != 1 && readSlot(1)))
                {
                    logNewSerial(tis1Based);
                    outKey = rgb & 0xFFFFFFu;
                    return true;
                }
            }
            return false;
        }

    }

    // =========================================================================
    //  LIFO сессий paint — какие IAT на callerMod включены (BWM-логика по стилю/теме).
    // =========================================================================

    struct TabPaintSession
    {
        HMODULE mod;
        HWND hWnd;
        bool useTheme;
        bool hookExtTextOut;
        bool hookDrawThemeBg;
        bool hookDrawThemeText;
        bool hookGetSysColorBrush;
    };
    thread_local std::vector<TabPaintSession> g_tabStack;
    thread_local int  g_tabDrawThemeTextSessionDepth = 0;

    static std::atomic<bool> g_shutdown{ false };

    // =========================================================================
    //  OVERALL DISPATCHER MAP
    // =========================================================================

    BOOL WINAPI Overall_MyExtTextOutW(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
    int WINAPI Overall_MyDrawThemeBg(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
    HRESULT WINAPI Overall_MyDrawThemeText(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, DWORD, LPCRECT);
    HRESULT WINAPI Overall_MyDrawThemeTextEx(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, LPRECT, const DTTOPTS*);
    HBRUSH WINAPI Overall_MyGetSysColorBrush(int);

    enum OverallHookIndex : size_t
    {
        OHK_EXTTEXTOUT = 0,
        OHK_DRAWTHEMEBG,
        OHK_DRAWTHEMETEXT,
        OHK_DRAWTHEMETEXTEX,
        OHK_GETSYSCOLORBRUSH,
        OHK_COUNT
    };

    inline const HookDispatcherDependency<OHK_COUNT, 3> g_overallHookDep
    { {
        // OHK_EXTTEXTOUT
        std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "ExtTextOutW", reinterpret_cast<PVOID>(Overall_MyExtTextOutW) },
        // OHK_DRAWTHEMEBG
        std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
            "DrawThemeBackground", reinterpret_cast<PVOID>(Overall_MyDrawThemeBg) },
        // OHK_DRAWTHEMETEXT
        std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
            "DrawThemeText", reinterpret_cast<PVOID>(Overall_MyDrawThemeText) },
        // OHK_DRAWTHEMETEXTEX
        std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
            "DrawThemeTextEx", reinterpret_cast<PVOID>(Overall_MyDrawThemeTextEx) },
        // OHK_GETSYSCOLORBRUSH (импорт user32 в COMCTL32 и т.п.)
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
            "GetSysColorBrush", reinterpret_cast<PVOID>(Overall_MyGetSysColorBrush) },
    } };

    using OverallDispatcher = HookDispatcher<OHK_COUNT, 3>;

    static std::unordered_map<void*, OverallDispatcher> g_overallMap;
    static wil::srwlock g_overallLock;

    // ── EnableHooksOnModule ──────────────────────────────────────────────────
    static bool EnableHooksOnModule(void* baseAddress, size_t index, bool enable)
    {
        if (!baseAddress) return false;

        auto lock = g_overallLock.lock_exclusive();

        auto it = g_overallMap.find(baseAddress);
        if (it == g_overallMap.end())
        {
            auto [inserted, ok] = g_overallMap.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(baseAddress),
                std::forward_as_tuple(g_overallHookDep, baseAddress));
            it = inserted;
            it->second.CacheHookData();
        }
        else
        {
            it->second.moduleAddress = baseAddress;
        }

        it->second.EnableHook(index, enable);
        return true;
    }

    // ── DisableAllOverallHooks ───────────────────────────────────────────────
    static void DisableAllOverallHooks()
    {
        auto lock = g_overallLock.lock_exclusive();
        for (auto& [addr, dispatcher] : g_overallMap)
            dispatcher.DisableAllHooks();
        g_overallMap.clear();
    }

    // ── GetCallerModule ──────────────────────────────────────────────────────
    static HMODULE GetCallerModule(void* returnAddress)
    {
        HMODULE hMod = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(returnAddress),
            &hMod);
        return hMod;
    }

    // ── GetOverallOrigSafe ───────────────────────────────────────────────────
    template <size_t index, typename T>
    static T GetOverallOrigSafe(HMODULE callerModule)
    {
        auto lock = g_overallLock.lock_shared();
        auto it = g_overallMap.find(static_cast<void*>(callerModule));
        if (it != g_overallMap.end())
            return it->second.template GetOrg<index, T>();
        return nullptr;
    }

    // =========================================================================
    //  Hook: ExtTextOutW
    // =========================================================================
    // Очистка фона + alpha-текст для unthemed табов.

    BOOL WINAPI Overall_MyExtTextOutW(
        HDC hdc, int x, int y, UINT options, const RECT* lprect,
        LPCWSTR lpString, UINT c, const INT* lpDx)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_EXTTEXTOUT, decltype(&ExtTextOutW)>(callerMod);

        if (!orig)
            return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

        if (g_tabStack.empty())
            return orig(hdc, x, y, options, lprect, lpString, c, lpDx);

        const bool themedTabSession = g_tabStack.back().useTheme;

        // 1) Очистка фона (ETO_OPAQUE) — только для unthemed: под стекло чёрным.
        // Для themed не трогаем прямоугольник текста: иначе подпись вкладки на чёрном, не стыкуется с alpha-язычком.
        if (!themedTabSession && (options & ETO_OPAQUE) && lprect)
        {
            PatBlt(hdc, lprect->left, lprect->top,
                lprect->right - lprect->left, lprect->bottom - lprect->top,
                BLACKNESS);
        }

        // 2) Ранний выход: глифы, только фон, нет текста, нет rect
        if (options == ETO_GLYPH_INDEX || options == ETO_IGNORELANGUAGE
            || (options == ETO_OPAQUE && !c) || !c || !lprect)
        {
            // Стрипаем ETO_OPAQUE — фон уже очищен выше
            if (options != ETO_OPAQUE)
                options &= ~ETO_OPAQUE;

            // Если opaque-only или нет текста — обработано полностью
            if (options == ETO_OPAQUE || !c)
                return TRUE;

            // Иначе (ETO_GLYPH_INDEX и др.) — передаём оригиналу без ETO_OPAQUE
            return orig(hdc, x, y, options, lprect, lpString, c, lpDx);
        }

        // 3) Рендеринг текста с альфа-каналом
        HWND hWnd = g_tabStack.back().hWnd;
        DWORD dwTextFlags = GetTextAlign(hdc);
        RECT client{ x, y, lprect->right, lprect->bottom };

        if (hWnd)
        {
            UINT style = static_cast<UINT>(GetWindowLongW(hWnd, GWL_STYLE));
            if (style & TCS_VERTICAL)
            {
                SIZE psizl{};
                GetTextExtentPoint32W(hdc, lpString, c, &psizl);

                dwTextFlags |= DT_SINGLELINE;
                if (style & TCS_RIGHT)
                {
                    dwTextFlags |= DT_RIGHT | DT_TOP | DT_NOCLIP;
                    client.right = x + psizl.cx;
                    client.left = 0;
                }
                else
                {
                    dwTextFlags |= DT_LEFT | DT_BOTTOM | DT_NOCLIP;
                    client.bottom = y + psizl.cy;
                    client.top = 0;
                }
            }
        }

        int result = 0;
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpString, c, &client, dwTextFlags, result)))
            return result;

        return orig(hdc, x, y, options, lprect, lpString, c, lpDx);
    }

    // =========================================================================
    //  Hook: DrawThemeBackground (themed Tab — alpha-композиция)
    // =========================================================================

    int WINAPI Overall_MyDrawThemeBg(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, LPCRECT pClipRect)
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTHEMEBG, decltype(&DrawThemeBackground)>(callerMod);

        if (!orig)
            return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        if (g_tabStack.empty())
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        if (!g_tabStack.back().hookDrawThemeBg)
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        const HWND tabHw = g_tabStack.back().hWnd;

        WCHAR themeClass[MAX_PATH + 1]{};
        if (!SUCCEEDED(Shared::ThemeHelper::GetThemeClass(hTheme, themeClass, MAX_PATH))
            || !StrEq(themeClass, L"Tab")
            || !pRect || IsRectEmpty(pRect))
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        DebugLog(L"[GlassCore][TabHooks] Overall_MyDrawThemeBg: themeClass=\"%s\" partId=%d stateId=%d caller=0x%p mod=\"%s\"",
            themeClass, iPartId, iStateId, _ReturnAddress(), Shared::WinApiUtils::GetModuleFileBaseName(callerMod).c_str());
        if (IsTabTonguePartId(iPartId))
        {
            const BYTE tabAlpha = GlassCore::ThemePaint::GetEffectiveDrawThemeConstantAlphaForSession(
                GlassShared::GlassControlKind::TabControl,
                tabHw);
            const BYTE chromaTh = GlassCore::ThemePaint::GetEffectiveTabChromaThresholdForSession(
                GlassShared::GlassControlKind::TabControl,
                tabHw);
            const bool useInactiveChroma = (iStateId != kTisHot && iStateId != kTisSelected);
            if (useInactiveChroma)
            {
                const int  tisKey  = Tis1BasedForChromaKey(iStateId);
                COLORREF   cKey    = 0;
                UINT32     cacheSer = 0;
                const bool haveSample
                    = TryGetTabChromaKeyForTis1Based(tisKey, cKey, cacheSer, tabHw);
                // COLORREF==0 — не хромаем; нет сэмпла — только альфа (как hot/selected без вырезки заливки).
                const bool doChroma = haveSample && (cKey & 0xFFFFFFu) != 0u;
                if (doChroma)
                {
                    size_t keyedOut = 0;
                    if (Shared::WinApiUtils::DrawThemeBackgroundWithConstantAlphaAndChromaKey(
                            orig,
                            hTheme,
                            hdc,
                            iPartId,
                            iStateId,
                            pRect,
                            pClipRect,
                            cKey,
                            chromaTh,
                            tabAlpha,
                            4'000'000u,
                            &keyedOut))
                    {
                        if (keyedOut == 0u && pRect)
                        {
                            const int a = (pRect->right - pRect->left) * (pRect->bottom - pRect->top);
                            if (a > 32)
                            {
                                DebugLog(DBG_NORMAL,
                                    L"[GlassCore][Tab] chroma: stateId=%d tisKey=%d key=0x%06X th=%u keyed=0 area=%d serial=%u "
                                    L"(санити: внутри не срезалось; сузить th или [GlassShared][ThemeImageSample] сэмпл?)",
                                    iStateId,
                                    tisKey,
                                    (unsigned)(cKey & 0xFFFFFFu),
                                    (unsigned)chromaTh,
                                    a,
                                    cacheSer);
                            }
                        }
                        return S_OK;
                    }
                }
                if (Shared::WinApiUtils::DrawThemeBackgroundWithConstantAlpha(
                        orig,
                        hTheme,
                        hdc,
                        iPartId,
                        iStateId,
                        pRect,
                        pClipRect,
                        tabAlpha))
                {
                    return S_OK;
                }
                return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
            }
            if (Shared::WinApiUtils::DrawThemeBackgroundWithConstantAlpha(
                         orig,
                         hTheme,
                         hdc,
                         iPartId,
                         iStateId,
                         pRect,
                         pClipRect,
                         tabAlpha))
            {
                return S_OK;
            }
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }

        // Фон вокруг вкладок и прочие «тела» темы — только alpha (как язычки).
        if (iPartId == kTabPartBody || iPartId == kTabPartAeroWizardBody)
        {
            if (Shared::WinApiUtils::DrawThemeBackgroundWithConstantAlpha(
                    orig,
                    hTheme,
                    hdc,
                    iPartId,
                    iStateId,
                    pRect,
                    pClipRect,
                    GlassCore::ThemePaint::GetEffectiveDrawThemeConstantAlphaForSession(
                    GlassShared::GlassControlKind::TabControl,
                    tabHw)))
                return S_OK;
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }

        // Область страниц под вкладками (TABP_PANE): фон темы с альфой; PatBlt — только для «тела» панели.
        if (iPartId == kTabPartPane)
        {
            const bool patBltGlassHole = ShouldPatBltTabClientPane(iPartId, iStateId);
            const BYTE tabAlpha = GlassCore::ThemePaint::GetEffectiveDrawThemeConstantAlphaForSession(
                    GlassShared::GlassControlKind::TabControl,
                    tabHw);
            if (tabAlpha == 255)
            {
                const HRESULT hrPane = orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
                if (patBltGlassHole)
                {
                    const RECT* basis = pClipRect ? pClipRect : pRect;
                    RECT        inner = *basis;
                    inner.left += kTabPaneBlackInset;
                    inner.top += kTabPaneBlackInset;
                    inner.right -= kTabPaneBlackInset;
                    inner.bottom -= kTabPaneBlackInset;
                    if (inner.right > inner.left && inner.bottom > inner.top)
                    {
                        PatBlt(hdc,
                            inner.left,
                            inner.top,
                            inner.right - inner.left,
                            inner.bottom - inner.top,
                            BLACKNESS);
                    }
                }
                return static_cast<int>(hrPane);
            }

            if (!Shared::WinApiUtils::DrawThemeBackgroundWithConstantAlpha(
                    orig,
                    hTheme,
                    hdc,
                    iPartId,
                    iStateId,
                    pRect,
                    pClipRect,
                    GlassCore::ThemePaint::GetEffectiveDrawThemeConstantAlphaForSession(
                    GlassShared::GlassControlKind::TabControl,
                    tabHw)))
                return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            if (patBltGlassHole)
            {
                const RECT* basis = pClipRect ? pClipRect : pRect;
                RECT        inner = *basis;
                inner.left += kTabPaneBlackInset;
                inner.top += kTabPaneBlackInset;
                inner.right -= kTabPaneBlackInset;
                inner.bottom -= kTabPaneBlackInset;
                if (inner.right > inner.left && inner.bottom > inner.top)
                {
                    PatBlt(hdc,
                        inner.left,
                        inner.top,
                        inner.right - inner.left,
                        inner.bottom - inner.top,
                        BLACKNESS);
                }
            }
            return S_OK;
        }

        return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
    }

    // =========================================================================
    //  Hook: DrawThemeText / DrawThemeTextEx (горизонтальный themed Tab — BWM WinHooks)
    // =========================================================================

    HRESULT WINAPI Overall_MyDrawThemeText(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCWSTR pszText, int cchText, DWORD dwTextFlags,
        DWORD dwTextFlags2, LPCRECT pRect)
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return DrawThemeText(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTHEMETEXT, decltype(&DrawThemeText)>(callerMod);

        if (!orig)
            return DrawThemeText(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

        if (g_tabStack.empty())
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

        if (!g_tabStack.back().hookDrawThemeText)
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

        WCHAR themeClass[MAX_PATH + 1]{};
        if (!SUCCEEDED(Shared::ThemeHelper::GetThemeClass(hTheme, themeClass, MAX_PATH))
            || !StrEq(themeClass, L"Tab"))
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

        // cchText == -1 — длина по нулю-терминатору (как в DrawThemeText).
        if (!pszText || cchText == 0 || !pRect || IsRectEmpty(pRect))
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

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

    HRESULT WINAPI Overall_MyDrawThemeTextEx(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCWSTR pszText, int cchText, DWORD dwTextFlags,
        LPRECT pRect, const DTTOPTS* pOptions)
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return DrawThemeTextEx(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTHEMETEXTEX, decltype(&DrawThemeTextEx)>(callerMod);

        if (!orig)
            return DrawThemeTextEx(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        if (g_tabStack.empty())
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        if (!g_tabStack.back().hookDrawThemeText)
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        WCHAR themeClass[MAX_PATH + 1]{};
        if (!SUCCEEDED(Shared::ThemeHelper::GetThemeClass(hTheme, themeClass, MAX_PATH))
            || !StrEq(themeClass, L"Tab"))
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        if (!pRect || IsRectEmpty(pRect))
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        const DTTOPTS* effectiveOpts = pOptions;
        DTTOPTS defaultOpts{ sizeof(DTTOPTS) };
        if (!effectiveOpts)
            effectiveOpts = &defaultOpts;

        if (effectiveOpts->dwFlags & DTT_CALCRECT)
        {
            DTTOPTS measure = *effectiveOpts;
            if (measure.dwSize < sizeof(DTTOPTS))
                measure.dwSize = sizeof(DTTOPTS);
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, &measure);
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

    // =========================================================================
    //  Hook: GetSysColorBrush (themed Tab — убрать серую кисть под alpha-язычками)
    // =========================================================================

    HBRUSH WINAPI Overall_MyGetSysColorBrush(int nIndex)
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return GetSysColorBrush(nIndex);

        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_GETSYSCOLORBRUSH, decltype(&GetSysColorBrush)>(callerMod);

        if (!orig)
            return GetSysColorBrush(nIndex);

        if (g_tabStack.empty())
            return orig(nIndex);

        if (!g_tabStack.back().hookGetSysColorBrush)
            return orig(nIndex);

        if (nIndex == COLOR_BTNFACE || nIndex == COLOR_WINDOW)
            return reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));

        return orig(nIndex);
    }

    // =========================================================================
    //  EnableTabHooks — включить/выключить хуки на модуле
    // =========================================================================

    static void EnableTabHooks(void* mod, bool enable, const TabPaintSession& s)
    {
        if (!mod) return;
        if (s.hookExtTextOut)
            EnableHooksOnModule(mod, OHK_EXTTEXTOUT, enable);
        if (s.hookDrawThemeBg)
            EnableHooksOnModule(mod, OHK_DRAWTHEMEBG, enable);
        if (s.hookDrawThemeText)
        {
            EnableHooksOnModule(mod, OHK_DRAWTHEMETEXT, enable);
            EnableHooksOnModule(mod, OHK_DRAWTHEMETEXTEX, enable);
        }
        if (s.hookGetSysColorBrush)
            EnableHooksOnModule(mod, OHK_GETSYSCOLORBRUSH, enable);
    }

    // =========================================================================
    //  PaintDispatcher provider callbacks
    // =========================================================================

    // Вызывается из PaintDispatcher при BeginPaint.
    static void OnBeginPaintGlobal(HWND hWnd, HMODULE callerMod)
    {
        using namespace GlassShared;
        const auto kind = GlassGetWindowKind(hWnd);
        if (kind != GlassControlKind::TabControl) return;

        const bool useTheme = TabUseThemeFromProp(hWnd);
        const UINT style = static_cast<UINT>(GetWindowLongW(hWnd, GWL_STYLE));
        const bool vertical = (style & TCS_VERTICAL) != 0;
        // BWM PaintHandler: ExtTextOut при vertical || !theme; DrawThemeText при theme && !vertical.
        const TabPaintSession session{
            callerMod,
            hWnd,
            useTheme,
            vertical || !useTheme,
            useTheme,
            useTheme && !vertical,
            useTheme,
        };
        g_tabStack.push_back(session);
        if (session.hookDrawThemeText)
            ++g_tabDrawThemeTextSessionDepth;
        EnableTabHooks(callerMod, true, session);
        DebugLog(DBG_NORMAL,
            L"[GlassCore][Tab] BeginPaint: callerMod=0x%p hWnd=0x%p themed=%d vert=%d "
            L"hookETO=%d hookDTB=%d hookDTT=%d hookGSCB=%d depth=%zu",
            callerMod, hWnd, useTheme ? 1 : 0, vertical ? 1 : 0,
            session.hookExtTextOut ? 1 : 0,
            session.hookDrawThemeBg ? 1 : 0,
            session.hookDrawThemeText ? 1 : 0,
            session.hookGetSysColorBrush ? 1 : 0,
            g_tabStack.size());
    }

    // Вызывается из PaintDispatcher при EndPaint.
    static void OnEndPaintGlobal(HWND hWnd, HMODULE /*callerMod*/)
    {
        using namespace GlassShared;
        const auto kind = GlassGetWindowKind(hWnd);
        if (kind != GlassControlKind::TabControl) return;
        if (g_tabStack.empty()) return;

        const TabPaintSession top = g_tabStack.back();
        if (top.hookDrawThemeText)
            --g_tabDrawThemeTextSessionDepth;
        g_tabStack.pop_back();
        EnableTabHooks(top.mod, false, top);
        DebugLog(DBG_NORMAL, L"[GlassCore][Tab] EndPaint: hWnd=0x%p depth=%zu", hWnd, g_tabStack.size());
    }

    // =========================================================================
    //  Lifecycle
    // =========================================================================

    void Prepare()
    {
        // Ничего — GDI+ больше не нужен
    }

    void Startup()
    {
        // Регистрация провайдера в PaintDispatcher (до PaintDispatcher::Startup)
        GlassCore::PaintDispatcher::RegisterPaintProvider({ OnBeginPaintGlobal, OnEndPaintGlobal });

        // Кэшируем диспетчеры для известных модулей.
        auto cacheModule = [&](void* mod)
        {
            if (!mod) return;
            auto lock = g_overallLock.lock_exclusive();
            auto [it, inserted] = g_overallMap.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(mod),
                std::forward_as_tuple(g_overallHookDep, mod));
            if (inserted)
            {
                it->second.CacheHookData();
                DebugLog(DBG_NORMAL, L"[GlassCore][Tab] Startup: cached 0x%p", mod);
            }
        };
        cacheModule(GlassCore::ModuleHandler::g_comctl32New);
        cacheModule(GlassCore::ModuleHandler::g_comctl32Legacy);
        cacheModule(GlassCore::ModuleHandler::g_entryPointModule);
    }

    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_seq_cst);
        DisableAllOverallHooks();
        g_tabDrawThemeTextSessionDepth = 0;
        g_tabStack.clear();
        g_shutdown.store(false, std::memory_order_seq_cst);
    }

    void OnRootWmThemeChangedForTabChroma(HWND themeAnchorOrNull) noexcept
    {
        (void)GlassCore::TabChroma::RefreshFromTheme(themeAnchorOrNull);
    }
}
