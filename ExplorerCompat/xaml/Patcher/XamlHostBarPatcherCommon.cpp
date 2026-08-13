#include "ExplorerPch.h"
#include "xaml/Patcher/XamlHostBarPatcher.h"
#include "xaml/Patcher/XamlHostBarPatcherCommon.h"
#include "xaml/Patcher/XamlHostBarPatcherInternals.h"

#include <inspectable.h>
#include <OleAuto.h>
#include <shlwapi.h>
#include <winstring.h>

#include <DebugLog.h>

#include <algorithm>
#include <cwchar>

// ─────────────────────────────────────────────────────────────────────────────
//  Реализация общих VTS3-хелперов и BFS-вспомогательных функций.
//  Используется из XamlHostBarPatcher.cpp (chrome / diag) и
//  XamlHostBarPatcherPopupDwxs.cpp (gate).
// ─────────────────────────────────────────────────────────────────────────────
namespace XamlHostBarPatcher::Detail
{
    void ReleaseDiagnosticPropertyChain(
        PropertyChainSource* sources,
        unsigned sourceCount,
        PropertyChainValue* values,
        unsigned propCount) noexcept
    {
        if (sources)
        {
            for (unsigned i = 0; i < sourceCount; ++i)
            {
                SysFreeString(sources[i].TargetType);
                SysFreeString(sources[i].Name);
            }
            CoTaskMemFree(sources);
        }
        if (values)
        {
            for (unsigned i = 0; i < propCount; ++i)
            {
                SysFreeString(values[i].Type);
                SysFreeString(values[i].DeclaringType);
                SysFreeString(values[i].ValueType);
                SysFreeString(values[i].ItemType);
                SysFreeString(values[i].Value);
                SysFreeString(values[i].PropertyName);
            }
            CoTaskMemFree(values);
        }
    }

    bool WszContainsI(LPCWSTR hay, const wchar_t* needle) noexcept
    {
        return hay && needle && StrStrIW(hay, needle) != nullptr;
    }

    bool TryParseDiagnosticInstanceHandle(BSTR value, hyper metaBits, InstanceHandle* out) noexcept
    {
        *out = 0;
        if (!value || !value[0])
            return false;
        constexpr hyper kIsValueHandle = static_cast<hyper>(MetadataBit::IsValueHandle);
        if ((metaBits & kIsValueHandle) == 0)
            return false;
        unsigned long long u = 0;
        if (swscanf_s(value, L"%llu", &u) == 1 && u != 0)
        {
            *out = static_cast<InstanceHandle>(u);
            return true;
        }
        if (swscanf_s(value, L"0x%llx", &u) == 1 && u != 0)
        {
            *out = static_cast<InstanceHandle>(u);
            return true;
        }
        return false;
    }

    bool TryParseCollectionElementHandleLoose(const CollectionElementValue& el, InstanceHandle* out) noexcept
    {
        *out = 0;
        if (TryParseDiagnosticInstanceHandle(el.Value, el.MetadataBits, out) && *out != 0)
            return true;
        if (!el.Value || !el.Value[0])
            return false;
        unsigned long long u = 0;
        if (swscanf_s(el.Value, L"%llu", &u) != 1 || u < 1000ULL)
            return false;
        *out = static_cast<InstanceHandle>(u);
        return true;
    }

    bool PropertyChainNameSuggestsVisualChild(const wchar_t* fullName) noexcept
    {
        if (!fullName || !fullName[0])
            return false;
        if (StrStrIW(fullName, L".Children")) return true;
        if (StrStrIW(fullName, L".Content")) return true;
        if (StrStrIW(fullName, L"XamlRoot")) return true;
        if (StrStrIW(fullName, L"RootScrollViewer")) return true;
        if (StrStrIW(fullName, L"PopupRoot")) return true;
        if (StrStrIW(fullName, L"DesktopWindowXamlSource")) return true;
        // Border.Child / Decorator.Child — не пересекается с .Children при проверке выше.
        if (StrStrIW(fullName, L".Child")) return true;
        return false;
    }

    void TryAppendNameFromPropertyChain(
        IVisualTreeService* vts,
        InstanceHandle elem,
        std::wstring* outName) noexcept
    {
        outName->clear();
        if (!vts || elem == 0)
            return;
        unsigned sourceCount = 0;
        unsigned propCount = 0;
        PropertyChainSource* sources = nullptr;
        PropertyChainValue* values = nullptr;
        const HRESULT hr = vts->GetPropertyValuesChain(elem, &sourceCount, &sources, &propCount, &values);
        if (FAILED(hr) || !values)
        {
            ReleaseDiagnosticPropertyChain(sources, sourceCount, values, propCount);
            return;
        }
        for (unsigned i = 0; i < propCount; ++i)
        {
            const PropertyChainValue& v = values[i];
            if (!v.PropertyName || !v.Value || !v.Value[0])
                continue;
            if (_wcsicmp(v.PropertyName, L"Name") == 0)
            {
                *outName = v.Value;
                break;
            }
            const size_t len = wcslen(v.PropertyName);
            if (len >= 5 && _wcsicmp(v.PropertyName + len - 5, L".Name") == 0)
            {
                *outName = v.Value;
                break;
            }
        }
        ReleaseDiagnosticPropertyChain(sources, sourceCount, values, propCount);
    }

