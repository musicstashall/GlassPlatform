#pragma once
#include <windows.h>

namespace GlassCore::PaintDispatcher
{
    using PaintBeginFn = void(*)(HWND hWnd, HMODULE callerModule);
    using PaintEndFn   = void(*)(HWND hWnd, HMODULE callerModule);

    struct PaintProvider { PaintBeginFn onBegin; PaintEndFn onEnd; };

    // После успешного вызова оригинального BeginPaint (валидный hdc и заполненный PAINTSTRUCT).
    // Не вызывается, если BeginPaint вернул NULL.
    // Возврат true — провайдер очистил фон в ps->rcPaint (или эквивалент); диспетчер выставит ps->fErase = 0.
    using PostPaintAfterBeginFn = bool(*)(
        HWND hWnd, HDC hdc, LPPAINTSTRUCT ps, HMODULE callerModule) noexcept;

    struct PostPaintProvider { PostPaintAfterBeginFn onAfterBeginPaint; };

    // Четыре независимых типа провайдеров.
    // Каждый контрол регистрирует только те, которые ему реально нужны:
    //
    //   RegisterPaintProvider       → pre-BeginPaint / EndPaint: onBegin ДО оригинала BeginPaint,
    //                                 onEnd на EndPaint. Button, Static, GroupBox; NetUI — IAT UIRibbon.
    //
    //   RegisterPostPaintProvider   → только после успешного BeginPaint: onAfterBeginPaint → bool.
    //                                 true: фон очищен → диспетчер сбросит ps->fErase. Сейчас — UiRibbon (NetUI).
    //
    //   RegisterGetDCProvider       → GetDC / GetDCEx / ReleaseDC
    //
    //   RegisterCompatDCProvider    → CreateCompatibleDC / DeleteDC
    //
    // Разделение гарантирует, что внутренние DeleteDC от uxtheme::EndBufferedPaint
    // (внутри DrawThemeContent) не достигают Button/Static провайдеров и не
    // отключают их IAT-хуки в середине animation-фрейма.
    //
    // ── Контракт paint-провайдеров (RegisterPaintProvider) ───────────────────
    //
    // 1) Провайдеры, включающие IAT через HookDispatcher::EnableHook, обязаны
    //    вызывать enable на каждый onBegin и disable на каждый onEnd для той же
    //    пары. Вложенные Begin/End обрабатывает refcount в HookDispatcher (как в BWM).
    //
    // 2) Для disable нужен тот же базовый модуль, что для enable в этой паре.
    //    Рекомендуется thread_local стек HMODULE (LIFO), зеркальный стеку DC здесь.
    //
    // 3) Колбэки onBegin/onEnd в GUI-потоке вызываются в порядке LIFO, согласованном
    //    со стеком BeginPaint/EndPaint. HWND с отрисовкой через BeginPaint принадлежит
    //    одному потоку-владельцу (обычное правило Win32).
    //
    // 4) Если BeginPaint вернул NULL, диспетчер вызывает onEnd сразу после onBegin
    //    (компенсация); провайдеры с ранним return в onBegin должны no-op в onEnd.
    //
    // ── Контракт post-провайдеров (RegisterPostPaintProvider) ────────────────
    //
    // onAfterBeginPaint возвращает true, только если фон в области ps->rcPaint фактически
    // очищен; диспетчер один раз выставит ps->fErase = 0, если хотя бы один провайдер
    // вернул true.
    //
    void RegisterPaintProvider       (PaintProvider provider);
    void RegisterPostPaintProvider   (PostPaintProvider provider);
    void RegisterGetDCProvider       (PaintProvider provider);
    void RegisterCompatDCProvider    (PaintProvider provider);

    void Startup();
    void Shutdown();

    // Вершина стека BeginPaint / GetDC / CreateCompatibleDC в текущем потоке (последний «хозяин» DC).
    // У memory-DC WindowFromDC часто NULL — для классификации MMC-графика в RootGdiHooks.
    [[nodiscard]] HWND GetPaintSessionHostHwnd() noexcept;

    // Только внутри onBegin у провайдеров GetDC/GetDCEx: HDC уже получен диспетчером (g_origGetDC).
    // Не вызывать повторно GetDC(hWnd) для того же окна — иначе рекурсия через HookGetDC.
    [[nodiscard]] HDC GetCurrentGetDcSessionHdc() noexcept;

    // Ближайший в стеке сессии оконный DC для hwnd (не OBJ_MEMDC, WindowFromDC == hwnd).
    // Нужен, когда GDI-вызовы идут в совместимый DC, а Flush/снимок — на реальный клиент.
    [[nodiscard]] HDC GetPaintSessionBaseWindowDc(HWND hWnd) noexcept;

    // Текущий стек BeginPaint/GetDC/MEMDC: запись (hWnd, hdc) добавляется при EndPaint/GetDC и CreateCompatibleDC.
    // У MEMDC WindowFromDC часто NULL — для сессионного буфера Edit сверяем hdc с этим стеком.
    [[nodiscard]] bool IsPaintStackDcForHwnd(HWND hWnd, HDC hdc) noexcept;

    // Диагностика и purge при menu session (plain Edit menu wipe v3).
    void LogPaintDcStackSnapshot(const wchar_t* site, size_t maxEntries) noexcept;
    void InvalidateStaleDcStackEntries() noexcept;
    void PurgePaintDcStackBeforeMenuAttach(HWND menuHwnd) noexcept;
    void PurgePaintDcStackAfterMenuDetach() noexcept;

    // UAH phase A: hdc был в стеке с owner ≠ popup menu.
    [[nodiscard]] bool WasDcRegisteredToNonMenuOwner(HDC hdc, HWND menuHwnd) noexcept;
}
