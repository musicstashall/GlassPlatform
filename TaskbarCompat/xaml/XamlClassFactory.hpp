#pragma once
#include <atomic>
#include <unknwn.h>

namespace TaskbarCompat
{
    // IClassFactory для CLSID_GlassTaskbarTapSite. Создаёт TaskbarTapSite.
    struct TaskbarTapSiteFactory final : IClassFactory
    {
        TaskbarTapSiteFactory() = default;

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
