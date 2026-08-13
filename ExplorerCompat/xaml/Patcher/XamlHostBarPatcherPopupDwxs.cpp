#include "ExplorerPch.h"
#include "xaml/Patcher/XamlHostBarPatcher.h"
#include "xaml/Patcher/XamlHostBarPatcherCommon.h"
#include "xaml/Patcher/XamlHostBarPatcherState.h"

#include <inspectable.h>
#include <winstring.h>

#include <DebugLog.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  DWXS gate: «остров проводника» vs «popup-остров».
//
//  «остров проводника» определяется по содержимому DWXS (Content +
//  поверхностный BFS по runtime-классам / именам), а не по ancestor-цепочке
//  g_parentOf (chrome не предок DWXS в VTC).
// ─────────────────────────────────────────────────────────────────────────────
namespace XamlHostBarPatcher::Detail
{
    namespace
    {
        std::mutex g_pendingDwxsMx;
        std::vector<UINT64> g_pendingDwxs;

        void PushPendingDwxs(UINT64 dwxsU) noexcept
        {
            std::lock_guard lock(g_pendingDwxsMx);
            g_pendingDwxs.push_back(dwxsU);
        }

        [[nodiscard]] bool SnapshotPendingDwxs(std::vector<UINT64>& outCopy) noexcept
        {
            std::lock_guard lock(g_pendingDwxsMx);
            if (g_pendingDwxs.empty())
                return false;
            outCopy = g_pendingDwxs;
            return true;
        }

        void ErasePendingDwxs(UINT64 dwxsU) noexcept
        {
            std::lock_guard lock(g_pendingDwxsMx);
            g_pendingDwxs.erase(
                std::remove(g_pendingDwxs.begin(), g_pendingDwxs.end(), dwxsU),
                g_pendingDwxs.end());
        }

