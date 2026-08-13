#pragma once

#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <xamlOM.h>

#include <winrt/base.h>

#include "config/UiRegistry.h"

struct IXamlDiagnostics;

namespace UICompat
{
    // Jump List SEH/ShellHost: Border#JumpListRestyledAcrylic через IVisualTreeService3.
    // Companion Border#JumpListAcrylic → Transparent (без двойного acrylic).
    // WinRT Background/getters на RestyledAcrylic → AV 0xC0000005.
    class JumpListPanelRegistry
    {
    public:
        enum class PanelKind : unsigned
        {
            Chrome = 0,     // JumpListRestyledAcrylic — полный патч
            Companion = 1,  // JumpListAcrylic — Transparent Background
        };

        JumpListPanelRegistry(
            IXamlDiagnostics* diagnostics,
            Registry::Settings settings) noexcept;
        ~JumpListPanelRegistry() = default;

        JumpListPanelRegistry(const JumpListPanelRegistry&) = delete;
        JumpListPanelRegistry& operator=(const JumpListPanelRegistry&) = delete;

        void RegisterPanel(InstanceHandle handle, PanelKind kind = PanelKind::Chrome) noexcept;

        void RetryUnresolvedPanels() noexcept;
        void Unregister(InstanceHandle handle) noexcept;
        void RestoreAll() noexcept;
        void ReapplyPatchedBrushes() noexcept;
        void RecaptureAfterThemeChange(Registry::Settings settings) noexcept;
        void RefreshBrushesAfterThemeChange(Registry::Settings settings) noexcept;
        void UpdateSettings(Registry::Settings settings) noexcept;
        [[nodiscard]] unsigned PanelCount() const noexcept;
        [[nodiscard]] unsigned UnresolvedCount() const noexcept;

    private:
        struct RegistryState
        {
            std::atomic<bool> shutdown{ false };
        };

        struct OriginalProperty
        {
            unsigned index{ UINT_MAX };
            winrt::com_ptr<::IInspectable> value;
            bool captured{ false };
        };

        struct PanelSlot
        {
            InstanceHandle handle{ 0 };
            DWORD uiTid{ 0 };
            PanelKind kind{ PanelKind::Chrome };
            // 0 Background, 1 BorderBrush, 2 BorderThickness, 3 CornerRadius
            std::array<OriginalProperty, 4> originals{};
            bool chromeCaptured{ false };
            bool chromePatched{ false };
            bool appliedOk{ false };
            bool needsRetry{ false };
            // После visual style: Stop ClearProperty Background (не SetProperty stale).
            bool clearBackgroundOnRestore{ false };
            ULONGLONG firstSeenTick{ 0 };

            void ClearWinrtRefs() noexcept
            {
                for (auto& o : originals)
                    o.value = nullptr;
            }

            void ResetChromeCapture() noexcept
            {
                ClearWinrtRefs();
                for (auto& o : originals)
                {
                    o.index = UINT_MAX;
                    o.captured = false;
                }
                chromeCaptured = false;
                chromePatched = false;
            }
        };

        struct MarshalSlotJob
        {
            JumpListPanelRegistry* self{ nullptr };
            std::shared_ptr<PanelSlot> slot;
            unsigned restored{ 0 };
        };

        void CaptureChrome(PanelSlot& slot) noexcept;
        [[nodiscard]] unsigned RestoreChrome(PanelSlot& slot) noexcept;
        void ApplyPanelVisualsOnCurrentThread(std::shared_ptr<PanelSlot> slot) noexcept;
        void ApplyCompanionOnCurrentThread(std::shared_ptr<PanelSlot> slot) noexcept;
        void ApplyPanelVisuals(std::shared_ptr<PanelSlot> slot) noexcept;
        [[nodiscard]] bool SetPropByIndex(
            InstanceHandle element,
            unsigned propIndex,
            ::IInspectable* value) noexcept;
        void RestoreSlotOnUiThread(std::shared_ptr<PanelSlot> slot) noexcept;
        void RefreshUnresolvedCount() noexcept;
        static void RestoreMarshalThunk(void* param) noexcept;
        static void ApplyMarshalThunk(void* param) noexcept;

        Registry::Settings m_settings;
        std::shared_ptr<RegistryState> m_state{ std::make_shared<RegistryState>() };
        winrt::com_ptr<IXamlDiagnostics> m_diagnostics;
        winrt::com_ptr<IVisualTreeService3> m_vts3;
        mutable std::mutex m_mx;
        std::map<InstanceHandle, std::shared_ptr<PanelSlot>> m_panels;
        std::atomic<unsigned> m_unresolved{ 0 };
    };
}