    void EnqueueChildPropertyHandle(
        IVisualTreeService* vts,
        IXamlDiagnostics* diag,
        InstanceHandle val,
        UINT64 parentU64,
        std::vector<std::pair<InstanceHandle, UINT64>>& queue,
        std::unordered_set<UINT64>& seen,
        unsigned& outEdges) noexcept
    {
        if (!vts || !diag || val == 0)
            return;

        unsigned collCount = 0;
        const HRESULT hrColl = vts->GetCollectionCount(val, &collCount);
        if (SUCCEEDED(hrColl) && collCount > 0 && collCount <= 4096)
        {
            for (unsigned ci = 0; ci < collCount; ++ci)
            {
                unsigned fetched = 1;
                CollectionElementValue* elems = nullptr;
                const HRESULT hrE = vts->GetCollectionElements(val, ci, &fetched, &elems);
                if (FAILED(hrE) || fetched == 0 || !elems)
                    continue;
                for (unsigned j = 0; j < fetched; ++j)
                {
                    InstanceHandle ch = 0;
                    (void)TryParseCollectionElementHandleLoose(elems[j], &ch);
                    SysFreeString(elems[j].ValueType);
                    SysFreeString(elems[j].Value);
                    if (ch == 0)
                        continue;
                    IInspectable* verify = nullptr;
                    if (FAILED(diag->GetIInspectableFromHandle(ch, &verify)) || !verify)
                        continue;
                    verify->Release();
                    const UINT64 chU = static_cast<UINT64>(ch);
                    XamlHostBarPatcher::TrackParent(ch, static_cast<InstanceHandle>(parentU64));
                    ++outEdges;
                    if (seen.insert(chU).second)
                        queue.emplace_back(ch, parentU64);
                }
                CoTaskMemFree(elems);
            }
            return;
        }

        IInspectable* probe = nullptr;
        if (SUCCEEDED(diag->GetIInspectableFromHandle(val, &probe)) && probe)
        {
            probe->Release();
            XamlHostBarPatcher::TrackParent(val, static_cast<InstanceHandle>(parentU64));
            ++outEdges;
            const UINT64 valU = static_cast<UINT64>(val);
            if (seen.insert(valU).second)
                queue.emplace_back(val, parentU64);
        }
    }

    void EnqueueVisualChildrenFromNamedProperty(
        IVisualTreeService2* vts2,
        IVisualTreeService* vts,
        IXamlDiagnostics* diag,
        InstanceHandle parent,
        UINT64 parentU64,
        const wchar_t* propName,
        std::vector<std::pair<InstanceHandle, UINT64>>& queue,
        std::unordered_set<UINT64>& seen,
        unsigned& outEdges) noexcept
    {
        if (!vts2 || !vts || !diag || !propName || parent == 0)
            return;
        unsigned idx = 0;
        if (FAILED(vts2->GetPropertyIndex(parent, propName, &idx)))
            return;
        InstanceHandle val{};
        if (FAILED(vts2->GetProperty(parent, idx, &val)) || val == 0)
            return;
        EnqueueChildPropertyHandle(vts, diag, val, parentU64, queue, seen, outEdges);
    }

    void TryExpandNodeViaPropertyValuesChain(
        IVisualTreeService2* vts2,
        IVisualTreeService* vts,
        IXamlDiagnostics* diag,
        InstanceHandle parent,
        UINT64 parentU64,
        std::vector<std::pair<InstanceHandle, UINT64>>& queue,
        std::unordered_set<UINT64>& seen,
        unsigned& outEdges) noexcept
    {
        if (!vts2 || !vts || !diag || parent == 0)
            return;

        unsigned sourceCount = 0;
        unsigned propCount = 0;
        PropertyChainSource* sources = nullptr;
        PropertyChainValue* values = nullptr;
        const HRESULT hr = vts->GetPropertyValuesChain(parent, &sourceCount, &sources, &propCount, &values);
        if (FAILED(hr) || !values || propCount == 0)
        {
            ReleaseDiagnosticPropertyChain(sources, sourceCount, values, propCount);
            return;
        }

        constexpr unsigned kMaxChainProps = 160;
        const unsigned n = (std::min)(propCount, kMaxChainProps);
        for (unsigned i = 0; i < n; ++i)
        {
            const PropertyChainValue& v = values[i];
            wchar_t fullName[768]{};
            if (v.DeclaringType && v.PropertyName && v.DeclaringType[0] && v.PropertyName[0])
                swprintf_s(fullName, L"%ls.%ls", v.DeclaringType, v.PropertyName);
            else if (v.PropertyName && wcschr(v.PropertyName, L'.'))
                wcscpy_s(fullName, v.PropertyName);
            else
                continue;

            if (!PropertyChainNameSuggestsVisualChild(fullName))
                continue;

            InstanceHandle val{};
            if (FAILED(vts2->GetProperty(parent, v.Index, &val)) || val == 0)
                continue;

            EnqueueChildPropertyHandle(vts, diag, val, parentU64, queue, seen, outEdges);
        }

        ReleaseDiagnosticPropertyChain(sources, sourceCount, values, propCount);
    }
} // namespace XamlHostBarPatcher::Detail
