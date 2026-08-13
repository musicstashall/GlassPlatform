#include "TaskbarPch.h"
#include <Windows.h>
#include <atomic>
#include <objbase.h>
#include <objidl.h>

#include <DebugLog.h>

#include "xaml/XamlClassFactory.hpp"
#include "xaml/XamlComLifetime.hpp"
#include "xaml/XamlGlassIds.hpp"

namespace TaskbarCompatCom
{
    std::atomic<long> g_objectCount{ 0 };
    std::atomic<long> g_serverLocks{ 0 };
    std::atomic<bool> g_shutdownDone{ false };
}

STDAPI DllCanUnloadNow()
{
    // После корректного TaskbarCompat_Shutdown разрешаем COM выгрузить DLL,
    // даже если оболочка ещё держит IUnknown на TAP-объекте (Unadvise + RestoreAll
    // уже сделаны, дальше всё — нейтральные ноль-вызовы).
    if (TaskbarCompatCom::g_shutdownDone.load(std::memory_order_acquire))
        return S_OK;

    if (TaskbarCompatCom::g_serverLocks.load(std::memory_order_acquire) != 0)
        return S_FALSE;
    if (TaskbarCompatCom::g_objectCount.load(std::memory_order_acquire) != 0)
        return S_FALSE;
    return S_OK;
}

STDAPI DllGetClassObject(_In_ REFCLSID rclsid, _In_ REFIID riid, _Outptr_ LPVOID FAR* ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;

    LPOLESTR clsidStr = nullptr;
    LPOLESTR riidStr = nullptr;
    const HRESULT hrClsidStr = StringFromCLSID(rclsid, &clsidStr);
    const HRESULT hrRiidStr  = StringFromIID(riid, &riidStr);
    DebugLog(
        DBG_IMPORTANT,
        L"[TaskbarCompat][Factory] DllGetClassObject: tid=%u clsid=%s riid=%s",
        static_cast<unsigned>(GetCurrentThreadId()),
        SUCCEEDED(hrClsidStr) && clsidStr ? clsidStr : L"<n/a>",
        SUCCEEDED(hrRiidStr) && riidStr  ? riidStr  : L"<n/a>");

    HRESULT hr = CLASS_E_CLASSNOTAVAILABLE;
    if (IsEqualCLSID(rclsid, CLSID_GlassTaskbarTapSite))
    {
        static TaskbarCompat::TaskbarTapSiteFactory s_factory;
        hr = s_factory.QueryInterface(riid, reinterpret_cast<void**>(ppv));
        DebugLog(
            DBG_IMPORTANT,
            L"[TaskbarCompat][Factory] DllGetClassObject TaskbarTapSite hr=0x%08X",
            static_cast<unsigned>(hr));
    }
    else
    {
        DebugLog(DBG_WARNING,
            L"[TaskbarCompat][Factory] DllGetClassObject CLASS_E_CLASSNOTAVAILABLE");
    }

    if (clsidStr) CoTaskMemFree(clsidStr);
    if (riidStr)  CoTaskMemFree(riidStr);
    return hr;
}
