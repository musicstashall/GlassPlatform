#pragma once
#include <atomic>
#include <unknwn.h>
#include <objidl.h>

#include "config/UiRegistry.h"

namespace UICompat
{
    class UiVisualTreeWatcher;

    struct UiTapSite final : IObjectWithSite
    {
        UiTapSite() noexcept;

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) noexcept override;
        ULONG STDMETHODCALLTYPE AddRef() noexcept override;
        ULONG STDMETHODCALLTYPE Release() noexcept override;

        HRESULT STDMETHODCALLTYPE SetSite(IUnknown* pUnkSite) noexcept override;
        HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** ppvSite) noexcept override;

        HRESULT SetSiteImpl(IUnknown* pUnkSite) noexcept;

        static void ShutdownActiveWatcher() noexcept;
        static void ReloadSettingsAndReapply() noexcept;
        /// Visual style: force re-brush без ClearValue (отдельно от ReloadSettingsAndReapply).
        static void RefreshBrushesAfterThemeChange() noexcept;
        static void SetPendingSettings(Registry::Settings settings) noexcept;
        // Создать/пересоздать HWND_MESSAGE на текущем потоке (вызывать с UI/VTC).
        static void EnsureMarshalWindowOnUiThread() noexcept;
        // Post (не Send) reapply на marshal HWND — бэкдроп QS при SHOW без блокировки.
        static void PostReapplyIfActive() noexcept;
        // Очередь prepare ShellSurface-панелей на marshal (после Advise / поздний VTC).
        static void PostShellPrepareIfActive() noexcept;
        // CatchUp NC на текущем UI-потоке (CREATE CoreWindow после IXDE / toast steal).
        static void CatchUpNcOnCurrentThread() noexcept;
        // Sync reapply на текущем UI-потоке hwnd (не Post на чужой marshal tid).
        static void ReapplyOnCurrentThread() noexcept;

    private:
        ~UiTapSite() = default;

        std::atomic<ULONG> m_ref{ 1 };
        IUnknown* m_site{ nullptr };
    };
}
