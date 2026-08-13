#pragma once
#include <atomic>
#include <cstdint>
#include <unknwn.h>
#include <objidl.h>
#include "config/TaskbarRegistry.h"

namespace TaskbarCompat
{
    class TaskbarVisualTreeWatcher;

    // IObjectWithSite для UWP XAML Diagnostics.
    // Один TapSite на процесс (CLSID_GlassTaskbarTapSite). При SetSite создаём
    // TaskbarVisualTreeWatcher и подписываемся IVisualTreeService3::AdviseVisualTreeChange.
    //
    // Watcher хранится как член TapSite (без global g_uwpTreeWatcher), но также
    // публикуется через статический указатель, чтобы TaskbarCompat_Shutdown мог
    // снять подписку без хождения через factory (паттерн из TranslucentTB::TAPSite).
    struct TaskbarTapSite final : IObjectWithSite
    {
        TaskbarTapSite() noexcept;

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) noexcept override;
        ULONG STDMETHODCALLTYPE AddRef() noexcept override;
        ULONG STDMETHODCALLTYPE Release() noexcept override;

        HRESULT STDMETHODCALLTYPE SetSite(IUnknown* pUnkSite) noexcept override;
        HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** ppvSite) noexcept override;

        // Тело SetSite без SEH (C++ объекты); снаружи — фильтр AV.
        HRESULT SetSiteImpl(IUnknown* pUnkSite) noexcept;

        // Глобальный shutdown активной сессии — снимает Advise и восстанавливает Fill.
        // Вызывается из TaskbarCompat_Shutdown (см. TaskbarCompat.cpp).
        static void ShutdownActiveWatcher() noexcept;

        // Переутверждение BackgroundFill/Stroke после SWCA ApplyEffect на Shell_*Tray*.
        static void ReapplyActiveWatcherXamlBackgroundPatches() noexcept;

        // Перечитать Tray\ из реестра и переутвердить патчи активного watcher.
        static void ReloadSettingsAndReapply() noexcept;

        // То же, но всегда асинхронно (PostMessage на marshal-окно), даже если вызов
        // пришёл с самого marshal-потока. Обязателен для вызовов из hook-колбэков
        // (WH_GETMESSAGE): синхронная мутация XAML внутри KiUserCallbackDispatcher
        // реэнтрантно входит в XAML core и рушит его CValue (краш explorer 13.08).
        static void RequestReloadSettingsAndReapply() noexcept;

        // Reassert 12×4s при OG (Task Scheduler или OpenGlass.dll в dwm.exe).
        static void StartOgPatchReassertIfNeeded() noexcept;

        // Передаёт настройки реестра в watcher до InitializeXamlDiagnosticsEx.
        // Подхватывается в SetSite на потоке IXDE.
        static void SetPendingSettings(Registry::Settings settings) noexcept;

    private:
        ~TaskbarTapSite() = default;

        std::atomic<ULONG> m_ref{ 1 };
        IUnknown* m_site{ nullptr };
    };

    // PostMessage на HWND_MESSAGE marshal (STA SetSite) — колбэк VTC с чужого потока.
    [[nodiscard]] bool TaskbarTapSite_PostVtcRectangleAdd(
        TaskbarVisualTreeWatcher* watcher,
        std::uint64_t parentHandle,
        std::uint64_t elementHandle,
        bool isBackgroundFill) noexcept;

}