        [[nodiscard]] bool TryGetDwxsContentHandle(
            IVisualTreeService3* vts3, InstanceHandle dwxs, InstanceHandle* outContent) noexcept
        {
            *outContent = 0;
            if (!vts3 || dwxs == 0)
                return false;
            auto* const vts2 = static_cast<IVisualTreeService2*>(vts3);
            static const wchar_t* const kDwxsContent[] = {
                L"Microsoft.UI.Xaml.Hosting.DesktopWindowXamlSource.Content",
                L"Windows.UI.Xaml.Hosting.DesktopWindowXamlSource.Content",
            };
            for (const wchar_t* p : kDwxsContent)
            {
                unsigned propIndex = 0;
                if (FAILED(vts2->GetPropertyIndex(dwxs, p, &propIndex)))
                    continue;
                InstanceHandle v = 0;
                if (SUCCEEDED(vts2->GetProperty(dwxs, propIndex, &v)) && v != 0)
                {
                    *outContent = v;
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool NodeRuntimeOrNameSuggestsFileExplorerDwxs(
            IXamlDiagnostics* diag, IVisualTreeService* vts, InstanceHandle h) noexcept
        {
            if (!diag || !vts || h == 0)
                return false;
            IInspectable* insp = nullptr;
            if (FAILED(diag->GetIInspectableFromHandle(h, &insp)) || !insp)
                return false;
            HSTRING hstr = nullptr;
            bool fromRt = false;
            const HRESULT hr = insp->GetRuntimeClassName(&hstr);
            if (SUCCEEDED(hr) && hstr)
            {
                UINT32 len = 0;
                const wchar_t* raw = WindowsGetStringRawBuffer(hstr, &len);
                if (raw && len > 0)
                {
                    static const wchar_t* const kTypeNeedles[] = {
                        L"FileExplorer", L"FilePane", L"FileExplorerExtensions", L"DetailsPane",
                        L"DetailsView", L"HomeView", L"FolderView",
                    };
                    for (const wchar_t* n : kTypeNeedles)
                    {
                        if (WszContainsI(raw, n))
                        {
                            fromRt = true;
                            break;
                        }
                    }
                }
                WindowsDeleteString(hstr);
            }
            insp->Release();
            if (fromRt)
                return true;

            std::wstring elemName;
            TryAppendNameFromPropertyChain(vts, h, &elemName);
            if (elemName.empty())
                return false;
            static const wchar_t* const kNameNeedles[] = {
                L"NavigationBarControlGrid", L"CommandBarControlRootGrid", L"FileExplorerAddressBarGrid",
                L"TabContainerGrid", L"TabContainer", L"FileExplorerCommandBar", L"DetailsViewControlRootGrid",
                L"HomeViewRootGrid", L"GalleryRootGrid",
            };
            for (const wchar_t* s : kNameNeedles)
            {
                if (_wcsicmp(elemName.c_str(), s) == 0)
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool BfsDwxsContentImpliesFileExplorer(
            IXamlDiagnostics* diag, IVisualTreeService3* vts3, InstanceHandle contentRoot) noexcept
        {
            if (!diag || !vts3 || contentRoot == 0)
                return false;
            auto* const vts2 = static_cast<IVisualTreeService2*>(vts3);
            auto* const vts = static_cast<IVisualTreeService*>(vts3);
            if (NodeRuntimeOrNameSuggestsFileExplorerDwxs(diag, vts, contentRoot))
                return true;

            const DWORD deadline = GetTickCount() + 100;
            std::vector<std::pair<InstanceHandle, UINT64>> q;
            std::unordered_set<UINT64> seen;
            unsigned nodesVisited = 0;
            unsigned edges = 0;
            {
                const UINT64 u = static_cast<UINT64>(contentRoot);
                if (seen.insert(u).second)
                    q.emplace_back(contentRoot, 0u);
            }

            static const wchar_t* const kDwxsBfsChildProps[] = {
                L"Children",
                L"Content",
                L"Child",
                L"Microsoft.UI.Xaml.Controls.Panel.Children",
                L"Microsoft.UI.Xaml.Controls.ContentControl.Content",
                L"Microsoft.UI.Xaml.Controls.Border.Child",
                L"Microsoft.UI.Xaml.Controls.Grid.Children",
                L"Windows.UI.Xaml.Controls.Panel.Children",
                L"Windows.UI.Xaml.Controls.ContentControl.Content",
                L"Windows.UI.Xaml.Controls.Border.Child",
            };

            for (size_t qi = 0; qi < q.size() && nodesVisited < 64 && GetTickCount() < deadline; ++qi)
            {
                const InstanceHandle cur = q[qi].first;
                const UINT64 curU = static_cast<UINT64>(cur);
                ++nodesVisited;
                if (NodeRuntimeOrNameSuggestsFileExplorerDwxs(diag, vts, cur))
                    return true;

                for (const wchar_t* pn : kDwxsBfsChildProps)
                {
                    EnqueueVisualChildrenFromNamedProperty(
                        vts2, vts, diag, cur, curU, pn, q, seen, edges);
                }
                if (nodesVisited <= 16u)
                {
                    TryExpandNodeViaPropertyValuesChain(
                        vts2, vts, diag, cur, curU, q, seen, edges);
                }
            }
            return false;
        }
    } // anonymous namespace

    bool DwxsShouldSkipMarkingAsPopupRoot(
        IXamlDiagnostics* diag, IVisualTreeService3* vts3, InstanceHandle dwxs) noexcept
    {
        InstanceHandle ch = 0;
        if (!TryGetDwxsContentHandle(vts3, dwxs, &ch) || ch == 0)
        {
            // DWXS пришёл без Content — отложить, попробовать позднее.
            PushPendingDwxs(static_cast<UINT64>(dwxs));
            return true;
        }
        if (BfsDwxsContentImpliesFileExplorer(diag, vts3, ch))
            return true;
        return false;
    }

    void ProcessPendingDwxsForPopup(
        IXamlDiagnostics* diag, IVisualTreeService3* vts3) noexcept
    {
        if (!diag || !vts3)
            return;
        if (g_disableHostBarPatching.load(std::memory_order_acquire))
            return;

        std::vector<UINT64> pendingCopy;
        if (!SnapshotPendingDwxs(pendingCopy))
            return;

        for (const UINT64 dwxsU : pendingCopy)
        {
            InstanceHandle ch = 0;
            if (!TryGetDwxsContentHandle(
                    vts3, static_cast<InstanceHandle>(dwxsU), &ch) ||
                ch == 0)
            {
                continue;
            }

            const bool isExplorer = BfsDwxsContentImpliesFileExplorer(diag, vts3, ch);
            ErasePendingDwxs(dwxsU);

            if (isExplorer)
                continue;

            // Popup-island roots через HWND-bridge больше не ведутся.
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][Popup] Отложенный DWXS (не explorer): handle=%llu",
                static_cast<unsigned long long>(dwxsU));
        }
    }
} // namespace XamlHostBarPatcher::Detail
