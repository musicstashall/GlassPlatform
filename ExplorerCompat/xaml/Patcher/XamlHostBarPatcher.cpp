#include "ExplorerPch.h"
#include "xaml/Patcher/XamlHostBarPatcher.h"
#include "xaml/Patcher/XamlHostBarPatcherState.h"
#include "xaml/Patcher/XamlHostBarPatcherInternals.h"
#include "xaml/Patcher/XamlHostBarPatcherCommon.h"
#include "xaml/Patcher/XamlHostBarPatcherPrivate.h"
#include "xaml/XamlShutdownMarshal.hpp"
#include <inspectable.h>
#include <OleAuto.h>
#include <windows.ui.xaml.media.h>
#include <windows.ui.xaml.shapes.h>
#include <windows.ui.xaml.controls.h>
#include <DebugLog.h>

#include <mutex>
#include <unordered_set>
#include <vector>

namespace
{
    void CollectExplorerShellHwndsWithWinUiImpl(std::unordered_set<HWND>& out) noexcept
    {
        EnumWindows(
            [](HWND top, LPARAM lp) -> BOOL {
                DWORD pid = 0;
                if (!GetWindowThreadProcessId(top, &pid) || pid != GetCurrentProcessId())
                    return TRUE;

                wchar_t cls[96]{};
                if (GetClassNameW(top, cls, static_cast<int>(std::size(cls))) <= 0)
                    return TRUE;
                if (wcscmp(cls, L"CabinetWClass") != 0 && wcscmp(cls, L"ExploreWClass") != 0)
                    return TRUE;

                bool hasIsland = false;
                EnumChildWindows(
                    top,
                    [](HWND ch, LPARAM lpIsland) -> BOOL {
                        auto* found = reinterpret_cast<bool*>(lpIsland);
                        wchar_t ccls[160]{};
                        if (GetClassNameW(ch, ccls, static_cast<int>(std::size(ccls))) > 0
                            && XamlHostBarPatcher::Internal::IsExplorerShellIslandChildClass(ccls))
                        {
                            *found = true;
                            return FALSE;
                        }
                        return TRUE;
                    },
                    reinterpret_cast<LPARAM>(&hasIsland));

                if (hasIsland)
                    reinterpret_cast<std::unordered_set<HWND>*>(lp)->insert(top);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&out));
    }
} // namespace

using XamlHostBarPatcher::Detail::WszContainsI;

void XamlHostBarPatcher::NotifyExplorerStartupPulse() noexcept
{
    const ULONGLONG until = GetTickCount64() + kExplorerStartupGateMs;
    g_explorerStartupGateUntilTick.store(until, std::memory_order_release);
    g_startupGateLogPrinted.store(false, std::memory_order_release);
    std::lock_guard lock(g_xamlMutex);
}

bool XamlHostBarPatcher::TypeOrNameHintsExplorerUi(BSTR type, BSTR name) noexcept
{
    static const wchar_t* const kKeys[] = {
        L"Cabinet", L"Explorer", L"FileExplorer", L"Navigation", L"FilePane",
    };
    for (const wchar_t* key : kKeys)
    {
        if (WszContainsI(type, key) || WszContainsI(name, key))
            return true;
    }
    return false;
}

bool XamlHostBarPatcher::ShouldCountVtcAddFilteredSample(BSTR type, BSTR name) noexcept
{
    return !TypeOrNameHintsExplorerUi(type, name);
}

bool XamlHostBarPatcher::MatchesExplorerDiagnosticIsland(BSTR type, BSTR name) noexcept
{
    static const wchar_t* const kNeedles[] = {
        L"FileExplorer_FolderViewIsland",
        L"DesktopChildSiteBridge",
    };
    for (const wchar_t* n : kNeedles)
    {
        if (WszContainsI(type, n) || WszContainsI(name, n))
            return true;
    }
    return false;
}
bool XamlHostBarPatcher::AncestorChainIncludesShellChrome(IXamlDiagnostics* diag, InstanceHandle leaf) noexcept
{
    if (!diag || leaf == 0)
        return true;

    std::vector<UINT64> chain;
    {
        std::lock_guard lock(g_xamlMutex);
        UINT64 cur = static_cast<UINT64>(leaf);
        for (int i = 0; i < 64; ++i)
        {
            chain.push_back(cur);
            const auto it = g_parentOf.find(cur);
            if (it == g_parentOf.end())
                break;
            if (it->second == 0 || it->second == cur)
                break;
            cur = it->second;
        }
    }

    for (const UINT64 h : chain)
    {
        if (Internal::RuntimeClassImpliesShellChromeUi(diag, h))
            return true;
    }
    return false;
}

