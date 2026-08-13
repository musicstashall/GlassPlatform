#pragma once

#include "xaml/Patcher/XamlHostBarPatcherState.h"

#include <windows.h>
#include <windows.system.h>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct IXamlDiagnostics;
struct IVisualTreeService;
struct VisualElement;

namespace XamlHostBarPatcher::Internal
{
    inline constexpr GUID IID_IMicrosoftUIDispatcherQueue{
        0xf6ebf8fa,
        0xbe1c,
        0x5bf6,
        { 0xa4, 0x67, 0x73, 0xda, 0x28, 0x73, 0x8a, 0xe8 }
    };
    inline constexpr GUID IID_IMicrosoftUIDispatcherQueueHandler{
        0x2e0872a9,
        0x4e29,
        0x5f14,
        { 0xb6, 0x88, 0xfb, 0x96, 0xd5, 0xf9, 0xd5, 0xf8 }
    };

    extern std::atomic<bool> g_loggedPatchSkipAlreadyInRegistry;

    /// Refcount для agile IDispatcherQueueHandler (AddRef/Release с разных потоков).
    struct AgileDqHandlerRef final
    {
        std::atomic<ULONG> value{ 1 };

        ULONG AddRef() noexcept
        {
            return value.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        [[nodiscard]] ULONG Release() noexcept
        {
            return value.fetch_sub(1, std::memory_order_acq_rel) - 1;
        }
    };

    /// Дублированный event: callback держит storage, waiter закрывает только свой HANDLE.
    struct DqSyncEvent final
    {
        struct Storage final
        {
            HANDLE handle{};

            Storage() noexcept
            {
                handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            }

            ~Storage() noexcept
            {
                if (handle)
                {
                    CloseHandle(handle);
                    handle = nullptr;
                }
            }
        };

        std::shared_ptr<Storage> storage;
        HANDLE waitHandle{};

        [[nodiscard]] static std::shared_ptr<DqSyncEvent> Create() noexcept
        {
            auto storageObj = std::make_shared<Storage>();
            if (!storageObj->handle)
                return nullptr;

            HANDLE dup = nullptr;
            if (!DuplicateHandle(
                    GetCurrentProcess(),
                    storageObj->handle,
                    GetCurrentProcess(),
                    &dup,
                    0,
                    FALSE,
                    DUPLICATE_SAME_ACCESS))
            {
                return nullptr;
            }

            auto self = std::shared_ptr<DqSyncEvent>(new (std::nothrow) DqSyncEvent());
            if (!self)
            {
                CloseHandle(dup);
                return nullptr;
            }

            self->storage = std::move(storageObj);
            self->waitHandle = dup;
            return self;
        }

        [[nodiscard]] HANDLE CallbackHandle() const noexcept
        {
            return storage ? storage->handle : nullptr;
        }

        [[nodiscard]] HANDLE WaitHandle() const noexcept
        {
            return waitHandle;
        }

        void CloseWaitHandle() noexcept
        {
            if (waitHandle)
            {
                CloseHandle(waitHandle);
                waitHandle = nullptr;
            }
        }

        void Signal() const noexcept
        {
            if (storage && storage->handle)
                SetEvent(storage->handle);
        }

        ~DqSyncEvent() noexcept
        {
            CloseWaitHandle();
        }

    private:
        DqSyncEvent() = default;
    };

    struct FullReplaySnapshot
    {
        std::unordered_map<UINT64, std::wstring> nameOf;
        std::unordered_map<UINT64, std::wstring> typeOf;
        std::unordered_map<UINT64, UINT64> parentOf;
        std::unordered_map<UINT64, std::vector<UINT64>> childrenOf;
        std::vector<UINT64> dwxsRoots;
    };

    [[nodiscard]] InstanceHandle CreateBoxedDouble(IXamlDiagnostics* diag, double value) noexcept;

    [[nodiscard]] bool RuntimeClassImpliesShellChromeUi(IXamlDiagnostics* diag, UINT64 handle) noexcept;
    [[nodiscard]] bool IsExplorerShellIslandChildClass(LPCWSTR cls) noexcept;
    [[nodiscard]] bool IsUnderKnownToolbarChrome(UINT64 handle) noexcept;
    [[nodiscard]] bool IsUnderExplorerAddressRibbonChrome(UINT64 elem) noexcept;
    [[nodiscard]] bool IsInsideTabContainer(UINT64 handle) noexcept;
    [[nodiscard]] bool IsNamedToolbarLayoutAnchor(UINT64 elem) noexcept;
    [[nodiscard]] bool IsExcludedAnchorType(std::wstring_view typeStr) noexcept;

    [[nodiscard]] bool TryGetDispatcherQueueFromDependencyObject(
        IXamlDiagnostics* diag,
        InstanceHandle elem,
        ABI::Windows::System::IDispatcherQueue** outDq) noexcept;

    [[nodiscard]] bool WaitForDispatcherQueueEvent(HANDLE ev, DWORD maxWaitMs) noexcept;

    [[nodiscard]] bool WaitForDqSyncEvent(const std::shared_ptr<DqSyncEvent>& sync, DWORD maxWaitMs) noexcept;

    [[nodiscard]] bool IsElementHandleLive(IXamlDiagnostics* diag, InstanceHandle elem) noexcept;

    void EnsureLazyHostBarRepatchTimer() noexcept;
    void ProcessPendingLazyHostBars(IXamlDiagnostics* diag) noexcept;

    [[nodiscard]] int TryPatchFileExplorerHostBars(IXamlDiagnostics* diag, VisualElement element) noexcept;

    void RestoreHostBarPropertyPatches(IXamlDiagnostics* diag) noexcept;
    void RestoreExplorerDiagBackgroundEntry(
        IXamlDiagnostics* diag,
        UINT64 handle,
        const ExplorerDiagBgPatch& patch) noexcept;

    [[nodiscard]] bool TryApplyExplorerHostBarChromeOnUiQueueSyncImpl(
        IXamlDiagnostics* diag,
        int* outPatched) noexcept;

    [[nodiscard]] bool TryRestoreExplorerHostBarChromeOnUiQueueSyncImpl(
        IXamlDiagnostics* diag,
        bool* outRestored) noexcept;

    [[nodiscard]] int ApplyExplorerChromeFullReplayImpl(IXamlDiagnostics* diag) noexcept;

    [[nodiscard]] int ApplyExplorerChromeIncremental(
        IXamlDiagnostics* diag,
        VisualElement element,
        InstanceHandle parentHandle) noexcept;

    void RestoreExplorerChromeHostBarOnlyImpl(IXamlDiagnostics* diag) noexcept;
    void RestoreExplorerChromeFullImpl(IXamlDiagnostics* diag) noexcept;

    [[nodiscard]] int ApplyExplorerChromeFullReplayFromRoot(
        IXamlDiagnostics* diag,
        UINT64 rootU,
        const FullReplaySnapshot& snap,
        IVisualTreeService* vts,
        size_t* outNodes) noexcept;

    void FreeVisualElementStrings(VisualElement& ve) noexcept;
}
