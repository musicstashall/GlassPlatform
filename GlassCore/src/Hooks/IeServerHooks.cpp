// InternetExplorer_Server (MMC DocObject): mshtml копирует кадр StretchBlt на клиент IE.
// IAT только StretchBlt на mshtml.dll; сессия BeginPaint/EndPaint и GetDC/ReleaseDC через PaintDispatcher.

#include "CorePch.h"

#include <Hooks/IeServer.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Subsystems/PaintDispatcher.h>
#include <GlassControlKind.h>
#include <DebugLog.h>

#pragma comment(lib, "msimg32.lib")

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <unordered_map>
#include <vector>

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::IeServerControl
{
    namespace
    {
        enum class IeSessionKind : uint8_t
        {
            Paint = 1,
            GetDc = 2,
        };

        thread_local std::vector<IeSessionKind> g_ieSessionStack;

        static std::atomic<bool> g_shutdown{ false };

        // Серый фон снимка MMC из трейса (mmcndmgr → memory-DC).
        constexpr COLORREF kMmcSnapGrayBrush = static_cast<COLORREF>(13743257);

        // Фон IE не из GCLP (MSHTML рисует сам). Без COM/DOM — якоря по системным цветам и типичным фонам MMC.
        // «Управление компьютером» и др. MMC: холст ~0xF0F0F0 — это GetSysColor(COLOR_BTNFACE).

        // Пороги мягкой маски: «близко к фону» → прозрачно, далеко → непрозрачно; между — градиент.
        struct IeStretchMaskParams
        {
            int  distTransparentMax = 0; // L1 до ближайшего якоря <= порога -> alpha 0
            int  distOpaqueMin      = 10; // >= порога -> alpha 255
            bool boxBlur3           = true;
        };

        constexpr IeStretchMaskParams kDefaultMaskParams{};

        [[nodiscard]] int ColorDistL1(COLORREF a, COLORREF b) noexcept
        {
            const int dr = std::abs(static_cast<int>((a >> 16) & 0xFF) - static_cast<int>((b >> 16) & 0xFF));
            const int dg = std::abs(static_cast<int>((a >> 8) & 0xFF) - static_cast<int>((b >> 8) & 0xFF));
            const int db = std::abs(static_cast<int>(a & 0xFF) - static_cast<int>(b & 0xFF));
            return dr + dg + db;
        }

        // Пиксели DIB после StretchBlt (BGR, старший байт не задан) → премультиплированный BGRA для GdiAlphaBlend.
        void ApplySoftAlphaMask(
            DWORD* px,
            int    width,
            int    height,
            const IeStretchMaskParams& p)
        {
            if (!px || width <= 0 || height <= 0)
                return;

            const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
            const COLORREF windowBg = GetSysColor(COLOR_WINDOW) & 0xFFFFFFu;
            const COLORREF btnFaceBg = GetSysColor(COLOR_BTNFACE) & 0xFFFFFFu;
            const COLORREF anchors[4] = { 0xFFFFFFu, windowBg, btnFaceBg, kMmcSnapGrayBrush & 0xFFFFFFu };

            std::vector<BYTE> alpha;
            alpha.resize(count);

            for (size_t i = 0; i < count; ++i)
            {
                const COLORREF rgb = px[i] & 0xFFFFFFu;
                int            dmin = INT_MAX;
                for (const COLORREF anch : anchors)
                    dmin = std::min(dmin, ColorDistL1(rgb, anch));

                int a;
                if (dmin <= p.distTransparentMax)
                    a = 0;
                else if (dmin >= p.distOpaqueMin)
                    a = 255;
                else
                    a = (255 * (dmin - p.distTransparentMax)) / (p.distOpaqueMin - p.distTransparentMax);
                alpha[i] = static_cast<BYTE>(a);
            }

            if (p.boxBlur3 && width >= 2 && height >= 2)
            {
                std::vector<BYTE> tmp = alpha;
                for (int y = 0; y < height; ++y)
                {
                    for (int x = 0; x < width; ++x)
                    {
                        int sum = 0;
                        int n   = 0;
                        for (int dy = -1; dy <= 1; ++dy)
                        {
                            const int ny = y + dy;
                            if (ny < 0 || ny >= height)
                                continue;
                            for (int dx = -1; dx <= 1; ++dx)
                            {
                                const int nx = x + dx;
                                if (nx < 0 || nx >= width)
                                    continue;
                                sum += tmp[static_cast<size_t>(ny) * static_cast<size_t>(width) + static_cast<size_t>(nx)];
                                ++n;
                            }
                        }
                        if (n > 0)
                            alpha[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] =
                                static_cast<BYTE>((sum + n / 2) / n);
                    }
                }
            }

            for (size_t i = 0; i < count; ++i)
            {
                const BYTE a = alpha[i];
                if (a == 0)
                {
                    px[i] = 0;
                    continue;
                }
                const BYTE b = static_cast<BYTE>(px[i] & 0xFFu);
                const BYTE g = static_cast<BYTE>((px[i] >> 8) & 0xFFu);
                const BYTE r = static_cast<BYTE>((px[i] >> 16) & 0xFFu);
                const BYTE ob  = static_cast<BYTE>(static_cast<unsigned>(b) * static_cast<unsigned>(a) / 255u);
                const BYTE og  = static_cast<BYTE>(static_cast<unsigned>(g) * static_cast<unsigned>(a) / 255u);
                const BYTE or_ = static_cast<BYTE>(static_cast<unsigned>(r) * static_cast<unsigned>(a) / 255u);
                px[i] = static_cast<DWORD>(ob) | (static_cast<DWORD>(og) << 8) | (static_cast<DWORD>(or_) << 16)
                    | (static_cast<DWORD>(a) << 24);
            }
        }

        [[nodiscard]] bool StrEqI(LPCWSTR a, LPCWSTR b) noexcept
        {
            return a && b && _wcsicmp(a, b) == 0;
        }

        [[nodiscard]] bool CallerBaseIs(HMODULE caller, LPCWSTR base) noexcept
        {
            if (!caller || !base)
                return false;
            wchar_t path[MAX_PATH]{};
            if (!GetModuleFileNameW(caller, path, MAX_PATH))
                return false;
            const wchar_t* p    = wcsrchr(path, L'\\');
            const wchar_t* name = p ? p + 1 : path;
            return StrEqI(name, base);
        }

        [[nodiscard]] bool LogThrottleMs(ULONGLONG& lastTick, DWORD periodMs) noexcept
        {
            const ULONGLONG n = GetTickCount64();
            if (n - lastTick < periodMs)
                return false;
            lastTick = n;
            return true;
        }

        [[nodiscard]] HWND HwndIeFromDestDc(HDC hdcDest) noexcept
        {
            if (!hdcDest)
                return nullptr;
            HWND w = WindowFromDC(hdcDest);
            if (w && GlassShared::GlassGetWindowKind(w) == GlassShared::GlassControlKind::InternetExplorer_Server)
                return w;
            w = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
            if (w && GlassShared::GlassGetWindowKind(w) == GlassShared::GlassControlKind::InternetExplorer_Server)
                return w;
            return nullptr;
        }

        [[nodiscard]] bool TryIeStretchBltSoftMaskToDest(
            decltype(&StretchBlt) orig,
            HDC   hdcDest,
            int   xDest,
            int   yDest,
            int   wDest,
            int   hDest,
            HDC   hdcSrc,
            int   xSrc,
            int   ySrc,
            int   wSrc,
            int   hSrc,
            const IeStretchMaskParams& maskParams)
        {
            if (!orig || !hdcDest || !hdcSrc)
                return false;
            if (wDest <= 0 || hDest <= 0 || wSrc <= 0 || hSrc <= 0)
                return false;

            constexpr size_t kMaxPixels = 12'000'000u;
            const long long  px64       = static_cast<long long>(wDest) * static_cast<long long>(hDest);
            if (px64 <= 0 || static_cast<size_t>(px64) > kMaxPixels)
                return false;
            const size_t px = static_cast<size_t>(px64);

            BITMAPINFO bi{};
            bi.bmiHeader.biSize          = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth         = wDest;
            bi.bmiHeader.biHeight        = -hDest;
            bi.bmiHeader.biPlanes        = 1;
            bi.bmiHeader.biBitCount      = 32;
            bi.bmiHeader.biCompression   = BI_RGB;

            void*             bits = nullptr;
            wil::unique_hbitmap dib{ CreateDIBSection(hdcDest, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) };
            if (!dib || !bits)
                return false;

            wil::unique_hdc hdcWork{ CreateCompatibleDC(hdcDest) };
            if (!hdcWork)
                return false;

            HGDIOBJ oldBmp = SelectObject(hdcWork.get(), dib.get());
            if (!oldBmp)
                return false;

            if (!orig(hdcWork.get(), 0, 0, wDest, hDest, hdcSrc, xSrc, ySrc, wSrc, hSrc, SRCCOPY))
            {
                (void)SelectObject(hdcWork.get(), oldBmp);
                return false;
            }

            ApplySoftAlphaMask(static_cast<DWORD*>(bits), wDest, hDest, maskParams);

            // Каждый кадр — с «чистого» прямоугольника: без сброса GdiAlphaBlend наслаивается на старые
            // полупрозрачные пиксели и текст получает накопительное «свечение». BLACKNESS — как в других
            // glass-путях платформы; учитывается текущий клип DC.
            PatBlt(hdcDest, xDest, yDest, wDest, hDest, BLACKNESS);

            BLENDFUNCTION blend{};
            blend.BlendOp             = AC_SRC_OVER;
            blend.BlendFlags          = 0;
            blend.SourceConstantAlpha = 255;
            blend.AlphaFormat         = AC_SRC_ALPHA;

            const BOOL ok = GdiAlphaBlend(
                hdcDest,
                xDest,
                yDest,
                wDest,
                hDest,
                hdcWork.get(),
                0,
                0,
                wDest,
                hDest,
                blend);
            (void)SelectObject(hdcWork.get(), oldBmp);
            return ok == TRUE;
        }

        BOOL WINAPI Overall_MyStretchBlt(
            HDC   hdcDest,
            int   xDest,
            int   yDest,
            int   wDest,
            int   hDest,
            HDC   hdcSrc,
            int   xSrc,
            int   ySrc,
            int   wSrc,
            int   hSrc,
            DWORD rop);

        enum OverallHookIndex : size_t
        {
            OHK_STRETCHBLT = 0,
            OHK_COUNT
        };

        inline const HookDispatcherDependency<OHK_COUNT, 3> g_overallHookDep
        { {
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
                "StretchBlt", reinterpret_cast<PVOID>(Overall_MyStretchBlt) },
        } };

        using OverallDispatcher = HookDispatcher<OHK_COUNT, 3>;

        static std::unordered_map<void*, OverallDispatcher> g_overallMap;
        static wil::srwlock                                 g_overallLock;

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
                it->second.moduleAddress = baseAddress;

            it->second.EnableHook(index, enable);
            return true;
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
            auto it   = g_overallMap.find(static_cast<void*>(callerModule));
            if (it != g_overallMap.end())
                return it->second.template GetOrg<index, T>();
            return nullptr;
        }

        static void EnableIeStretchHook(bool enable) noexcept
        {
            if (g_shutdown.load(std::memory_order_acquire))
                return;

            void* const msh = GetModuleHandleW(L"mshtml.dll");
            DebugLog(
                DBG_IMPORTANT,
                L"[GlassCore][IeServer] StretchBlt IAT %ls mshtml=%p",
                enable ? L"ON" : L"OFF",
                msh);

            if (msh)
                (void)EnableHooksOnModule(msh, OHK_STRETCHBLT, enable);
            else if (enable)
                DebugLog(DBG_WARNING, L"[GlassCore][IeServer] mshtml.dll не загружен — StretchBlt IAT не включён");
        }

        static void CacheModuleIfPresent(const wchar_t* dll) noexcept
        {
            void* m = GetModuleHandleW(dll);
            if (!m)
                return;
            auto lock = g_overallLock.lock_exclusive();
            auto [it, inserted] = g_overallMap.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(m),
                std::forward_as_tuple(g_overallHookDep, m));
            if (inserted)
                it->second.CacheHookData();
        }

        BOOL WINAPI Overall_MyStretchBlt(
            HDC   hdcDest,
            int   xDest,
            int   yDest,
            int   wDest,
            int   hDest,
            HDC   hdcSrc,
            int   xSrc,
            int   ySrc,
            int   wSrc,
            int   hSrc,
            DWORD rop)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto    orig      = GetOverallOrigSafe<OHK_STRETCHBLT, decltype(&StretchBlt)>(callerMod);

            const bool callerMshtml = CallerBaseIs(callerMod, L"mshtml.dll");
            if (callerMshtml)
            {
                static ULONGLONG s_sb = 0;
                if (LogThrottleMs(s_sb, 350))
                {
                    DebugLog(
                        DBG_NORMAL,
                        L"[GlassCore][IeServer][StretchBlt] mshtml sess=%zu destDC=%p rop=0x%X %dx%d",
                        g_ieSessionStack.size(),
                        hdcDest,
                        rop,
                        wDest,
                        hDest);
                }
            }

            if (!orig)
            {
                if (callerMshtml)
                {
                    static ULONGLONG s_no = 0;
                    if (LogThrottleMs(s_no, 800))
                        DebugLog(DBG_WARNING, L"[GlassCore][IeServer][StretchBlt] orig=null для mshtml");
                }
                return StretchBlt(hdcDest, xDest, yDest, wDest, hDest, hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);
            }

            if (g_ieSessionStack.empty() || !callerMshtml || rop != SRCCOPY)
                return orig(hdcDest, xDest, yDest, wDest, hDest, hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);

            if (!HwndIeFromDestDc(hdcDest))
            {
                static ULONGLONG s_nd = 0;
                if (LogThrottleMs(s_nd, 500))
                    DebugLog(DBG_NORMAL, L"[GlassCore][IeServer][StretchBlt] dest не отмеченный IE — orig");
                return orig(hdcDest, xDest, yDest, wDest, hDest, hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);
            }

            if (TryIeStretchBltSoftMaskToDest(
                    orig,
                    hdcDest,
                    xDest,
                    yDest,
                    wDest,
                    hDest,
                    hdcSrc,
                    xSrc,
                    ySrc,
                    wSrc,
                    hSrc,
                    kDefaultMaskParams))
            {
                static ULONGLONG s_ok = 0;
                if (LogThrottleMs(s_ok, 400))
                    DebugLog(DBG_IMPORTANT, L"[GlassCore][IeServer][StretchBlt] AlphaBlend OK %dx%d", wDest, hDest);
                return TRUE;
            }

            return orig(hdcDest, xDest, yDest, wDest, hDest, hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);
        }

        static void OnIePaintBegin(HWND hWnd, HMODULE /*callerMod*/) noexcept
        {
            using namespace GlassShared;
            if (g_shutdown.load(std::memory_order_acquire))
                return;
            if (GlassGetWindowKind(hWnd) != GlassControlKind::InternetExplorer_Server)
                return;

            if (g_ieSessionStack.empty())
                EnableIeStretchHook(true);
            g_ieSessionStack.push_back(IeSessionKind::Paint);
            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][IeServer] sess BeginPaint hwnd=0x%p depth=%zu",
                hWnd,
                g_ieSessionStack.size());
        }

        static void OnIePaintEnd(HWND hWnd, HMODULE /*callerMod*/) noexcept
        {
            using namespace GlassShared;
            if (GlassGetWindowKind(hWnd) != GlassControlKind::InternetExplorer_Server)
                return;
            if (g_ieSessionStack.empty() || g_ieSessionStack.back() != IeSessionKind::Paint)
                return;
            g_ieSessionStack.pop_back();
            DebugLog(
                DBG_NORMAL,
                L"[GlassCore][IeServer] sess EndPaint hwnd=0x%p depth=%zu",
                hWnd,
                g_ieSessionStack.size());
            if (g_ieSessionStack.empty())
                EnableIeStretchHook(false);
        }

        static void OnIeGetDcBegin(HWND hWnd, HMODULE /*callerMod*/) noexcept
        {
            using namespace GlassShared;
            if (g_shutdown.load(std::memory_order_acquire))
                return;
            if (GlassGetWindowKind(hWnd) != GlassControlKind::InternetExplorer_Server)
                return;

            if (g_ieSessionStack.empty())
                EnableIeStretchHook(true);
            g_ieSessionStack.push_back(IeSessionKind::GetDc);
            DebugLog(
                DBG_IMPORTANT,
                L"[GlassCore][IeServer] sess GetDC hwnd=0x%p depth=%zu",
                hWnd,
                g_ieSessionStack.size());
        }

        static void OnIeGetDcEnd(HWND hWnd, HMODULE /*callerMod*/) noexcept
        {
            using namespace GlassShared;
            if (GlassGetWindowKind(hWnd) != GlassControlKind::InternetExplorer_Server)
                return;
            if (g_ieSessionStack.empty() || g_ieSessionStack.back() != IeSessionKind::GetDc)
                return;
            g_ieSessionStack.pop_back();
            DebugLog(
                DBG_IMPORTANT,
                L"[GlassCore][IeServer] sess ReleaseDC hwnd=0x%p depth=%zu",
                hWnd,
                g_ieSessionStack.size());
            if (g_ieSessionStack.empty())
                EnableIeStretchHook(false);
        }

    } // namespace

    void Prepare() {}

    void Startup()
    {
        GlassCore::PaintDispatcher::RegisterPaintProvider({ OnIePaintBegin, OnIePaintEnd });
        GlassCore::PaintDispatcher::RegisterGetDCProvider({ OnIeGetDcBegin, OnIeGetDcEnd });

        CacheModuleIfPresent(L"mshtml.dll");

        DebugLog(
            DBG_IMPORTANT,
            L"[GlassCore][IeServer] Startup: провайдеры; mshtml=%p",
            GetModuleHandleW(L"mshtml.dll"));
    }

    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_seq_cst);
        DisableAllOverallHooks();
        g_ieSessionStack.clear();
        g_shutdown.store(false, std::memory_order_seq_cst);
    }
}
