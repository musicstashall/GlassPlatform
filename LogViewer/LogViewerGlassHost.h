#pragma once
#include <Windows.h>

namespace LogViewerGlass
{
    [[nodiscard]] bool LoadGlassCoreDll() noexcept;

    /// Загрузить WndCompat.dll и WndCompat_Initialize(Full) после GlassCore (как GlassSettings).
    [[nodiscard]] bool EnsureWndCompatRuntime() noexcept;
    [[nodiscard]] bool IsWndCompatLoaded() noexcept;

    using WndCompatHandleWinEventFn = void(WINAPI*)(DWORD event, HWND hWnd, LONG idObject, LONG idChild,
        DWORD dwEventThread, DWORD dwmsEventTime);
    [[nodiscard]] WndCompatHandleWinEventFn GetWndCompatHandleWinEvent() noexcept;

    /// GlassCore_Shutdown, дренаж сообщений, WndCompat_BeginShutdown, FreeLibrary (оба модуля).
    void ShutdownGlassCoreDll() noexcept;

    [[nodiscard]] bool IsGlassCoreLoaded() noexcept;
    [[nodiscard]] HMODULE GetGlassCoreModule() noexcept;

    using GlassCoreApplyDropDownShowFn = void(WINAPI*)(HWND);
    /// GlassCore_ApplyDropDownShow (DropDown эффекты при показе); nullptr если экспорт отсутствует.
    [[nodiscard]] GlassCoreApplyDropDownShowFn GetGlassCoreApplyDropDownShow() noexcept;

    /// Повторно прочитать настройки текста из реестра (throttle — чтобы не дёргать на каждом сообщении).
    void ReloadRenderTextInkFromRegistryThrottled(DWORD minIntervalMs = 500) noexcept;
}
