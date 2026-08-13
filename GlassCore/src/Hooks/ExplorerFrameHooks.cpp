#include "CorePch.h"
#include <Hooks/ExplorerFrame.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Core/Intercept/HookHelper.h>
#include <Hooks/RootGdiHooks.h>
#include <Subsystems/RootSwcaGradient.h>
#include <Subsystems/Settings.h>
#include <RootBackdropPolicy.h>
#include <RootPolicyQuery.h>
#include <SystemHelper.h>
#include <DebugLog.h>
#include <uxtheme.h>
#include <Helpers/ThemeHelper.h>
#include <Subsystems/RootSwcaGradient.h>
#include <intrin.h>
#include <unordered_map>
#include <unordered_set>
#include <Core/Intercept/DebugDetourHooks.h>
using namespace GlassCore::Hooks;
using namespace std::literals;
namespace GlassCore::ExplorerFrameControl
{
    namespace Detail
    {
        enum OverallHookIndex : size_t
        {
            OHK_DWM_EXTEND_FRAME = 0,
            OHK_GET_THEME_COLOR,      // uxtheme.dll (caller filter: SHELL32.dll)
            OHK_EXT_TEXT_OUT_W,       // gdi32.dll   (IAT shlwapi)
            OHK_CREATE_WINDOW_EX,     // user32.dll  (раннее подклассирование explorer)
            OHK_DWM_SET_ATTR,         // dwmapi.dll  (backdrop для explorer root)
            OHK_REGISTER_CLASS_EX,    // user32.dll  (подмена кисти при регистрации класса)
            OHK_CREATE_SOLID_BRUSH,   // gdi32.dll   (IAT explorerframe — как DUI CreateSolidBrush)
            OHK_SET_BK_COLOR,         // gdi32.dll   (IAT shlwapi — на весь срок жизни модуля в процессе)
            OHK_COUNT
        };

        // Forward declarations
        HRESULT WINAPI Overall_MyDwmExtendFrameIntoClientArea(HWND hWnd, const MARGINS* pMarInset);
        HRESULT WINAPI Overall_MyGetThemeColor(HTHEME hTheme, int iPartId, int iStateId, int iPropId, COLORREF* pColor);
        BOOL    WINAPI Overall_MyExtTextOutW(HDC hdc, int x, int y, UINT options, const RECT* lprect, LPCWSTR lpString, UINT c, const INT* lpDx);
        HWND    WINAPI Overall_MyCreateWindowExW(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName, DWORD dwStyle, int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam);
        HRESULT WINAPI Overall_MyDwmSetWindowAttribute(HWND hwnd, DWORD dwAttribute, LPCVOID pvAttribute, DWORD cbAttribute);
        ATOM    WINAPI Overall_MyRegisterClassExW(const WNDCLASSEXW* lpWndClass);
        HBRUSH  WINAPI Overall_MyCreateSolidBrush(COLORREF color);
        COLORREF WINAPI Overall_MySetBkColor(HDC hdc, COLORREF color);