HRESULT XamlHostBarPatcher::TryDiagnosticBlackenBackground(
    IXamlDiagnostics* diag,
    InstanceHandle handle,
    IInspectable* insp) noexcept
{
    if (!IsExplorerXamlPatchingAllowed())
        return E_FAIL;
    (void)diag;
    if (!insp)
        return E_INVALIDARG;

    ABI::Windows::UI::Xaml::Media::ISolidColorBrush* black = nullptr;
    HRESULT hr = Detail::CreateSolidColorBrushArgb(255, 0, 0, 0, &black);
    if (FAILED(hr) || !black)
        return hr;

    ABI::Windows::UI::Xaml::Media::IBrush* blackAsBrush = nullptr;
    hr = black->QueryInterface(
        __uuidof(ABI::Windows::UI::Xaml::Media::IBrush),
        reinterpret_cast<void**>(&blackAsBrush));
    black->Release();
    if (FAILED(hr) || !blackAsBrush)
        return hr;

    const UINT64 key = static_cast<UINT64>(handle);

    auto tryPanel = [&]() -> HRESULT {
        ABI::Windows::UI::Xaml::Controls::IPanel* panel = nullptr;
        if (FAILED(insp->QueryInterface(__uuidof(ABI::Windows::UI::Xaml::Controls::IPanel), reinterpret_cast<void**>(&panel))) || !panel)
            return E_NOINTERFACE;
        ABI::Windows::UI::Xaml::Media::IBrush* old = nullptr;
        HRESULT hrL = panel->get_Background(&old);
        if (FAILED(hrL)) { panel->Release(); return hrL; }
        hrL = panel->put_Background(blackAsBrush);
        panel->Release();
        if (FAILED(hrL)) { if (old) old->Release(); return hrL; }
        blackAsBrush->Release(); blackAsBrush = nullptr;
        { std::lock_guard lock(g_xamlMutex); g_explorerDiagBackgrounds[key] = ExplorerDiagBgPatch{ old, BgPatchTarget::Panel }; }
        //DebugLog(DBG_IMPORTANT, L"[ExplorerCompat][XamlTree][Diag] зачернён Background (IPanel) handle=%llu", static_cast<unsigned long long>(key));
        return S_OK;
        };
    auto tryControl = [&]() -> HRESULT {
        ABI::Windows::UI::Xaml::Controls::IControl* ctl = nullptr;
        if (FAILED(insp->QueryInterface(__uuidof(ABI::Windows::UI::Xaml::Controls::IControl), reinterpret_cast<void**>(&ctl))) || !ctl)
            return E_NOINTERFACE;
        ABI::Windows::UI::Xaml::Media::IBrush* old = nullptr;
        HRESULT hrL = ctl->get_Background(&old);
        if (FAILED(hrL)) { ctl->Release(); return hrL; }
        hrL = ctl->put_Background(blackAsBrush);
        ctl->Release();
        if (FAILED(hrL)) { if (old) old->Release(); return hrL; }
        blackAsBrush->Release(); blackAsBrush = nullptr;
        { std::lock_guard lock(g_xamlMutex); g_explorerDiagBackgrounds[key] = ExplorerDiagBgPatch{ old, BgPatchTarget::Control }; }
        //DebugLog(DBG_IMPORTANT, L"[ExplorerCompat][XamlTree][Diag] зачернён Background (IControl) handle=%llu", static_cast<unsigned long long>(key));
        return S_OK;
        };
    auto tryBorder = [&]() -> HRESULT {
        ABI::Windows::UI::Xaml::Controls::IBorder* border = nullptr;
        if (FAILED(insp->QueryInterface(__uuidof(ABI::Windows::UI::Xaml::Controls::IBorder), reinterpret_cast<void**>(&border))) || !border)
            return E_NOINTERFACE;
        ABI::Windows::UI::Xaml::Media::IBrush* old = nullptr;
        HRESULT hrL = border->get_Background(&old);
        if (FAILED(hrL)) { border->Release(); return hrL; }
        hrL = border->put_Background(blackAsBrush);
        border->Release();
        if (FAILED(hrL)) { if (old) old->Release(); return hrL; }
        blackAsBrush->Release(); blackAsBrush = nullptr;
        { std::lock_guard lock(g_xamlMutex); g_explorerDiagBackgrounds[key] = ExplorerDiagBgPatch{ old, BgPatchTarget::Border }; }
        DebugLog(DBG_IMPORTANT, L"[ExplorerCompat][XamlTree][Diag] зачернён Background (IBorder) handle=%llu", static_cast<unsigned long long>(key));
        return S_OK;
        };

    if (SUCCEEDED(tryPanel())) return S_OK;
    if (SUCCEEDED(tryControl())) return S_OK;
    if (SUCCEEDED(tryBorder())) return S_OK;

    if (blackAsBrush) blackAsBrush->Release();
    DebugLog(DBG_WARNING, L"[ExplorerCompat][XamlTree][Diag] не удалось QI IPanel/IControl/IBorder для handle=%llu",
        static_cast<unsigned long long>(key));
    return E_NOINTERFACE;
}

