#include "SharedPch.h"
#include <Helpers/DwmGlassOpacity.h>

#include <atomic>

#ifndef WM_DWMCOLORIZATIONCHANGED
#define WM_DWMCOLORIZATIONCHANGED 0x0320
#endif

namespace
{
    std::atomic<int>     g_dwmColorizationBatchDepth{0};
    std::atomic<bool>    g_dwmColorizationNotifyPending{false};
    std::atomic<uint32_t> g_colorizationWaveId{0};

    void PostDwmColorizationBroadcast() noexcept
    {
        uint32_t wave = g_colorizationWaveId.fetch_add(1u, std::memory_order_acq_rel) + 1u;
        if (wave == 0u)
            wave = g_colorizationWaveId.fetch_add(1u, std::memory_order_acq_rel) + 1u;
        (void)::PostMessageW(
            HWND_BROADCAST,
            WM_DWMCOLORIZATIONCHANGED,
            static_cast<WPARAM>(wave),
            0);
    }
} // namespace

namespace Shared::DwmGlassOpacity
{
    namespace
    {
        constexpr wchar_t kDwmKey[] = L"Software\\Microsoft\\Windows\\DWM";
        constexpr wchar_t kGlassOpacity[] = L"GlassOpacity";
        constexpr wchar_t kGlassOpacityInactive[] = L"GlassOpacityInactive";
        constexpr std::uint32_t kDefPercent = 63;

        [[nodiscard]] std::uint32_t ClampPercent(std::uint32_t p) noexcept
        {
            if (p > 100)
                return 100;
            return p;
        }

        [[nodiscard]] std::uint32_t AlphaByteFromPercent0To100(std::uint32_t percent0_100) noexcept
        {
            const std::uint32_t p = ClampPercent(percent0_100);
            return (p * 255u + 50u) / 100u;
        }

        [[nodiscard]] bool IsWindowRootEqualForegroundRoot(HWND hWnd) noexcept
        {
            if (!hWnd || !IsWindow(hWnd))
                return false;
            HWND fg = GetForegroundWindow();
            if (!fg)
                return false;
            HWND rWnd = GetAncestor(hWnd, GA_ROOT);
            HWND rFg = GetAncestor(fg, GA_ROOT);
            return rWnd != nullptr && rWnd == rFg;
        }

        [[nodiscard]] bool OpenOrCreateDwmKeyForWrite(HKEY& outKey) noexcept
        {
            outKey = nullptr;
            if (RegCreateKeyExW(
                    HKEY_CURRENT_USER,
                    kDwmKey,
                    0,
                    nullptr,
                    0,
                    KEY_SET_VALUE,
                    nullptr,
                    &outKey,
                    nullptr) != ERROR_SUCCESS)
            {
                return false;
            }
            return true;
        }
    } // namespace

    void NotifyDwmColorizationChanged() noexcept
    {
        if (g_dwmColorizationBatchDepth.load(std::memory_order_acquire) > 0)
        {
            g_dwmColorizationNotifyPending.store(true, std::memory_order_release);
            return;
        }
        PostDwmColorizationBroadcast();
    }

    bool IsGeneratedDwmColorizationWave(
        WPARAM colorizationWave,
        LPARAM messageLParam) noexcept
    {
        if (messageLParam != 0 || colorizationWave == 0)
            return false;

        const auto rawWave = static_cast<std::uint64_t>(colorizationWave);
        if (rawWave > UINT32_MAX)
            return false;

        const std::uint32_t generatedThrough =
            g_colorizationWaveId.load(std::memory_order_acquire);
        return generatedThrough != 0
            && static_cast<std::uint32_t>(rawWave) <= generatedThrough;
    }

    void BeginDwmColorizationNotifyBatch() noexcept
    {
        g_dwmColorizationBatchDepth.fetch_add(1, std::memory_order_acq_rel);
    }

    void EndDwmColorizationNotifyBatch() noexcept
    {
        const int prev = g_dwmColorizationBatchDepth.fetch_sub(1, std::memory_order_acq_rel);
        if (prev <= 0)
            return;
        if (prev > 1)
            return;
        if (g_dwmColorizationNotifyPending.exchange(false, std::memory_order_acq_rel))
            PostDwmColorizationBroadcast();
    }

    std::uint32_t ReadGlassOpacityPercent0To100() noexcept
    {
        DWORD type = 0;
        DWORD data = kDefPercent;
        DWORD size = sizeof(data);
        if (RegGetValueW(HKEY_CURRENT_USER, kDwmKey, kGlassOpacity, RRF_RT_REG_DWORD, &type, &data, &size) !=
            ERROR_SUCCESS)
            return kDefPercent;
        return ClampPercent(static_cast<std::uint32_t>(data));
    }

