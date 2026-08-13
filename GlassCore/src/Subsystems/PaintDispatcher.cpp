#include "CorePch.h"
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/TextGlowClientRegistry.h>
#include <Core/Intercept/HookHelper.h>
#include <GlassControlKind.h>
#include <GlassBackdropMark.h>
#include <Hooks/ScrollBarTheme.h>
#include <WinApiUtils.h>
#include <InjectProcessMap.h>
#include <Subsystems/RootSwcaGradient.h>

using namespace GlassCore::Hooks;

namespace GlassCore::PaintDispatcher
{
    static std::atomic<bool> g_shutdown{ false };

    // Четыре раздельных списка провайдеров.
    //
    // Ключевое свойство: HookDeleteDC обращается ТОЛЬКО к g_compatDCProviders.
    // Button/Static в g_paintProviders; Button дополнительно в g_getDCProviders (GetDC без BeginPaint).
    // До них DeleteDC не доходит.
    // Это устраняет проблему: внутренние DeleteDC от uxtheme::EndBufferedPaint
    // (внутри DrawThemeContent) раньше ложно вызывали OnEndPaintGlobal и отключали
    // IAT-хуки кнопки в середине animation-фрейма.
    static std::vector<PaintProvider>    g_paintProviders;       // pre BeginPaint / EndPaint
    static std::vector<PostPaintProvider> g_postPaintProviders;   // после успешного BeginPaint
    static std::vector<PaintProvider>    g_getDCProviders;        // GetDC / ReleaseDC
    static std::vector<PaintProvider>    g_compatDCProviders;     // CreateCompatibleDC / DeleteDC

    // Стек (HWND, HDC) для BeginPaint и GetDC.
    // Поддерживает вложенные сессии (например, SendMessage(child, WM_PAINT) из wndproc).
    // g_activePaintHWnd = вершина стека = HWND для следующего CreateCompatibleDC.
    struct DCEntry { HWND hWnd; HDC hDC; };
    thread_local std::vector<DCEntry> g_dcStack;
    thread_local HWND                 g_activePaintHWnd = nullptr;
    thread_local HDC                  g_tlCurrentGetDcForProviders = nullptr;

    // Оригинальные указатели на функции (заменяются Detours)
    static decltype(&BeginPaint)         g_origBeginPaint         = nullptr;
    static decltype(&EndPaint)           g_origEndPaint           = nullptr;
    static decltype(&GetDC)              g_origGetDC              = nullptr;
    static decltype(&GetDCEx)            g_origGetDCEx            = nullptr;
    static decltype(&ReleaseDC)          g_origReleaseDC          = nullptr;
    static decltype(&CreateCompatibleDC) g_origCreateCompatibleDC = nullptr;
    static decltype(&DeleteDC)           g_origDeleteDC           = nullptr;

    // ── Helpers ──────────────────────────────────────────────────────────────