void XamlHostBarPatcher::RestoreDiagBackgrounds(IXamlDiagnostics* diag) noexcept
{
    if (!diag)
        return;
    std::unordered_map<UINT64, ExplorerDiagBgPatch> local;
    {
        std::lock_guard lock(g_xamlMutex);
        local.swap(g_explorerDiagBackgrounds);
    }
    for (auto& [handle, patch] : local)
        Internal::RestoreExplorerDiagBackgroundEntry(diag, handle, patch);
}

void XamlHostBarPatcher::TrackParent(InstanceHandle child, InstanceHandle parent) noexcept
{
    if (child != 0 && parent != 0)
    {
        std::lock_guard lock(g_xamlMutex);
        g_parentOf[static_cast<UINT64>(child)] = static_cast<UINT64>(parent);
    }
}

void XamlHostBarPatcher::TrackElementMetadata(VisualElement element) noexcept
{
    if (element.Handle == 0)
        return;
    const UINT64 key = static_cast<UINT64>(element.Handle);
    std::lock_guard lock(g_xamlMutex);
    if (element.Name)
        g_nameOf[key] = element.Name;
    if (element.Type)
        g_typeOf[key] = element.Type;
    else
        g_typeOf.erase(key);
}

void XamlHostBarPatcher::RegisterExplorerShellHwnd(HWND hShell) noexcept
{
    if (!hShell || !IsWindow(hShell))
        return;
    wchar_t cls[96]{};
    if (GetClassNameW(hShell, cls, static_cast<int>(std::size(cls))) <= 0)
        return;
    if (wcscmp(cls, L"CabinetWClass") != 0 && wcscmp(cls, L"ExploreWClass") != 0)
        return;
    std::lock_guard lock(g_xamlMutex);
    g_patchedExplorerShellHwnds.insert(hShell);
}

const std::unordered_set<HWND>& XamlHostBarPatcher::GetPatchedExplorerShellHwnds() noexcept
{
    return g_patchedExplorerShellHwnds;
}

void XamlHostBarPatcher::ClearPatchedExplorerShellHwnds() noexcept
{
    std::lock_guard lock(g_xamlMutex);
    g_patchedExplorerShellHwnds.clear();
}
int XamlHostBarPatcher::ApplyExplorerChrome(
    IXamlDiagnostics* diag,
    ExplorerChromeApplyScope scope,
    const VisualElement* element,
    InstanceHandle parent) noexcept
{
    if (!diag)
        return 0;

    int patched = 0;
    switch (scope)
    {
    case ExplorerChromeApplyScope::Incremental:
        if (!element)
            return 0;
        patched = Internal::ApplyExplorerChromeIncremental(diag, *element, parent);
        break;
    case ExplorerChromeApplyScope::FullReplay:
        patched = Internal::ApplyExplorerChromeFullReplayImpl(diag);
        break;
    default:
        return 0;
    }

    return patched;
}