    bool TryReadGlassOpacityRegistryValueIfPresent(std::uint32_t& out0_100) noexcept
    {
        DWORD type = 0;
        DWORD data = 0;
        DWORD size = sizeof(data);
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                kDwmKey,
                kGlassOpacity,
                RRF_RT_REG_DWORD,
                &type,
                &data,
                &size) != ERROR_SUCCESS)
            return false;
        if (type != REG_DWORD)
            return false;
        out0_100 = ClampPercent(static_cast<std::uint32_t>(data));
        return true;
    }

    bool TryReadGlassOpacityInactivePercent0To100(std::uint32_t& out0_100) noexcept
    {
        DWORD type = 0;
        DWORD data = 0;
        DWORD size = sizeof(data);
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                kDwmKey,
                kGlassOpacityInactive,
                RRF_RT_REG_DWORD,
                &type,
                &data,
                &size) != ERROR_SUCCESS)
            return false;
        out0_100 = ClampPercent(static_cast<std::uint32_t>(data));
        return true;
    }

    std::uint32_t MergeGlassOpacityIntoArgb(std::uint32_t argb, HWND hWnd) noexcept
    {
        std::uint32_t percent = ReadGlassOpacityPercent0To100();
        if (!IsWindowRootEqualForegroundRoot(hWnd))
        {
            std::uint32_t inact{};
            if (TryReadGlassOpacityInactivePercent0To100(inact))
                percent = inact;
        }
        const std::uint32_t a = AlphaByteFromPercent0To100(percent);
        return (argb & 0x00FFFFFFu) | (a << 24);
    }

    void WriteGlassOpacityPercent0To100(int percent0_100) noexcept
    {
        int p = percent0_100;
        if (p < 0)
            p = 0;
        if (p > 100)
            p = 100;
        const DWORD dw = static_cast<DWORD>(p);
        HKEY hKey = nullptr;
        if (!OpenOrCreateDwmKeyForWrite(hKey))
            return;
        const LSTATUS ls =
            RegSetValueExW(hKey, kGlassOpacity, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dw), sizeof(dw));
        RegCloseKey(hKey);
        if (ls == ERROR_SUCCESS)
            NotifyDwmColorizationChanged();
    }

    void WriteGlassOpacityInactivePercent0To100(int percent0_100) noexcept
    {
        int p = percent0_100;
        if (p < 0)
            p = 0;
        if (p > 100)
            p = 100;
        const DWORD dw = static_cast<DWORD>(p);
        HKEY hKey = nullptr;
        if (!OpenOrCreateDwmKeyForWrite(hKey))
            return;
        const LSTATUS ls = RegSetValueExW(
            hKey,
            kGlassOpacityInactive,
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&dw),
            sizeof(dw));
        RegCloseKey(hKey);
        if (ls == ERROR_SUCCESS)
            NotifyDwmColorizationChanged();
    }

    void DeleteGlassOpacityInactiveValue() noexcept
    {
        HKEY hKey = nullptr;
        if (!OpenOrCreateDwmKeyForWrite(hKey))
            return;
        RegDeleteValueW(hKey, kGlassOpacityInactive);
        RegCloseKey(hKey);
        NotifyDwmColorizationChanged();
    }

    void DeleteGlassOpacityValue() noexcept
    {
        HKEY hKey = nullptr;
        if (!OpenOrCreateDwmKeyForWrite(hKey))
            return;
        RegDeleteValueW(hKey, kGlassOpacity);
        RegCloseKey(hKey);
        NotifyDwmColorizationChanged();
    }

    void RestoreGlassOpacityRegistrySnapshot(
        bool glassOpacityKeyPresent,
        std::uint32_t glassOpacity0_100,
        bool glassOpacityInactiveKeyPresent,
        std::uint32_t glassOpacityInactive0_100) noexcept
    {
        HKEY hKey = nullptr;
        if (!OpenOrCreateDwmKeyForWrite(hKey))
            return;

        if (glassOpacityKeyPresent)
        {
            const DWORD v = static_cast<DWORD>(ClampPercent(glassOpacity0_100));
            RegSetValueExW(
                hKey,
                kGlassOpacity,
                0,
                REG_DWORD,
                reinterpret_cast<const BYTE*>(&v),
                sizeof(v));
        }
        else
            RegDeleteValueW(hKey, kGlassOpacity);

        if (glassOpacityInactiveKeyPresent)
        {
            const DWORD v = static_cast<DWORD>(ClampPercent(glassOpacityInactive0_100));
            RegSetValueExW(
                hKey,
                kGlassOpacityInactive,
                0,
                REG_DWORD,
                reinterpret_cast<const BYTE*>(&v),
                sizeof(v));
        }
        else
            RegDeleteValueW(hKey, kGlassOpacityInactive);

        RegCloseKey(hKey);
        NotifyDwmColorizationChanged();
    }
} // namespace Shared::DwmGlassOpacity
