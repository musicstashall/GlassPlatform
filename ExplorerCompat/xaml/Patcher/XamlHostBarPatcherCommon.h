#pragma once

#include "ExplorerPch.h"

#include <xamlOM.h>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

struct IXamlDiagnostics;

// ─────────────────────────────────────────────────────────────────────────────
//  Общие хелперы патчера XAML, используемые из нескольких TU:
//   • XamlHostBarPatcher.cpp                  — chrome / FolderView Diag
//   • XamlHostBarPatcherPopupDwxs.cpp         — popup-vs-explorer DWXS gate
// ─────────────────────────────────────────────────────────────────────────────
namespace XamlHostBarPatcher::Detail
{
    // Подстрока без учёта регистра (StrStrIW обёртка с проверкой на nullptr).
    [[nodiscard]] bool WszContainsI(LPCWSTR hay, const wchar_t* needle) noexcept;

    // Разбор Value из PropertyChainValue / CollectionElementValue в InstanceHandle.
    [[nodiscard]] bool TryParseDiagnosticInstanceHandle(
        BSTR value, hyper metaBits, InstanceHandle* out) noexcept;
    [[nodiscard]] bool TryParseCollectionElementHandleLoose(
        const CollectionElementValue& el, InstanceHandle* out) noexcept;

    // Эвристика: имя свойства похоже на «дочерний визуал» (Children / Content / Child / …).
    [[nodiscard]] bool PropertyChainNameSuggestsVisualChild(const wchar_t* fullName) noexcept;

    // Достать «обычное» Name элемента из PropertyValuesChain.
    void TryAppendNameFromPropertyChain(
        IVisualTreeService* vts,
        InstanceHandle elem,
        std::wstring* outName) noexcept;

    // Очередь визуального обхода: добавить детей по имени свойства / по цепочке свойств.
    void EnqueueChildPropertyHandle(
        IVisualTreeService* vts,
        IXamlDiagnostics* diag,
        InstanceHandle val,
        UINT64 parentU64,
        std::vector<std::pair<InstanceHandle, UINT64>>& queue,
        std::unordered_set<UINT64>& seen,
        unsigned& outEdges) noexcept;

    void EnqueueVisualChildrenFromNamedProperty(
        IVisualTreeService2* vts2,
        IVisualTreeService* vts,
        IXamlDiagnostics* diag,
        InstanceHandle parent,
        UINT64 parentU64,
        const wchar_t* propName,
        std::vector<std::pair<InstanceHandle, UINT64>>& queue,
        std::unordered_set<UINT64>& seen,
        unsigned& outEdges) noexcept;

    void TryExpandNodeViaPropertyValuesChain(
        IVisualTreeService2* vts2,
        IVisualTreeService* vts,
        IXamlDiagnostics* diag,
        InstanceHandle parent,
        UINT64 parentU64,
        std::vector<std::pair<InstanceHandle, UINT64>>& queue,
        std::unordered_set<UINT64>& seen,
        unsigned& outEdges) noexcept;

    // ─── DWXS gate (XamlHostBarPatcherPopupDwxs.cpp) ──────────────────────

    // true, если данный DWXS — content относится к острову проводника (не popup).
    // Дополнительно: при отсутствии Content откладывает узел в очередь до прихода Content.
    [[nodiscard]] bool DwxsShouldSkipMarkingAsPopupRoot(
        IXamlDiagnostics* diag,
        IVisualTreeService3* vts3,
        InstanceHandle dwxs) noexcept;

    // Дообработка отложенных DWXS, которые не имели Content при первом visit.
    void ProcessPendingDwxsForPopup(
        IXamlDiagnostics* diag,
        IVisualTreeService3* vts3) noexcept;
}