void XamlHostBarPatcher::RestoreExplorerChrome(
    IXamlDiagnostics* diag,
    ExplorerChromeRestoreScope scope) noexcept
{
    if (!diag)
        return;

    switch (scope)
    {
    case ExplorerChromeRestoreScope::HostBarOnly:
        Internal::RestoreExplorerChromeHostBarOnlyImpl(diag);
        break;
    case ExplorerChromeRestoreScope::Full:
        Internal::RestoreExplorerChromeFullImpl(diag);
        break;
    default:
        break;
    }
}
void XamlHostBarPatcher::OnElementRemoved(IXamlDiagnostics* diag, VisualElement element) noexcept
{
    std::lock_guard lock(g_xamlMutex);
    const UINT64 key = static_cast<UINT64>(element.Handle);
    g_parentOf.erase(key);
    g_nameOf.erase(key);
    g_typeOf.erase(key);
    g_tabRepatchTargets.erase(key);
    for (auto it = g_pendingLazyHostBars.begin(); it != g_pendingLazyHostBars.end();)
    {
        if (it->handle == key)
            it = g_pendingLazyHostBars.erase(it);
        else
            ++it;
    }

    // ЗОНА 3: шапка проводника.
    for (auto it = g_hostBarPropertyPatches.begin(); it != g_hostBarPropertyPatches.end();)
    {
        if (it->first.element == key)
            it = g_hostBarPropertyPatches.erase(it);
        else
            ++it;
    }

    const auto itFill = g_originalFills.find(key);
    if (itFill != g_originalFills.end())
    {
        if (diag)
        {
            IInspectable* insp = nullptr;
            if (SUCCEEDED(diag->GetIInspectableFromHandle(element.Handle, &insp)) && insp)
            {
                ABI::Windows::UI::Xaml::Shapes::IShape* shape = nullptr;
                if (SUCCEEDED(insp->QueryInterface(
                    __uuidof(ABI::Windows::UI::Xaml::Shapes::IShape),
                    reinterpret_cast<void**>(&shape))))
                {
                    shape->put_Fill(itFill->second);
                    shape->Release();
                }
                insp->Release();
            }
        }
        itFill->second->Release();
        g_originalFills.erase(itFill);
        return;
    }

    const auto itDiag = g_explorerDiagBackgrounds.find(key);
    if (itDiag != g_explorerDiagBackgrounds.end())
    {
        const ExplorerDiagBgPatch patch = itDiag->second;
        g_explorerDiagBackgrounds.erase(itDiag);
        Internal::RestoreExplorerDiagBackgroundEntry(diag, key, patch);
        return;
    }

    const auto itBg = g_originalBorderBackgrounds.find(key);
    if (itBg != g_originalBorderBackgrounds.end())
    {
        if (diag)
        {
            IInspectable* insp = nullptr;
            if (SUCCEEDED(diag->GetIInspectableFromHandle(element.Handle, &insp)) && insp)
            {
                ABI::Windows::UI::Xaml::Controls::IBorder* border = nullptr;
                if (SUCCEEDED(insp->QueryInterface(
                    __uuidof(ABI::Windows::UI::Xaml::Controls::IBorder),
                    reinterpret_cast<void**>(&border))))
                {
                    border->put_Background(itBg->second);
                    border->Release();
                }
                insp->Release();
            }
        }
        itBg->second->Release();
        g_originalBorderBackgrounds.erase(itBg);
    }
}
void XamlHostBarPatcher::StopHostBarRepatchTimer() noexcept
{
    HWND hMsg = XamlShutdownMarshal::GetMessageWindowHwnd();
    if (hMsg)
        KillTimer(hMsg, XamlShutdownMarshal::kRepatchTimerId);
    std::lock_guard lock(g_xamlMutex);
    g_repatchCyclesRemaining = 0;
}

void XamlHostBarPatcher::CollectExplorerShellHwndsWithWinUi(std::unordered_set<HWND>& out) noexcept
{
    CollectExplorerShellHwndsWithWinUiImpl(out);
}
// ════════════════════════════════════════════════════════════════════════════
//  Cleanup
// ════════════════════════════════════════════════════════════════════════════

void XamlHostBarPatcher::Cleanup() noexcept
{
    std::lock_guard lock(g_xamlMutex);
    g_parentOf.clear();
    g_nameOf.clear();
    g_typeOf.clear();
    g_originalFills.clear();
    g_originalBorderBackgrounds.clear();
    g_hostBarPropertyPatches.clear();
    for (auto& [k, insp] : g_hostBarOriginalInspectables)
    {
        if (insp)
            insp->Release();
    }
    g_hostBarOriginalInspectables.clear();
    g_tabContainerHandles.clear();
    g_tabRepatchTargets.clear();
    g_explorerDiagBackgrounds.clear();
    g_pendingLazyHostBars.clear();
    g_repatchCyclesRemaining = 0;
    g_pendingRepatchTabElements.store(false, std::memory_order_release);
    g_pendingRestoreHostBarChrome.store(false, std::memory_order_release);
    g_patchedExplorerShellHwnds.clear();
}

bool XamlHostBarPatcher::TryApplyExplorerHostBarChromeOnUiQueueSync(
    IXamlDiagnostics* diag,
    int* outPatched) noexcept
{
    return Internal::TryApplyExplorerHostBarChromeOnUiQueueSyncImpl(diag, outPatched);
}

bool XamlHostBarPatcher::TryRestoreExplorerHostBarChromeOnUiQueueSync(
    IXamlDiagnostics* diag,
    bool* outRestored) noexcept
{
    return Internal::TryRestoreExplorerHostBarChromeOnUiQueueSyncImpl(diag, outRestored);
}