        inline const HookDispatcherDependency<OHK_COUNT, 4> g_overallHookDep
        { {
            std::tuple{ std::array{ "dwmapi.dll"sv, ""sv, ""sv, ""sv },
                        "DwmExtendFrameIntoClientArea",
                        reinterpret_cast<PVOID>(Overall_MyDwmExtendFrameIntoClientArea) },
            std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv, ""sv },
                        "GetThemeColor",
                        reinterpret_cast<PVOID>(Overall_MyGetThemeColor) },
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
                        "ExtTextOutW",
                        reinterpret_cast<PVOID>(Overall_MyExtTextOutW) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-window-l1-1-0.dll"sv, ""sv, ""sv },
                        "CreateWindowExW",
                        reinterpret_cast<PVOID>(Overall_MyCreateWindowExW) },
            std::tuple{ std::array{ "dwmapi.dll"sv, "ext-ms-win-dwmapi-ext-l1-1-0.dll"sv, ""sv, ""sv },
                        "DwmSetWindowAttribute",
                        reinterpret_cast<PVOID>(Overall_MyDwmSetWindowAttribute) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-window-l1-1-0.dll"sv, ""sv, ""sv },
                        "RegisterClassExW",
                        reinterpret_cast<PVOID>(Overall_MyRegisterClassExW) },
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
                        "CreateSolidBrush",
                        reinterpret_cast<PVOID>(Overall_MyCreateSolidBrush) },
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
                        "SetBkColor",
                        reinterpret_cast<PVOID>(Overall_MySetBkColor) }
        } };

        using OverallDispatcher = HookDispatcher<OHK_COUNT, 4>;

        static std::unordered_map<void*, OverallDispatcher> g_overallMap;
        static std::unordered_set<HWND>                     g_trackedExplorerRoots;
        static wil::srwlock                                 g_overallLock;
        static std::atomic<bool>                            g_shutdown{ false };
        static std::atomic<bool>                            g_bgHooksEnabled{ false };
        static std::atomic<bool>                            g_explorerChromeRestoring{ false };

        struct ClassBrushEntry
        {
            HBRUSH origBrush;
            LONG_PTR origClassStyle;
        };
        static std::unordered_map<ATOM, ClassBrushEntry>    g_explorerClassBrushes;

        constexpr wchar_t kExplorerIATExtendProp[] = L"GlassPlatform.ExplorerIATExtend";
        constexpr wchar_t kExplorerStockTopMarginProp[] = L"GlassPlatform.ExplorerStockTopMargin";

        void ClearExplorerStockTopMarginMarker(HWND hWnd) noexcept
        {
            if (!hWnd)
                return;
            RemovePropW(hWnd, kExplorerStockTopMarginProp);
        }

        void CaptureExplorerStockTopMarginFromOrig(HWND hWnd, const MARGINS* pMarInset) noexcept
        {
            if (!hWnd || !pMarInset || pMarInset->cyTopHeight <= 0)
                return;
            SetPropW(
                hWnd,
                kExplorerStockTopMarginProp,
                reinterpret_cast<HANDLE>(static_cast<intptr_t>(pMarInset->cyTopHeight)));
        }

    static void RestoreExplorerShellChromeForTrackedRoots(
        bool clearTrackedRoots,
        bool restoreClassBrush) noexcept
        {
            auto lock = g_overallLock.lock_exclusive();

            for (HWND h : g_trackedExplorerRoots)
            {
                if (!h || !IsWindow(h))
                    continue;

                RemovePropW(h, kExplorerIATExtendProp);
                if (restoreClassBrush)
                    ClearExplorerStockTopMarginMarker(h);
                const ATOM classAtom = static_cast<ATOM>(GetClassLongPtrW(h, GCW_ATOM));
                auto it = g_explorerClassBrushes.find(classAtom);
                if (restoreClassBrush && it != g_explorerClassBrushes.end())
                {
                    SetClassLongPtrW(h, GCLP_HBRBACKGROUND,
                        reinterpret_cast<LONG_PTR>(it->second.origBrush));
                    SetClassLongPtrW(h, GCL_STYLE, it->second.origClassStyle);
                    g_explorerClassBrushes.erase(it);
                }

                if (restoreClassBrush)
                {
                    RedrawWindow(h, nullptr, nullptr,
                        RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME | RDW_UPDATENOW);
                }
            }

            if (clearTrackedRoots)
                g_trackedExplorerRoots.clear();

            if (restoreClassBrush)
                g_explorerClassBrushes.clear();
        }

        [[nodiscard]] static bool IsExplorerProcess() noexcept
        {
            static const bool s_result = (GetModuleHandleW(L"explorer.exe") != nullptr);
            return s_result;
        }

        [[nodiscard]] static bool EnableHooksOnModule(void* baseAddress, size_t index, bool enable) noexcept
        {
            if (!baseAddress)
                return false;
            auto lock = g_overallLock.lock_exclusive();
            auto it = g_overallMap.find(baseAddress);
            if (it == g_overallMap.end())
            {
                const auto emplaced = g_overallMap.emplace(
                    std::piecewise_construct,
                    std::forward_as_tuple(baseAddress),
                    std::forward_as_tuple(g_overallHookDep, baseAddress));
                it = emplaced.first;
                if (emplaced.second)
                    it->second.CacheHookData();
            }
            else
            {
                it->second.moduleAddress = baseAddress;
            }
            const bool ok = it->second.EnableHook(index, enable);
            if (!ok)
                DebugLog(DBG_WARNING,
                    L"[GlassCore][ExplorerFrame] EnableHooksOnModule FAILED: index=%zu module=0x%p (IAT entry not found)",
                    index, baseAddress);
            return ok;
        }

        static void DisableAllOverallHooks() noexcept
        {
            auto lock = g_overallLock.lock_exclusive();
            for (auto& [addr, dispatcher] : g_overallMap)
                dispatcher.DisableAllHooks();
            g_overallMap.clear();
        }

        [[nodiscard]] static HMODULE GetCallerModule(void* returnAddress) noexcept
        {
            HMODULE hMod = nullptr;
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(returnAddress),
                &hMod);
            return hMod;
        }

        template <size_t index, typename T>
        [[nodiscard]] static T GetOverallOrigSafe(HMODULE callerModule) noexcept
        {
            auto lock = g_overallLock.lock_shared();
            auto it = g_overallMap.find(static_cast<void*>(callerModule));
            if (it != g_overallMap.end())
                return it->second.template GetOrg<index, T>();
            return nullptr;
        }

        static void GetModuleFileBaseName(HMODULE mod, wchar_t* buf, size_t bufChars) noexcept
        {
            if (!mod)
            {
                wcscpy_s(buf, bufChars, L"?");
                return;
            }
            wchar_t path[MAX_PATH + 1]{};
            if (!GetModuleFileNameW(mod, path, MAX_PATH))
            {
                wcscpy_s(buf, bufChars, L"?");
                return;
            }
            const wchar_t* slash = wcsrchr(path, L'\\');
            wcscpy_s(buf, bufChars, slash ? slash + 1 : path);
        }

        static void FormatRectShort(wchar_t* buf, size_t bufChars, LPCRECT rc) noexcept
        {
            if (!rc)
            {
                wcscpy_s(buf, bufChars, L"-");
                return;
            }
            swprintf_s(
                buf,
                bufChars,
                L"%d,%d,%d,%d",
                rc->left,
                rc->top,
                rc->right,
                rc->bottom);
        }

        static void FormatHdcWindowSuffix(HDC hdc, wchar_t* buf, size_t bufChars) noexcept
        {
            if (!hdc)
            {
                wcscpy_s(buf, bufChars, L"hdcWin=-");
                return;
            }
            HWND const w = WindowFromDC(hdc);
            if (!w)
            {
                wcscpy_s(buf, bufChars, L"hdcWin=-");
                return;
            }
            wchar_t cls[96]{};
            if (!GetClassNameW(w, cls, static_cast<int>(std::size(cls))))
                wcscpy_s(cls, L"?");
            swprintf_s(buf, bufChars, L"hdcWin=%p cls=\"%s\"", w, cls);
        }

        [[nodiscard]] static bool ShouldEnableExplorerBgHooks() noexcept
        {
            if (!GlassCore::Root::IsFullInjectionMode())
                return false;
            return Shared::RootPolicyQuery::IsRootGlassRenderingEnabled();
        }

        [[nodiscard]] bool ShouldPassthroughExplorerFrameHooks(HWND hWnd) noexcept
        {
            if (!hWnd || !IsWindow(hWnd))
                return true;
            if (!Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hWnd))
                return true;
            if (g_explorerChromeRestoring.load(std::memory_order_acquire))
                return true;
            if (!g_bgHooksEnabled.load(std::memory_order_acquire))
                return true;
            if (!Shared::RootPolicyQuery::IsRootGlassRenderingEnabled())
                return true;
            return false;
        }

        [[nodiscard]] bool ShouldForceFullFrameMarginsForExplorerShellRoot(HWND hWnd) noexcept
        {
            if (Shared::SystemHelper::g_buildNumber < 22000)
                return false;
            if (ShouldPassthroughExplorerFrameHooks(hWnd))
                return false;
            return true;
        }

        [[nodiscard]] static bool ExplorerShellRootIsVisuallyActive(HWND hRoot) noexcept
        {
            const HWND fg = GetForegroundWindow();
            if (!fg)
                return false;
            const HWND rWnd = GetAncestor(hRoot, GA_ROOT);
            const HWND rFg = GetAncestor(fg, GA_ROOT);
            return rWnd && rWnd == rFg;
        }

        [[nodiscard]] static bool ShouldLogExplorerFrameHotPath() noexcept
        {
            static thread_local DWORD s_lastTick = 0;
            const DWORD now = GetTickCount();
            if (now - s_lastTick < 500u)
                return false;
            s_lastTick = now;
            return true;
        }

        HRESULT WINAPI Overall_MyDwmExtendFrameIntoClientArea(HWND hWnd, const MARGINS* pMarInset)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_DWM_EXTEND_FRAME, decltype(&DwmExtendFrameIntoClientArea)>(callerMod);
            if (!orig)
                return DwmExtendFrameIntoClientArea(hWnd, pMarInset);
            if (!ShouldForceFullFrameMarginsForExplorerShellRoot(hWnd))
                return orig(hWnd, pMarInset);
            CaptureExplorerStockTopMarginFromOrig(hWnd, pMarInset);
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][ExplorerFrame] DwmExtendFrameIntoClientArea: Forcing full frame margins for explorer shell root hwnd=0x%p",
                hWnd);
            constexpr MARGINS kForceFullFrame{ -1, -1, -1, -1 };
            // Маркер IAT extend; полный SWCA — ExplorerRoot::Attach (после CreateWindowExW), не здесь:
            // ранний apply внутри create заполнял кэш и блокировал ATTACH при открытом LogViewer.
            SetPropW(hWnd, kExplorerIATExtendProp, reinterpret_cast<HANDLE>(1));
            return orig(hWnd, &kForceFullFrame);
        }

        // ── GetThemeColor ──────────────────────────────────────────────────────
        HRESULT WINAPI Overall_MyGetThemeColor(
            HTHEME hTheme, int iPartId, int iStateId, int iPropId, COLORREF* pColor)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_GET_THEME_COLOR, decltype(&GetThemeColor)>(callerMod);
            if (!orig || !pColor)
                return GetThemeColor(hTheme, iPartId, iStateId, iPropId, pColor);

            WCHAR themeClassEnter[MAX_PATH + 1]{};
            const HRESULT hrClassEnter = Shared::ThemeHelper::GetThemeClass(hTheme, themeClassEnter, MAX_PATH);
            const wchar_t* const clsEnter = SUCCEEDED(hrClassEnter) ? themeClassEnter : L"(GetThemeClass failed)";

            DebugLog(L"[GlassCore][ExplorerFrame] GetThemeColor ENTER: themeClass=\"%s\" iPartId=%d iStateId=%d iPropId=%d caller=0x%p bgEnabled=%d",
                clsEnter, iPartId, iStateId, iPropId, callerMod,
                (int)g_bgHooksEnabled.load(std::memory_order_relaxed));

            if (!g_bgHooksEnabled.load(std::memory_order_acquire))
                return orig(hTheme, iPartId, iStateId, iPropId, pColor);

            if (callerMod != GetModuleHandleW(L"SHELL32.dll") && callerMod != GetModuleHandleW(L"DUI70.dll"))
                return orig(hTheme, iPartId, iStateId, iPropId, pColor);

            HRESULT hr = orig(hTheme, iPartId, iStateId, iPropId, pColor);

            if (SUCCEEDED(hr) && iPropId == TMT_FILLCOLOR)
            {
                WCHAR themeClass[MAX_PATH + 1]{};
                const HRESULT hrThemeClass = Shared::ThemeHelper::GetThemeClass(hTheme, themeClass, MAX_PATH);
                if (FAILED(hrThemeClass))
                    return hr;
                DebugLog(L"[GlassCore][ExplorerFrame] GetThemeColor: themeClass=\"%s\" iPartId=%d iStateId=%d iPropId=%d originalColor=0x%08X",
					themeClass, iPartId, iStateId, iPropId, *pColor);
                bool shouldOverride = false;

                // Условия из ТЗ (сравнение через wcscmp, как принято для WCHAR-буферов)
                if ((wcscmp(themeClass, L"ItemsView") == 0 || wcscmp(themeClass, L"ExplorerStatusBar") == 0 || wcscmp(themeClass, L"ExplorerNavPane") == 0)
                    && iPartId == 0 && iStateId == 0)
                {
                    shouldOverride = true;
                }
                else if (wcscmp(themeClass, L"ReadingPane") == 0 && iPartId == 1 && iStateId == 0)
                {
                    shouldOverride = true;
                }
                else if (wcscmp(themeClass, L"ProperTree") == 0 && iPartId == 2 && iStateId == 0)
                {
                    shouldOverride = true;
                }
                else if (wcscmp(themeClass, L"CommandModule") == 0 || wcscmp(themeClass, L"Header") == 0 || wcscmp(themeClass, L"TryHarder") == 0 ||
                    wcscmp(themeClass, L"Edit") == 0 || wcscmp(themeClass, L"Tooltip") == 0 || wcscmp(themeClass, L"ReadingPane") == 0 ||
                    wcscmp(themeClass, L"PreviewPane") == 0 || wcscmp(themeClass, L"MediaStyle") == 0 || wcscmp(themeClass, L"ListViewStyle") == 0 ||
                    wcscmp(themeClass, L"ControlPanelStyle") == 0 || wcscmp(themeClass, L"InfoBar") == 0 ||
                    wcscmp(themeClass, L"FileExplorerBannerContainer") == 0 || wcscmp(themeClass, L"ExplorerNavPane") == 0)
                {
                    shouldOverride = true;
                }

                if (shouldOverride)
                    *pColor = RGB(0, 0, 0);
            }
            return hr;
        }

        // ── ExtTextOutW ────────────────────────────────────────────────────────
        BOOL WINAPI Overall_MyExtTextOutW(
            HDC hdc, int x, int y, UINT options, const RECT* lprect,
            LPCWSTR lpString, UINT c, const INT* lpDx)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_EXT_TEXT_OUT_W, decltype(&ExtTextOutW)>(callerMod);
            if (!orig)
                return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

            if (!g_bgHooksEnabled.load(std::memory_order_acquire))
                return orig(hdc, x, y, options, lprect, lpString, c, lpDx);

            if (callerMod != GetModuleHandleW(L"shlwapi.dll"))
                return orig(hdc, x, y, options, lprect, lpString, c, lpDx);

            if (options == ETO_OPAQUE && lprect != nullptr && c == 0)
            {
                if (ShouldLogExplorerFrameHotPath())
                {
                    DebugLog(DBG_IMPORTANT,
                        L"[GlassCore][ExplorerFrame] ExtTextOutW BLACKNESS: left=%d top=%d right=%d bottom=%d",
                        lprect->left, lprect->top, lprect->right, lprect->bottom);
                }
                PatBlt(hdc, lprect->left, lprect->top,
                    lprect->right - lprect->left, lprect->bottom - lprect->top, BLACKNESS);
            }

            return orig(hdc, x, y, options, lprect, lpString, c, lpDx);
        }

        // ── CreateWindowExW — раннее подклассирование окон проводника ──────────
        HWND WINAPI Overall_MyCreateWindowExW(
            DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName,
            DWORD dwStyle, int X, int Y, int nWidth, int nHeight,
            HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_CREATE_WINDOW_EX, decltype(&CreateWindowExW)>(callerMod);
            if (!orig)
                return CreateWindowExW(dwExStyle, lpClassName, lpWindowName, dwStyle,
                    X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);

            HWND hWnd = orig(dwExStyle, lpClassName, lpWindowName, dwStyle,
                X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);
            if (!hWnd)
                return hWnd;
            if (g_shutdown.load(std::memory_order_acquire))
                return hWnd;

            wchar_t className[256]{};
            if (IS_INTRESOURCE(lpClassName))
            {
                if (GetClassNameW(hWnd, className, static_cast<int>(std::size(className))) <= 0)
                    return hWnd;
            }
            else
            {
                wcsncpy_s(className, lpClassName, _TRUNCATE);
            }

            if (_wcsicmp(className, L"CabinetWClass") == 0 || _wcsicmp(className, L"ExploreWClass") == 0)
            {
                // Fallback: подмена кисти через SetClassLongPtrW, если RegisterClassExW
                // не перехватил (класс зарегистрирован до установки хука).
                if (g_bgHooksEnabled.load(std::memory_order_acquire))
                {
                    const ATOM classAtom = static_cast<ATOM>(GetClassLongPtrW(hWnd, GCW_ATOM));
                    if (classAtom)
                    {
                        auto lock = g_overallLock.lock_shared();
                        const bool alreadyPatched = g_explorerClassBrushes.contains(classAtom);
                        lock.reset();

                        if (!alreadyPatched)
                        {
                            const LONG_PTR prevStyle = GetClassLongPtrW(hWnd, GCL_STYLE);
                            auto prevBrush = reinterpret_cast<HBRUSH>(
                                SetClassLongPtrW(hWnd, GCLP_HBRBACKGROUND,
                                    reinterpret_cast<LONG_PTR>(GetStockObject(BLACK_BRUSH))));
                            SetClassLongPtrW(hWnd, GCL_STYLE, prevStyle | CS_HREDRAW | CS_VREDRAW);

                            auto exLock = g_overallLock.lock_exclusive();
                            g_explorerClassBrushes.try_emplace(classAtom,
                                ClassBrushEntry{ prevBrush, prevStyle });
                        }
                    }
                }

                using EarlyAttachFn = void(WINAPI*)(HWND);
                static EarlyAttachFn s_earlyAttach = nullptr;
                if (!s_earlyAttach)
                {
                    HMODULE hWndCompat = GetModuleHandleW(L"WndCompat.dll");
                    if (hWndCompat)
                        s_earlyAttach = reinterpret_cast<EarlyAttachFn>(
                            GetProcAddress(hWndCompat, "WndCompat_EarlyExplorerRootAttach"));
                }
                if (s_earlyAttach)
                {
                    DebugLog(DBG_IMPORTANT,
                        L"[GlassCore][ExplorerFrame] CreateWindowExW early attach: hwnd=0x%p class=\"%s\"",
                        hWnd, className);
                    s_earlyAttach(hWnd);
                }
            }

            return hWnd;
        }

        // ── RegisterClassExW — подмена кисти при регистрации класса ──────────
        ATOM WINAPI Overall_MyRegisterClassExW(const WNDCLASSEXW* lpWndClass)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_REGISTER_CLASS_EX, decltype(&RegisterClassExW)>(callerMod);
            if (!orig)
                return RegisterClassExW(lpWndClass);

            if (!lpWndClass || !lpWndClass->lpszClassName)
                return orig(lpWndClass);

            if (!g_bgHooksEnabled.load(std::memory_order_acquire))
                return orig(lpWndClass);

            if (_wcsicmp(lpWndClass->lpszClassName, L"CabinetWClass") != 0
                && _wcsicmp(lpWndClass->lpszClassName, L"ExploreWClass") != 0)
                return orig(lpWndClass);

            WNDCLASSEXW modified = *lpWndClass;
            const HBRUSH origBrush = modified.hbrBackground;
            const DWORD  origStyle = modified.style;
            modified.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            modified.style |= CS_HREDRAW | CS_VREDRAW;

            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][ExplorerFrame] RegisterClassExW: подмена кисти для \"%s\"",
                lpWndClass->lpszClassName);

            const ATOM atom = orig(&modified);
            if (atom)
            {
                auto lock = g_overallLock.lock_exclusive();
                g_explorerClassBrushes.try_emplace(atom,
                    ClassBrushEntry{ origBrush, static_cast<LONG_PTR>(origStyle) });
            }
            return atom;
        }

        // ── CreateSolidBrush — IAT explorerframe (BWM/DirectUI-паттерн фона нав. панели / дерева)
        HBRUSH WINAPI Overall_MyCreateSolidBrush(COLORREF color)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_CREATE_SOLID_BRUSH, decltype(&CreateSolidBrush)>(callerMod);
            if (!orig)
                return CreateSolidBrush(color);

            DebugLog(L"[GlassCore][ExplorerFrame] CreateSolidBrush ENTER: color=0x%08X caller=0x%p bgEnabled=%d shutdown=%d",
                color, callerMod,
                (int)g_bgHooksEnabled.load(std::memory_order_relaxed),
                (int)g_shutdown.load(std::memory_order_relaxed));

            if (g_shutdown.load(std::memory_order_acquire))
                return orig(color);
            if (!g_bgHooksEnabled.load(std::memory_order_acquire))
                return orig(color);

            const COLORREF colorIn = color;
            if (color == GetSysColor(COLOR_WINDOW)
                || color == GetSysColor(COLOR_BTNFACE)
                || Shared::ThemeHelper::IsThemeColor(color, L"ExplorerNavPane")
                || Shared::ThemeHelper::IsThemeColor(color, L"ProperTree"))
            {
                DebugLog(L"[GlassCore][ExplorerFrame] CreateSolidBrush: patch 0x%08X → black", colorIn);
                color = RGB(0, 0, 0);
            }
            return orig(color);
        }

        COLORREF WINAPI Overall_MySetBkColor(HDC hdc, COLORREF color)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_SET_BK_COLOR, decltype(&SetBkColor)>(callerMod);
            if (!orig)
            {
                DebugLog(DBG_WARNING,
                    L"[GlassCore][ExplorerFrame] SetBkColor: нет orig (IAT не найден для caller) hdc=0x%p color=0x%08X caller=0x%p",
                    hdc, color, callerMod);
                return SetBkColor(hdc, color);
            }

            if (g_shutdown.load(std::memory_order_acquire))
                return orig(hdc, color);
            if (!g_bgHooksEnabled.load(std::memory_order_acquire))
                return orig(hdc, color);

            // IAT SetBk на shlwapi: перехват на весь срок жизни модуля; подмена только при включённом фоне проводника.
            return orig(hdc, RGB(0, 0, 0));
        }

        // ── DwmSetWindowAttribute — backdrop для explorer root ────────────────
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

        [[nodiscard]] static DWORD MapEffectTypeToDwmBackdrop(DWORD effectType) noexcept
        {
            switch (effectType)
            {
            case 6:  return static_cast<DWORD>(DWMSBT_TRANSIENTWINDOW);  // Acrylic
            case 7:  return static_cast<DWORD>(DWMSBT_MAINWINDOW);       // Mica
            case 8:  return static_cast<DWORD>(4);                        // MicaAlt (DWMSBT_TABBEDWINDOW)
            default: return static_cast<DWORD>(DWMSBT_TRANSIENTWINDOW);  // SWCA 1-5 → заглушка
            }
        }

        [[nodiscard]] static DWORD ReadDwmAttrDword(LPCVOID pv, DWORD cb) noexcept
        {
            if (!pv || cb < sizeof(DWORD))
                return 0xFFFFFFFFu;
            return *static_cast<const DWORD*>(pv);
        }

        static void LogExplorerDwmAttrValue(
            DWORD dwAttribute,
            LPCVOID pvAttribute,
            DWORD cbAttribute,
            const wchar_t* tag) noexcept
        {
            if (!tag)
                return;

            switch (dwAttribute)
            {
            case DWMWA_SYSTEMBACKDROP_TYPE:
                DebugLog(
                    L"[GlassCore][ExplorerFrame][DWM] %s attr=SYSTEMBACKDROP_TYPE value=%u cb=%u",
                    tag,
                    ReadDwmAttrDword(pvAttribute, cbAttribute),
                    cbAttribute);
                break;
            case DWMWA_CAPTION_COLOR:
            {
                const DWORD v = ReadDwmAttrDword(pvAttribute, cbAttribute);
                DebugLog(
                    L"[GlassCore][ExplorerFrame][DWM] %s attr=CAPTION_COLOR value=0x%08X cb=%u",
                    tag,
                    v,
                    cbAttribute);
                break;
            }
            case DWMWA_BORDER_COLOR:
            {
                const DWORD v = ReadDwmAttrDword(pvAttribute, cbAttribute);
                DebugLog(
                    L"[GlassCore][ExplorerFrame][DWM] %s attr=BORDER_COLOR value=0x%08X cb=%u",
                    tag,
                    v,
                    cbAttribute);
                break;
            }
            default:
                DebugLog(
                    L"[GlassCore][ExplorerFrame][DWM] %s attr=%u cb=%u",
                    tag,
                    dwAttribute,
                    cbAttribute);
                break;
            }
        }

        HRESULT WINAPI Overall_MyDwmSetWindowAttribute(
            HWND hwnd, DWORD dwAttribute, LPCVOID pvAttribute, DWORD cbAttribute)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_DWM_SET_ATTR, decltype(&DwmSetWindowAttribute)>(callerMod);
            if (!orig)
                return DwmSetWindowAttribute(hwnd, dwAttribute, pvAttribute, cbAttribute);

            DebugLog(L"[GlassCore][ExplorerFrame] DwmSetWindowAttribute ENTER: hwnd=0x%p attr=%u caller=0x%p bgEnabled=%d RootBackdropMode=%u",
                hwnd, dwAttribute, callerMod, (int)g_bgHooksEnabled.load(std::memory_order_relaxed),
                Shared::RootPolicyQuery::ReadRootBackdropMode());
            LogExplorerDwmAttrValue(dwAttribute, pvAttribute, cbAttribute, L"sys-in");

            if (ShouldPassthroughExplorerFrameHooks(hwnd))
            {
                const HRESULT hrPassthrough = orig(hwnd, dwAttribute, pvAttribute, cbAttribute);
                DebugLog(
                    L"[GlassCore][ExplorerFrame][DWM] passthrough (restore/OFF) hr=0x%08X attr=%u",
                    hrPassthrough,
                    dwAttribute);
                return hrPassthrough;
            }

            // RootBackdropMode: DWM (1) — path 1+2 ниже (подмена + orig, анимация active/inactive).
            // SWCA (2) — block orig на SYSTEMBACKDROP_TYPE; композицию задаёт ApplyRootSwcaBackdrop.
            if (Shared::RootPolicyQuery::ReadRootBackdropMode() == 2u
                && Shared::SystemHelper::g_buildNumber >= 22000
                && Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hwnd))
            {
                if (dwAttribute == DWMWA_SYSTEMBACKDROP_TYPE)
                {
                    DebugLog(
                        L"[GlassCore][ExplorerFrame][DWM] SWCA block SYSTEMBACKDROP_TYPE hwnd=0x%p sys=%u",
                        hwnd,
                        ReadDwmAttrDword(pvAttribute, cbAttribute));
                    return S_OK;
                }

                if (dwAttribute == DWMWA_CAPTION_COLOR)
                {
                    const bool darkMode = (GetSysColor(COLOR_WINDOW) < 0xFFFFFF);
                    const bool visuallyActive = ExplorerShellRootIsVisuallyActive(hwnd);
                    GlassCore::Settings::WindowBackdropEffectContext swcaCtx{};
                    GlassCore::Settings::QueryRootBackdropEffectContext(
                        darkMode, visuallyActive, swcaCtx);

                    // ET 1–5: без orig(CAPTION_COLOR) и без light restore — полный SWCA reapply.
                    if (GlassCore::Settings::IsRootSwcaEngineEffectType(swcaCtx.effectType))
                    {
                        DebugLog(
                            L"[GlassCore][ExplorerFrame][DWM] SWCA block CAPTION_COLOR reason=swca_et "
                            L"hwnd=0x%p effType=%u active=%u",
                            hwnd,
                            swcaCtx.effectType,
                            visuallyActive ? 1u : 0u);
                        GlassCore::Root::RemoveRootSwcaApplyCacheEntry(hwnd);
                        GlassCore::Root::SetNextRootSwcaApplyReason(L"CAPTION_COLOR_SWCA_ET");
                        if (visuallyActive)
                            GlassCore::Root::OnRootSwcaBackdropActiveApply(hwnd);
                        else
                            GlassCore::Root::OnRootSwcaBackdropInactiveApply(hwnd);
                        return S_OK;
                    }

                    // ET 6–8 при mode=2: без изменений (orig + light restore).
                    LogExplorerDwmAttrValue(dwAttribute, pvAttribute, cbAttribute, L"SWCA orig-in");
                    const HRESULT hr = orig(hwnd, dwAttribute, pvAttribute, cbAttribute);
                    DebugLog(
                        L"[GlassCore][ExplorerFrame][DWM] SWCA orig CAPTION_COLOR hr=0x%08X active=%u effType=%u",
                        hr,
                        visuallyActive ? 1u : 0u,
                        swcaCtx.effectType);
                    if (SUCCEEDED(hr))
                    {
                        GlassCore::Root::RemoveRootSwcaApplyCacheEntry(hwnd);
                        if (visuallyActive)
                            GlassCore::Root::ApplyRootSwcaCaptionColorTransition(hwnd, true);
                        else
                            GlassCore::Root::ApplyRootSwcaCaptionColorTransition(hwnd, false);
                    }
                    return hr;
                }
            }

            const bool darkMode = (GetSysColor(COLOR_WINDOW) < 0xFFFFFF);
            GlassCore::Settings::WindowBackdropEffectContext ctx{};
            GlassCore::Settings::QueryRootBackdropEffectContext(
                darkMode, ExplorerShellRootIsVisuallyActive(hwnd), ctx);
            const DWORD mappedBackdrop = MapEffectTypeToDwmBackdrop(ctx.effectType);
            const bool skipCaptionPath2 = ctx.effectType == 1u
                || ctx.effectType == 6u
                || mappedBackdrop == static_cast<DWORD>(DWMSBT_MAINWINDOW);
            const DWORD sysBackdrop = ReadDwmAttrDword(pvAttribute, cbAttribute);
            DebugLog(
                L"[GlassCore][ExplorerFrame][DWM] DWM mode: hwnd=0x%p attr=%u sysBackdrop=%u mapped=%u "
                L"effectType=%u skipPath2=%u active=%u",
                hwnd,
                dwAttribute,
                sysBackdrop,
                mappedBackdrop,
                ctx.effectType,
                skipCaptionPath2 ? 1u : 0u,
                ExplorerShellRootIsVisuallyActive(hwnd) ? 1u : 0u);
            // Путь 1: подмена DWMWA_SYSTEMBACKDROP_TYPE ДО вызова оригинала.
            LPCVOID pvSend = pvAttribute;
            DWORD localBackdrop{};
            if (dwAttribute == DWMWA_SYSTEMBACKDROP_TYPE
                && pvAttribute && cbAttribute >= sizeof(DWORD))
            {
                localBackdrop = (ctx.effectType == 1u)
                    ? static_cast<DWORD>(DWMSBT_NONE)
                    : mappedBackdrop;
                pvSend = &localBackdrop;
                DebugLog(
                    L"[GlassCore][ExplorerFrame][DWM] path1 glass SBT=%u (sys was %u effectType=%u)",
                    localBackdrop,
                    sysBackdrop,
                    ctx.effectType);
            }
            HRESULT hr = orig(hwnd, dwAttribute, pvSend, cbAttribute);
            DebugLog(
                L"[GlassCore][ExplorerFrame][DWM] orig hr=0x%08X attr=%u",
                hr,
                dwAttribute);

            // Путь 2: при смене фокуса explorerframe отправляет DWMWA_CAPTION_COLOR,
            // но не повторяет DWMWA_SYSTEMBACKDROP_TYPE. Для не-системных типов backdrop
            // (Acrylic, SWCA 1-5) нужно инжектировать повторно, иначе DWM сбросит на дефолт.
            // EffectType=1 (default) и Mica — система сама анимирует; повторный вызов ломает transition.
            if (dwAttribute == DWMWA_CAPTION_COLOR && SUCCEEDED(hr) && !skipCaptionPath2)
            {
                DebugLog(
                    L"[GlassCore][ExplorerFrame][DWM] path2 glass re-inject SBT=%u (mapped=%u)",
                    mappedBackdrop,
                    mappedBackdrop);
                (void)orig(hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                    &mappedBackdrop, sizeof(mappedBackdrop));
            }
            else if (dwAttribute == DWMWA_CAPTION_COLOR && SUCCEEDED(hr) && skipCaptionPath2)
            {
                const wchar_t* skipReason = L"other";
                if (ctx.effectType == 1u)
                    skipReason = L"default";
                else if (ctx.effectType == 6u)
                    skipReason = L"acrylic";
                else if (mappedBackdrop == static_cast<DWORD>(DWMSBT_MAINWINDOW))
                    skipReason = L"mica";
                DebugLog(
                    L"[GlassCore][ExplorerFrame][DWM] path2 skip reason=%s (effectType=%u mapped=%u)",
                    skipReason,
                    ctx.effectType,
                    mappedBackdrop);
            }

            return hr;
        }

    } // namespace Detail

    void OnExplorerModuleLoaded(HMODULE hModule, const wchar_t* baseName) noexcept
    {
        if (Detail::g_shutdown.load(std::memory_order_acquire))
            return;
        if (!hModule || !baseName)
            return;
        if (!Detail::IsExplorerProcess())
            return;

        if (_wcsicmp(baseName, L"explorerframe.dll") == 0)
        {
            (void)Detail::EnableHooksOnModule(hModule, Detail::OHK_DWM_EXTEND_FRAME, true);
            (void)Detail::EnableHooksOnModule(hModule, Detail::OHK_CREATE_WINDOW_EX, true);
            (void)Detail::EnableHooksOnModule(hModule, Detail::OHK_DWM_SET_ATTR, true);
            (void)Detail::EnableHooksOnModule(hModule, Detail::OHK_REGISTER_CLASS_EX, true);
            (void)Detail::EnableHooksOnModule(hModule, Detail::OHK_CREATE_SOLID_BRUSH, true);
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][ExplorerFrame] OnExplorerModuleLoaded: explorerframe.dll (5 hooks)");
        }
        else if (_wcsicmp(baseName, L"shell32.dll") == 0)
        {
            (void)Detail::EnableHooksOnModule(hModule, Detail::OHK_CREATE_WINDOW_EX, true);
            //(void)Detail::EnableHooksOnModule(hModule, Detail::OHK_GET_THEME_COLOR, true);
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][ExplorerFrame] OnExplorerModuleLoaded: shell32.dll (2 hooks)");
        }
        else if (_wcsicmp(baseName, L"DUI70.dll") == 0)
        {
            //(void)Detail::EnableHooksOnModule(hModule, Detail::OHK_GET_THEME_COLOR, true);
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][ExplorerFrame] OnExplorerModuleLoaded: DUI70.dll (GetThemeColor @ DUI70)");
        }
        else if (_wcsicmp(baseName, L"shlwapi.dll") == 0)
        {
            (void)Detail::EnableHooksOnModule(hModule, Detail::OHK_EXT_TEXT_OUT_W, true);
            (void)Detail::EnableHooksOnModule(hModule, Detail::OHK_SET_BK_COLOR, true);
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][ExplorerFrame] OnExplorerModuleLoaded: shlwapi.dll (ExtTextOutW + SetBkColor)");
        }
        else if (_wcsicmp(baseName, L"twinui.pcshell.dll") == 0)
        {
            (void)Detail::EnableHooksOnModule(hModule, Detail::OHK_DWM_SET_ATTR, true);
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][ExplorerFrame] OnExplorerModuleLoaded: twinui.pcshell.dll (1 hook)");
        }
    }

    void OnExplorerModuleUnloaded(HMODULE hModule, const wchar_t* baseName) noexcept
    {
        if (!hModule)
            return;
        if (!Detail::IsExplorerProcess())
            return;

        auto lock = Detail::g_overallLock.lock_exclusive();
        auto it = Detail::g_overallMap.find(static_cast<void*>(hModule));
        if (it != Detail::g_overallMap.end())
        {
            it->second.DisableAllHooks();
            Detail::g_overallMap.erase(it);
        }

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][ExplorerFrame] OnExplorerModuleUnloaded: %s", baseName ? baseName : L"?");
    }

    void Prepare() {}

    void Startup()
    {
        Detail::g_bgHooksEnabled.store(
            Detail::ShouldEnableExplorerBgHooks(), std::memory_order_release);
    }

    void RestoreExplorerFrameChrome(ExplorerFrameRestoreScope scope) noexcept
    {
        const bool full = (scope == ExplorerFrameRestoreScope::Shutdown);
        Detail::RestoreExplorerShellChromeForTrackedRoots(full, full);
        DebugLog(
            DBG_IMPORTANT,
            L"[GlassCore][ExplorerFrame] RestoreExplorerFrameChrome scope=%s",
            full ? L"Shutdown" : L"LiveOff");
    }

    void BeginExplorerShellChromeRestore() noexcept
    {
        Detail::g_explorerChromeRestoring.store(true, std::memory_order_release);
        Detail::g_bgHooksEnabled.store(false, std::memory_order_release);
        RestoreExplorerFrameChrome(ExplorerFrameRestoreScope::LiveOff);
        DebugLog(
            DBG_IMPORTANT,
            L"[GlassCore][ExplorerFrame] BeginExplorerShellChromeRestore");

        std::vector<HWND> trackedRoots;
        {
            auto lock = Detail::g_overallLock.lock_shared();
            trackedRoots.reserve(Detail::g_trackedExplorerRoots.size());
            for (HWND h : Detail::g_trackedExplorerRoots)
            {
                if (h && IsWindow(h))
                    trackedRoots.push_back(h);
            }
        }

        for (HWND hRoot : trackedRoots)
            GlassCore::Root::RestoreWin11ExplorerStockFrame(hRoot);

        GlassCore::Root::TriggerExplorerShellChromeRedrawAll();
        GlassCore::Root::TriggerExplorerShellRootRedrawAll();
    }

    void ClearExplorerIATExtendMarker(HWND hRoot) noexcept
    {
        if (!hRoot || !IsWindow(hRoot))
            return;
        RemovePropW(hRoot, Detail::kExplorerIATExtendProp);
    }

    void ClearExplorerStockTopMarginMarker(HWND hRoot) noexcept
    {
        Detail::ClearExplorerStockTopMarginMarker(hRoot);
    }

    int GetExplorerStockTopMargin(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return 0;
        const HANDLE prop = GetPropW(hWnd, Detail::kExplorerStockTopMarginProp);
        if (!prop)
            return 0;
        const int margin = static_cast<int>(reinterpret_cast<intptr_t>(prop));
        return margin > 0 ? margin : 0;
    }

    void Shutdown()
    {
        Detail::g_shutdown.store(true, std::memory_order_seq_cst);
        Detail::g_explorerChromeRestoring.store(false, std::memory_order_seq_cst);
        Detail::g_bgHooksEnabled.store(false, std::memory_order_seq_cst);
        Detail::DisableAllOverallHooks();
        RestoreExplorerFrameChrome(ExplorerFrameRestoreScope::Shutdown);
        Detail::g_shutdown.store(false, std::memory_order_seq_cst);
    }

    void OnExplorerRootAttach(HWND hRoot) noexcept
    {
        if (Detail::g_shutdown.load(std::memory_order_acquire))
            return;
        if (!hRoot || !IsWindow(hRoot))
            return;
        if (!Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
            return;

        auto lock = Detail::g_overallLock.lock_exclusive();
        Detail::g_trackedExplorerRoots.insert(hRoot);
    }

    void OnExplorerRootDetach(HWND hRoot) noexcept
    {
        if (!hRoot)
            return;
        RemovePropW(hRoot, Detail::kExplorerIATExtendProp);
        Detail::ClearExplorerStockTopMarginMarker(hRoot);

        auto lock = Detail::g_overallLock.lock_exclusive();
        Detail::g_trackedExplorerRoots.erase(hRoot);
    }

    void SyncExplorerFrameBackdropHookFromRegistry() noexcept
    {
        if (Detail::g_shutdown.load(std::memory_order_acquire))
            return;

        const bool shouldEnable = Detail::ShouldEnableExplorerBgHooks();
        const bool wasEnabled = Detail::g_bgHooksEnabled.load(std::memory_order_acquire);
        Detail::g_bgHooksEnabled.store(shouldEnable, std::memory_order_release);

        if (wasEnabled && !shouldEnable)
            RestoreExplorerFrameChrome(ExplorerFrameRestoreScope::LiveOff);
    }

    bool DidExplorerIATExtendApply(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return false;
        return GetPropW(hWnd, Detail::kExplorerIATExtendProp) != nullptr;
    }

    bool IsWin11ExplorerShellCabinetChromeHwnd(HWND hRoot) noexcept
    {
        if (!hRoot || !IsWindow(hRoot))
            return false;
        if (Shared::SystemHelper::g_buildNumber < 22000)
            return false;
        if (!Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
            return false;
        return Shared::RootBackdropPolicy::ExplorerShellRootHasShellTabDescendant(hRoot)
            || DidExplorerIATExtendApply(hRoot);
    }
}