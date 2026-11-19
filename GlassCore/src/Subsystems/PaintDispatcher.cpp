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
    // g_activePaintHWnd — ближайшая не-Foreign запись сверху (хост стеклянной сессии).
    // Foreign: PopupOnly-барьер чужого BeginPaint/GetDC поверх стеклянной сессии.
    // Такая запись не будит провайдеров и прячет хост, пока чужое окно рисуется.
    enum class DcEntryType : unsigned char { Paint, GetDC, Compat, Foreign };
    // rootVisibleAtPush: GA_ROOT был видим при push. Скрытый к моменту prune корень
    // протухает только если был видим — снимок AnimateWindow/WM_PRINT скрытого
    // списка (push до показа) не срезается посреди анимации.
    struct DCEntry { HWND hWnd; HDC hDC; DcEntryType type; bool rootVisibleAtPush = false; };
    thread_local std::vector<DCEntry> g_dcStack;
    thread_local HWND                 g_activePaintHWnd = nullptr;
    thread_local HDC                  g_tlCurrentGetDcForProviders = nullptr;
    thread_local bool                 g_pruningCompat = false;
    thread_local unsigned             g_pruneLogBudget = 32;
    thread_local unsigned             g_foreignLogBudget = 32;

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
        case GlassControlKind::TrackBar:
            // TrackBar: в Full все сессии идут через провайдеров/GDI-подмены
            // (SetBkBrush COLOR_WINDOW/BTNFACE/MENU → BLACK в Root GDI-хуках);
            // в PopupOnly то же самое нужно доаттаченным слайдерам попапа
            // «Настройки вида», иначе GetSysColorBrush no-op → белая плашка
            // (Round-5: «трекбар по-прежнему белый»).
            return true;
        case GlassControlKind::ListView:
        case GlassControlKind::WinForms_ListView:
            // PopupOnly: доаттаченные списки под приаттаченным flyout'ом (DropDown/ListviewPopup,
            // см. WinEventHandler: attach только в цепочке предков DropDown). Сессия даёт
            // glass в Full-стиле этому ListView: чёрный фон LVM + glass-скроллбар
            // (comctl ScrollBar DTB). Неприаттаченный (kind=None) — не диспетчеризуется.
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

    static void RefreshActivePaintHwnd() noexcept
    {
        g_activePaintHWnd = nullptr;
        for (auto it = g_dcStack.rbegin(); it != g_dcStack.rend(); ++it)
        {
            if (it->type == DcEntryType::Foreign)
                continue;
            g_activePaintHWnd = it->hWnd;
            return;
        }
    }

    static void EndCompatProviders(HWND hWnd) noexcept
    {
        for (auto& p : g_compatDCProviders)
        {
            if (p.onEnd)
                p.onEnd(hWnd, nullptr);
        }
    }

    // PopupOnly: compat-запись протухла, если окно уничтожено, маркер kind снят
    // или GA_ROOT скрыт после того, как был видим (uxtheme кэширует memory-DC после закрытия попапа).
    // Foreign протухает только с уничтоженным окном (EndPaint/ReleaseDC уже не придут).
    [[nodiscard]] static bool IsStaleEntry(const DCEntry& entry) noexcept
    {
        if (entry.type == DcEntryType::Foreign)
            return !entry.hWnd || !IsWindow(entry.hWnd);
        if (entry.type != DcEntryType::Compat)
            return false;
        if (!entry.hWnd || !IsWindow(entry.hWnd))
            return true;
        if (GlassShared::GlassGetWindowKind(entry.hWnd) == GlassShared::GlassControlKind::None)
            return true;
        if (!entry.rootVisibleAtPush)
            return false;
        const HWND root = GetAncestor(entry.hWnd, GA_ROOT);
        return !root || !IsWindowVisible(root);
    }

    [[nodiscard]] static bool IsRootVisible(HWND hWnd) noexcept
    {
        const HWND root = hWnd ? GetAncestor(hWnd, GA_ROOT) : nullptr;
        return root && IsWindowVisible(root);
    }

    static void LogPrunedCompat(HWND hWnd) noexcept
    {
        if (g_pruneLogBudget == 0)
            return;
        --g_pruneLogBudget;
        wchar_t cls[128]{};
        if (hWnd && IsWindow(hWnd))
            GetClassNameW(hWnd, cls, static_cast<int>(std::size(cls)));
        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][PaintDispatcher] prune stale entry hwnd=0x%p class=\"%s\"",
            hWnd, cls);
    }

    static void PruneStaleCompatEntries() noexcept
    {
        if (g_pruningCompat || GlassCore::Root::IsFullInjectionMode())
            return;
        g_pruningCompat = true;
        bool removed = false;
        for (size_t i = 0; i < g_dcStack.size();)
        {
            if (!IsStaleEntry(g_dcStack[i]))
            {
                ++i;
                continue;
            }
            const HWND stale = g_dcStack[i].hWnd;
            const bool compat = (g_dcStack[i].type == DcEntryType::Compat);
            g_dcStack.erase(g_dcStack.begin() + static_cast<std::ptrdiff_t>(i));
            removed = true;
            LogPrunedCompat(stale);
            if (compat)
                EndCompatProviders(stale);
        }
        if (removed)
            RefreshActivePaintHwnd();
        g_pruningCompat = false;
    }

    // PopupOnly: compat-DC, созданные внутри Paint/GetDC-сессии, закрываются вместе с ней.
    // DeleteDC кэшированного memory-DC позже запись уже не найдёт и onEnd не повторит.
    static void UnwindCompatAbove(size_t keepIndex) noexcept
    {
        if (GlassCore::Root::IsFullInjectionMode() || g_pruningCompat)
            return;
        if (keepIndex >= g_dcStack.size())
            return;
        g_pruningCompat = true;
        for (size_t i = g_dcStack.size(); i-- > keepIndex + 1;)
        {
            const DcEntryType type = g_dcStack[i].type;
            if (type != DcEntryType::Compat && type != DcEntryType::Foreign)
                continue;
            const HWND stale = g_dcStack[i].hWnd;
            g_dcStack.erase(g_dcStack.begin() + static_cast<std::ptrdiff_t>(i));
            LogPrunedCompat(stale);
            if (type == DcEntryType::Compat)
                EndCompatProviders(stale);
        }
        g_pruningCompat = false;
    }

    static void PushForeignBarrier(HWND hWnd, HDC hDC) noexcept
    {
        if (!hWnd || !hDC)
            return;
        const HWND covered = g_activePaintHWnd;
        if (g_foreignLogBudget > 0)
        {
            --g_foreignLogBudget;
            wchar_t cls[128]{};
            GetClassNameW(hWnd, cls, static_cast<int>(std::size(cls)));
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][PaintDispatcher] foreign barrier hwnd=0x%p class=\"%s\" over host=0x%p",
                hWnd, cls, covered);
        }
        g_dcStack.push_back({ hWnd, hDC, DcEntryType::Foreign });
        g_activePaintHWnd = nullptr;
    }

    static void PopForeignBarrier(HWND hWnd, HDC hDC) noexcept
    {
        if (!hDC)
            return;
        for (auto it = g_dcStack.rbegin(); it != g_dcStack.rend(); ++it)
        {
            if (it->type != DcEntryType::Foreign || it->hDC != hDC)
                continue;
            if (hWnd && it->hWnd != hWnd)
                continue;
            g_dcStack.erase((it + 1).base());
            RefreshActivePaintHwnd();
            return;
        }
    }

    // Кладёт DC на стек и уведомляет указанный список провайдеров.
    static void PushDC(HWND hWnd, HDC hDC,
                       std::vector<PaintProvider>& providers,
                       HMODULE callerMod,
                       DcEntryType type) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire) || !hWnd) return;
        PruneStaleCompatEntries();
        if (!ShouldInvokeProvidersForHwnd(hWnd)) return;
        GlassCore::TextGlow::EnsureTextGlowClientRegistered(hWnd);
        g_dcStack.push_back({ hWnd, hDC, type });
        g_activePaintHWnd = hWnd;
        for (auto& p : providers) p.onBegin(hWnd, callerMod);
    }

    // Снимает DC со стека по паре (HWND, HDC), поиск с конца.
    // Хинт HWND обязателен для пар Begin/End и Get/Release: иначе закрытие одного окна
    // может вырезать запись другого окна при совпадении HDC (переиспользование DC ОС).
    // Для DeleteDC хинта нет (сигнатура без HWND) — поиск только по HDC, как раньше.
    // Уведомляет указанный список провайдеров.
    static void PopDC(HDC hDC,
                      std::vector<PaintProvider>& providers,
                      HMODULE callerMod,
                      HWND hWndHint = nullptr) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire)) return;
        PruneStaleCompatEntries();
        for (auto it = g_dcStack.rbegin(); it != g_dcStack.rend(); ++it)
        {
            if (it->hDC != hDC) continue;
            if (hWndHint && it->hWnd != hWndHint) continue;

            const auto fwd = (it + 1).base();
            const size_t index = static_cast<size_t>(std::distance(g_dcStack.begin(), fwd));
            const DcEntryType type = g_dcStack[index].type;
            HWND hWnd = g_dcStack[index].hWnd;
            if (type == DcEntryType::Paint || type == DcEntryType::GetDC)
                UnwindCompatAbove(index);
            if (index >= g_dcStack.size())
                return;
            g_dcStack.erase(g_dcStack.begin() + static_cast<std::ptrdiff_t>(index));
            RefreshActivePaintHwnd();

            if (type != DcEntryType::Foreign)
            {
                for (auto& p : providers) p.onEnd(hWnd, callerMod);
            }
            return;
        }
    }

    [[nodiscard]] static bool IsPopupOnlyForeignWindow(HWND hWnd) noexcept
    {
        return !g_shutdown.load(std::memory_order_acquire)
            && !GlassCore::Root::IsFullInjectionMode()
            && hWnd
            && !IsPopupOnlyPaintDispatcherKind(GlassShared::GlassGetWindowKind(hWnd));
    }

    // ── Detours хуки ─────────────────────────────────────────────────────────

    static HDC WINAPI HookBeginPaint(HWND hWnd, LPPAINTSTRUCT lpPaint)
    {
        PruneStaleCompatEntries();
        // PopupOnly: чужое окно поверх стеклянной сессии прячет хост (Foreign),
        // провайдеров не будит. Пустой стек — прежний fast-path без записи.
        if (IsPopupOnlyForeignWindow(hWnd))
        {
            if (g_dcStack.empty())
                return g_origBeginPaint(hWnd, lpPaint);
            HDC hdc = g_origBeginPaint(hWnd, lpPaint);
            if (hdc)
                PushForeignBarrier(hWnd, hdc);
            return hdc;
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
            // P1: запись в стек — только для уведомлённых сессий. Иначе EndPaint найдёт
            // чужую запись и разошлёт onEnd провайдерам, которые onBegin не получали
            // («закрытие без открытия», расхождение стеков Root).
            if (notifyProviders)
            {
                g_dcStack.push_back({ hWnd, hdc, DcEntryType::Paint });
                g_activePaintHWnd = hWnd;
            }
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
        if (IsPopupOnlyForeignWindow(hWnd))
        {
            if (lpPaint)
                PopForeignBarrier(hWnd, lpPaint->hdc);
            return g_origEndPaint(hWnd, lpPaint);
        }
        HMODULE callerMod = DetourGetContainingModule(_ReturnAddress());
        if (lpPaint) PopDC(lpPaint->hdc, g_paintProviders, callerMod, hWnd);
        return g_origEndPaint(hWnd, lpPaint);
    }

    static HDC WINAPI HookGetDC(HWND hWnd)
    {
        if (IsPopupOnlyForeignWindow(hWnd))
        {
            PruneStaleCompatEntries();
            HDC hdc = g_origGetDC(hWnd);
            if (hdc && !g_dcStack.empty())
                PushForeignBarrier(hWnd, hdc);
            return hdc;
        }

        HMODULE callerMod = DetourGetContainingModule(_ReturnAddress());
        HDC hdc = g_origGetDC(hWnd);   // сначала оригинал — нужен валидный DC

        if (hdc)
        {
            g_tlCurrentGetDcForProviders = hdc;
            PushDC(hWnd, hdc, g_getDCProviders, callerMod, DcEntryType::GetDC);
            g_tlCurrentGetDcForProviders = nullptr;
        }
        return hdc;
    }

    static HDC WINAPI HookGetDCEx(HWND hWnd, HRGN hrgnClip, DWORD flags)
    {
        if (IsPopupOnlyForeignWindow(hWnd))
        {
            PruneStaleCompatEntries();
            HDC hdc = g_origGetDCEx ? g_origGetDCEx(hWnd, hrgnClip, flags) : nullptr;
            if (hdc && !g_dcStack.empty())
                PushForeignBarrier(hWnd, hdc);
            return hdc;
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
            PushDC(hWnd, hdc, g_getDCProviders, callerMod, DcEntryType::GetDC);
            g_tlCurrentGetDcForProviders = nullptr;
        }
        return hdc;
    }

    static int WINAPI HookReleaseDC(HWND hWnd, HDC hDC)
    {
        if (IsPopupOnlyForeignWindow(hWnd))
        {
            PopForeignBarrier(hWnd, hDC);
            return g_origReleaseDC(hWnd, hDC);
        }
        HMODULE callerMod = DetourGetContainingModule(_ReturnAddress());
        PopDC(hDC, g_getDCProviders, callerMod, hWnd);
        return g_origReleaseDC(hWnd, hDC);
    }

    // CreateCompatibleDC/DeleteDC — только g_compatDCProviders.
    // Button/Static зарегистрированы в g_paintProviders → DeleteDC их не трогает.
    static HDC WINAPI HookCreateCompatibleDC(HDC hdcSrc)
    {
        HMODULE callerMod = DetourGetContainingModule(_ReturnAddress());
        HDC hdc = g_origCreateCompatibleDC(hdcSrc);
        // P3: HWND-источник спрашиваем сначала у ОС (как первая ветка BWM WndList),
        // и только для memory-DC (WindowFromDC==NULL) — у вершины стека.
        // Привязка вслепую к g_activePaintHWnd давала stale-HWND: мем-DC, созданный
        // внутри чужой/завершённой сессии (в т.ч. buffered paint), включал чужие
        // compat-спеки. Стек DC ведём только для уведомлённых (см. P1), чтобы MEMDC
        // из buffered paint отражался в IsPaintStackDcForHwnd для Edit session buffer.
        if (hdc)
        {
            PruneStaleCompatEntries();
            HWND hResolved = hdcSrc ? WindowFromDC(hdcSrc) : nullptr;
            if (!GlassCore::Root::IsFullInjectionMode())
            {
                // Чужой оконный DC не приписываем стеклянному хосту.
                // Fallback — только memory-DC источник и не под Foreign-барьером.
                const bool foreignTop = !g_dcStack.empty()
                    && g_dcStack.back().type == DcEntryType::Foreign;
                if (!hResolved)
                {
                    if (!foreignTop && g_activePaintHWnd)
                        hResolved = g_activePaintHWnd;
                }
                else if (!ShouldInvokeProvidersForHwnd(hResolved))
                {
                    hResolved = nullptr;
                }
            }
            else if ((!hResolved || !ShouldInvokeProvidersForHwnd(hResolved)) && g_activePaintHWnd)
            {
                hResolved = g_activePaintHWnd;
            }
            if (hResolved && ShouldInvokeProvidersForHwnd(hResolved))
            {
                g_dcStack.push_back({ hResolved, hdc, DcEntryType::Compat, IsRootVisible(hResolved) });
                g_activePaintHWnd = hResolved;
                for (auto& p : g_compatDCProviders)
                    p.onBegin(hResolved, callerMod);
            }
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
        (void)Shared::InjectProcessMap::TrySetDiagLifecycle(
            GetCurrentProcessId(),
            Shared::InjectProcessMap::DiagLifecyclePaintDispatcher,
            true);

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
        (void)Shared::InjectProcessMap::TrySetDiagLifecycle(
            GetCurrentProcessId(),
            Shared::InjectProcessMap::DiagLifecyclePaintDispatcher,
            false);
        (void)Shared::InjectProcessMap::TrySetDiagFlag(
            GetCurrentProcessId(),
            Shared::InjectProcessMap::DiagPaintDispatcher,
            true);
    }

    HWND GetPaintSessionHostHwnd() noexcept
    {
        return g_activePaintHWnd;
    }

    bool IsForeignPaintBarrierActive() noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire) || g_dcStack.empty())
            return false;
        return g_dcStack.back().type == DcEntryType::Foreign;
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

    // Tier-1: владелец memory-DC из стека текущего потока (см. PaintDispatcher.h).
    HWND GetStackOwnerForDc(HDC hdc) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire) || !hdc)
            return nullptr;
        for (auto it = g_dcStack.rbegin(); it != g_dcStack.rend(); ++it)
        {
            if (it->hDC != hdc || !it->hWnd)
                continue;
            if (!IsWindow(it->hWnd))
                continue;
            return it->hWnd;
        }
        return nullptr;
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
