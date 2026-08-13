#pragma once
#include <atomic>
#include <unknwn.h>

namespace UICompat
{
    // IClassFactory для CLSID_GlassUICompatTapSite. Создаёт UiTapSite.
    struct UiTapSiteFactory final : IClassFactory
    {
        UiTapSiteFactory() = default;

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) noexcept override;
        ULONG STDMETHODCALLTYPE AddRef() noexcept override;
        ULONG STDMETHODCALLTYPE Release() noexcept override;

        HRESULT STDMETHODCALLTYPE CreateInstance(
            IUnknown* pUnkOuter, REFIID riid, void** ppvObject) noexcept override;
        HRESULT STDMETHODCALLTYPE LockServer(BOOL fLock) noexcept override;

    private:
        std::atomic<ULONG> m_ref{ 1 };
    };
}
