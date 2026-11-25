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
    // Честный COM-контракт: пока оболочка/XAML держит ссылку на любой наш объект
    // (TAP или VTC-watcher), образ обязан оставаться mapped. g_shutdownDone больше
    // не форсирует S_OK: «нейтральные ноль-вызовы» в unmapped образе невозможны —
    // при неудавшемся UnadviseVisualTreeChange LVT продолжает держать watcher, и
    // следующий SignalMutation зовёт его vtable (дамп explorer.exe.11524:
    // AddElementToLVT+0x2f6, чтение [rax+18h] в <Unloaded_TaskbarCompat.dll>).
    // Остаток модуля после Stop лучше AV — см. anyModuleKept в GlassCompat.
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