    // PopupOnly: не гонять всех провайдеров на unmarked / Root(GenericContainer).
    // Detours остаются (Full-процессы и popup-сессии), fan-out — только для kind,
    // которые реально аттачатся в PopupOnly и используют PaintDispatcher.
    [[nodiscard]] static bool IsPopupOnlyPaintDispatcherKind(
        GlassShared::GlassControlKind kind) noexcept
    {
        using GlassShared::GlassControlKind;
        switch (kind)
        {
        case GlassControlKind::ComboLBox:
        case GlassControlKind::Toolbar:
        case GlassControlKind::MonthCal:
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] static bool ShouldInvokeProvidersForHwnd(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return false;
        if (!GlassCore::Root::IsFullInjectionMode())
        {
            const auto kind = GlassShared::GlassGetWindowKind(hWnd);
            if (!IsPopupOnlyPaintDispatcherKind(kind))
                return false;
        }
        return GlassShared::GlassIsBackdropAllowedForHwnd(hWnd);
    }

    // Кладёт DC на стек и уведомляет указанный список провайдеров.
    static void PushDC(HWND hWnd, HDC hDC,
                       std::vector<PaintProvider>& providers,
                       HMODULE callerMod) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire) || !hWnd) return;
        if (!ShouldInvokeProvidersForHwnd(hWnd)) return;
        GlassCore::TextGlow::EnsureTextGlowClientRegistered(hWnd);
        g_dcStack.push_back({ hWnd, hDC });
        g_activePaintHWnd = hWnd;
        for (auto& p : providers) p.onBegin(hWnd, callerMod);
    }

    // Снимает DC со стека по значению HDC (поиск с конца — обычный случай O(1)).
    // Уведомляет указанный список провайдеров.
    static void PopDC(HDC hDC,
                      std::vector<PaintProvider>& providers,
                      HMODULE callerMod) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire)) return;
        for (auto it = g_dcStack.rbegin(); it != g_dcStack.rend(); ++it)
        {
            if (it->hDC != hDC) continue;

            HWND hWnd = it->hWnd;
            g_dcStack.erase((it + 1).base());
            g_activePaintHWnd = g_dcStack.empty() ? nullptr : g_dcStack.back().hWnd;

            for (auto& p : providers) p.onEnd(hWnd, callerMod);
            return;
        }
    }

    // ── Detours хуки ─────────────────────────────────────────────────────────

    static HDC WINAPI HookBeginPaint(HWND hWnd, LPPAINTSTRUCT lpPaint)
    {
        // PopupOnly fast-path: без DetourGetContainingModule / провайдеров на чужих HWND.
        if (!g_shutdown.load(std::memory_order_acquire)
            && !GlassCore::Root::IsFullInjectionMode()
            && hWnd
            && !IsPopupOnlyPaintDispatcherKind(GlassShared::GlassGetWindowKind(hWnd)))
        {
            return g_origBeginPaint(hWnd, lpPaint);
        }

        HMODULE callerMod = DetourGetContainingModule(_ReturnAddress());
        // onBegin ДО оригинала: IAT патчится до первого рисования в wndproc после BeginPaint.
        // Допущение: сам BeginPaint не инициирует повторный WM_PAINT/DrawTheme до возврата
        // (недокументированное поведение ОС; перенос onBegin после оригинала рискован).
        const bool notifyProviders = !g_shutdown.load(std::memory_order_acquire)
            && ShouldInvokeProvidersForHwnd(hWnd);
        if (notifyProviders)
        {
            GlassCore::TextGlow::EnsureTextGlowClientRegistered(hWnd);
            for (auto& p : g_paintProviders) p.onBegin(hWnd, callerMod);
        }
        HDC hdc = g_origBeginPaint(hWnd, lpPaint);
        if (hdc)
        {
            bool backgroundCleared = false;
            if (notifyProviders)
            {
                for (auto& p : g_postPaintProviders)
                {
                    if (!p.onAfterBeginPaint)
                        continue;
                    if (p.onAfterBeginPaint(hWnd, hdc, lpPaint, callerMod))
                        backgroundCleared = true;
                }
            }
            if (backgroundCleared && lpPaint)
                lpPaint->fErase = 0;
            g_dcStack.push_back({ hWnd, hdc });
            g_activePaintHWnd = hWnd;
        }
        else if (notifyProviders)
        {
            // BeginPaint вернул NULL — EndPaint для этой пары не будет; компенсируем onBegin,
            // чтобы провайдеры (стек HMODULE, HookDispatcher::hookRef) не разбалансировались.
            for (auto& p : g_paintProviders) p.onEnd(hWnd, callerMod);
        }
        return hdc;
    }

    static BOOL WINAPI HookEndPaint(HWND hWnd, const PAINTSTRUCT* lpPaint)
    {
        if (!g_shutdown.load(std::memory_order_acquire)
            && !GlassCore::Root::IsFullInjectionMode()
            && hWnd
            && !IsPopupOnlyPaintDispatcherKind(GlassShared::GlassGetWindowKind(hWnd)))
        {
            return g_origEndPaint(hWnd, lpPaint);
        }
        HMODULE callerMod = DetourGetContainingModule(_ReturnAddress());
        if (lpPaint) PopDC(lpPaint->hdc, g_paintProviders, callerMod);
        return g_origEndPaint(hWnd, lpPaint);
    }

    static HDC WINAPI HookGetDC(HWND hWnd)
    {
        if (!g_shutdown.load(std::memory_order_acquire)
            && !GlassCore::Root::IsFullInjectionMode()
            && hWnd
            && !IsPopupOnlyPaintDispatcherKind(GlassShared::GlassGetWindowKind(hWnd)))
        {
            return g_origGetDC(hWnd);
        }

        HMODULE callerMod = DetourGetContainingModule(_ReturnAddress());
        HDC hdc = g_origGetDC(hWnd);   // сначала оригинал — нужен валидный DC

        if (hdc)
        {
            g_tlCurrentGetDcForProviders = hdc;
            PushDC(hWnd, hdc, g_getDCProviders, callerMod);
            g_tlCurrentGetDcForProviders = nullptr;
        }
        return hdc;
    }

    static HDC WINAPI HookGetDCEx(HWND hWnd, HRGN hrgnClip, DWORD flags)
    {
        if (!g_shutdown.load(std::memory_order_acquire)
            && !GlassCore::Root::IsFullInjectionMode()
            && hWnd
            && !IsPopupOnlyPaintDispatcherKind(GlassShared::GlassGetWindowKind(hWnd)))
        {
            return g_origGetDCEx ? g_origGetDCEx(hWnd, hrgnClip, flags) : nullptr;
        }

        HMODULE callerMod = DetourGetContainingModule(_ReturnAddress());
        HDC     hdc       = g_origGetDCEx ? g_origGetDCEx(hWnd, hrgnClip, flags) : nullptr;
        if (hdc)
        {
            // Ленивая установка IAT ScrollBar на comctl32: caller=comctl32 или uxtheme; HWND — whitelist / ComctlScrollHost.
            // Для TreeView/ListView дублирующий вызов Ensure также в onBegin (до оригинального BeginPaint).
            if (ShouldInvokeProvidersForHwnd(hWnd))
                GlassCore::ScrollBarTheme::EnsureComctlScrollBarIatIfComctlCaller(hWnd, callerMod);
            g_tlCurrentGetDcForProviders = hdc;
            PushDC(hWnd, hdc, g_getDCProviders, callerMod);
            g_tlCurrentGetDcForProviders = nullptr;
        }
        return hdc;
    }

    static int WINAPI HookReleaseDC(HWND hWnd, HDC hDC)
    {
        if (!g_shutdown.load(std::memory_order_acquire)
            && !GlassCore::Root::IsFullInjectionMode()
            && hWnd
            && !IsPopupOnlyPaintDispatcherKind(GlassShared::GlassGetWindowKind(hWnd)))
        {
            return g_origReleaseDC(hWnd, hDC);
        }
        HMODULE callerMod = DetourGetContainingModule(_ReturnAddress());
        PopDC(hDC, g_getDCProviders, callerMod);
        return g_origReleaseDC(hWnd, hDC);
    }

    // CreateCompatibleDC/DeleteDC — только g_compatDCProviders.
    // Button/Static зарегистрированы в g_paintProviders → DeleteDC их не трогает.
    static HDC WINAPI HookCreateCompatibleDC(HDC hdcSrc)
    {
        HMODULE callerMod = DetourGetContainingModule(_ReturnAddress());
        HDC hdc = g_origCreateCompatibleDC(hdcSrc);
        // Стек DC ведём всегда (не только при g_compatDCProviders), чтобы MEMDC из buffered paint
        // отражался в IsPaintStackDcForHwnd для Edit session buffer.
        if (hdc && g_activePaintHWnd && ShouldInvokeProvidersForHwnd(g_activePaintHWnd))
        {
            g_dcStack.push_back({ g_activePaintHWnd, hdc });
            for (auto& p : g_compatDCProviders)
                p.onBegin(g_activePaintHWnd, callerMod);
        }
        return hdc;
    }

    static BOOL WINAPI HookDeleteDC(HDC hdc)
    {
        HMODULE callerMod = DetourGetContainingModule(_ReturnAddress());
        PopDC(hdc, g_compatDCProviders, callerMod);
        return g_origDeleteDC(hdc);
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void RegisterPaintProvider       (PaintProvider p) { g_paintProviders.push_back(p); }
    void RegisterPostPaintProvider   (PostPaintProvider p) { g_postPaintProviders.push_back(p); }
    void RegisterGetDCProvider       (PaintProvider p) { g_getDCProviders.push_back(p); }
    void RegisterCompatDCProvider    (PaintProvider p) { g_compatDCProviders.push_back(p); }

    void Startup()
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        HMODULE gdi32  = GetModuleHandleW(L"gdi32.dll");
        if (!gdi32) gdi32 = GetModuleHandleW(L"gdi32full.dll");

        g_origBeginPaint         = reinterpret_cast<decltype(&BeginPaint)>(
            GetProcAddress(user32, "BeginPaint"));
        g_origEndPaint           = reinterpret_cast<decltype(&EndPaint)>(
            GetProcAddress(user32, "EndPaint"));
        g_origGetDC              = reinterpret_cast<decltype(&GetDC)>(
            GetProcAddress(user32, "GetDC"));
        g_origGetDCEx            = reinterpret_cast<decltype(&GetDCEx)>(
            GetProcAddress(user32, "GetDCEx"));
        g_origReleaseDC          = reinterpret_cast<decltype(&ReleaseDC)>(
            GetProcAddress(user32, "ReleaseDC"));
        g_origCreateCompatibleDC = reinterpret_cast<decltype(&CreateCompatibleDC)>(
            GetProcAddress(gdi32, "CreateCompatibleDC"));
        g_origDeleteDC           = reinterpret_cast<decltype(&DeleteDC)>(
            GetProcAddress(gdi32, "DeleteDC"));

        Detours::Write([&]()
        {
            Detours::Attach(reinterpret_cast<PVOID*>(&g_origBeginPaint),         HookBeginPaint);
            Detours::Attach(reinterpret_cast<PVOID*>(&g_origEndPaint),           HookEndPaint);
            Detours::Attach(reinterpret_cast<PVOID*>(&g_origGetDC),              HookGetDC);
            if (g_origGetDCEx)
                Detours::Attach(reinterpret_cast<PVOID*>(&g_origGetDCEx), HookGetDCEx);
            Detours::Attach(reinterpret_cast<PVOID*>(&g_origReleaseDC),          HookReleaseDC);
            Detours::Attach(reinterpret_cast<PVOID*>(&g_origCreateCompatibleDC), HookCreateCompatibleDC);
            Detours::Attach(reinterpret_cast<PVOID*>(&g_origDeleteDC),           HookDeleteDC);
        });

        DebugLog(DBG_IMPORTANT, L"[GlassCore][PaintDispatcher] PaintDispatcher::Startup");
        (void)Shared::InjectProcessMap::TrySetDiagFlag(
            GetCurrentProcessId(),
            Shared::InjectProcessMap::DiagPaintDispatcher,
            true);
    }

    HWND GetPaintSessionHostHwnd() noexcept
    {
        return g_activePaintHWnd;
    }

    HDC GetCurrentGetDcSessionHdc() noexcept
    {
        return g_tlCurrentGetDcForProviders;
    }

    HDC GetPaintSessionBaseWindowDc(HWND hWnd) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire) || !hWnd)
            return nullptr;
        for (auto it = g_dcStack.rbegin(); it != g_dcStack.rend(); ++it)
        {
            if (it->hWnd != hWnd || !it->hDC)
                continue;
            if (GetObjectType(it->hDC) == OBJ_MEMDC)
                continue;
            if (WindowFromDC(it->hDC) == hWnd)
                return it->hDC;
        }
        return nullptr;
    }

    bool IsPaintStackDcForHwnd(HWND hWnd, HDC hdc) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire) || !hWnd || !hdc)
            return false;
        for (auto it = g_dcStack.rbegin(); it != g_dcStack.rend(); ++it)
        {
            if (it->hWnd == hWnd && it->hDC == hdc)
                return true;
        }
        return false;
    }

    void Shutdown()
    {
        (void)Shared::InjectProcessMap::TrySetDiagFlag(
            GetCurrentProcessId(),
            Shared::InjectProcessMap::DiagPaintDispatcher,
            false);

        g_shutdown.store(true, std::memory_order_seq_cst);

        Detours::Write([&]()
        {
            Detours::Detach(reinterpret_cast<PVOID*>(&g_origBeginPaint),         HookBeginPaint);
            Detours::Detach(reinterpret_cast<PVOID*>(&g_origEndPaint),           HookEndPaint);
            Detours::Detach(reinterpret_cast<PVOID*>(&g_origGetDC),              HookGetDC);
            if (g_origGetDCEx)
                Detours::Detach(reinterpret_cast<PVOID*>(&g_origGetDCEx), HookGetDCEx);
            Detours::Detach(reinterpret_cast<PVOID*>(&g_origReleaseDC),          HookReleaseDC);
            Detours::Detach(reinterpret_cast<PVOID*>(&g_origCreateCompatibleDC), HookCreateCompatibleDC);
            Detours::Detach(reinterpret_cast<PVOID*>(&g_origDeleteDC),           HookDeleteDC);
        });

        g_paintProviders.clear();
        g_postPaintProviders.clear();
        g_getDCProviders.clear();
        g_compatDCProviders.clear();
        g_dcStack.clear();
        g_activePaintHWnd = nullptr;
        g_shutdown.store(false, std::memory_order_seq_cst);
        DebugLog(DBG_IMPORTANT, L"[GlassCore][PaintDispatcher] PaintDispatcher::Shutdown");
    }
}
