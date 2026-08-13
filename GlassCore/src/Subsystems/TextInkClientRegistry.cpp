// Реестр Edit/RichEdit (+ WinForms): register на attach, soft-invalidate при ink reload.

#include "CorePch.h"
#include <Subsystems/TextInkClientRegistry.h>
#include <GlassControlKind.h>
#include <DebugLog.h>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GlassCore::TextInk
{
    namespace
    {
        using WndCompatEnsureWinFormsWindowMarkedFn = void(WINAPI*)(HWND) noexcept;

        std::mutex g_inkMutex;
        std::unordered_map<std::uintptr_t, std::unordered_set<std::uintptr_t>> g_inkHwndByBucket;
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_bucketByHwnd;

        [[nodiscard]] WndCompatEnsureWinFormsWindowMarkedFn GetWndCompatEnsureWinFormsWindowMarkedFn() noexcept
        {
            static const WndCompatEnsureWinFormsWindowMarkedFn s_pfn = []() noexcept -> WndCompatEnsureWinFormsWindowMarkedFn {
                const HMODULE hMod = GetModuleHandleW(L"WndCompat.dll");
                if (!hMod)
                    return nullptr;
                return reinterpret_cast<WndCompatEnsureWinFormsWindowMarkedFn>(
                    GetProcAddress(hMod, "WndCompat_EnsureWinFormsWindowMarked"));
            }();
            return s_pfn;
        }

        void DispatchWndCompatEnsureWinFormsWindowMarked(HWND hWnd) noexcept
        {
            if (!hWnd || !IsWindow(hWnd))
                return;
            const WndCompatEnsureWinFormsWindowMarkedFn pfn = GetWndCompatEnsureWinFormsWindowMarkedFn();
            if (!pfn)
                return;
            pfn(hWnd);
        }

        [[nodiscard]] HWND ResolveTextInkRegistryBucket(HWND hControl, bool syntheticParentCatchUp) noexcept
        {
            if (!hControl)
                return nullptr;
            using namespace GlassShared;

            if (syntheticParentCatchUp)
            {
                for (HWND w = hControl; w; w = GetParent(w))
                {
                    if (GlassGetWindowKind(w) != GlassControlKind::None)
                        continue;
                    DispatchWndCompatEnsureWinFormsWindowMarked(w);
                }
            }

            for (HWND w = hControl; w; w = GetParent(w))
            {
                if (GlassGetWindowKind(w) == GlassControlKind::WinForms_Window)
                    return w;
            }
            const HWND ga = GetAncestor(hControl, GA_ROOT);
            return ga ? ga : hControl;
        }

        [[nodiscard]] HWND ResolveTextInkRegistryBucket(HWND hControl) noexcept
        {
            return ResolveTextInkRegistryBucket(hControl, false);
        }

        [[nodiscard]] bool IsInkBucketUnderGaRoot(HWND bucketHwnd, HWND gaRoot) noexcept
        {
            if (!bucketHwnd || !gaRoot || !IsWindow(bucketHwnd) || !IsWindow(gaRoot))
                return false;
            if (bucketHwnd == gaRoot)
                return true;
            const HWND bucketGa = GetAncestor(bucketHwnd, GA_ROOT);
            if (bucketGa && bucketGa == gaRoot)
                return true;
            for (HWND w = bucketHwnd; w;)
            {
                const HWND owner = GetWindow(w, GW_OWNER);
                if (!owner)
                    break;
                if (owner == gaRoot)
                    return true;
                if (GetAncestor(owner, GA_ROOT) == gaRoot)
                    return true;
                w = owner;
            }
            return false;
        }

        void EraseHwndFromMaps(HWND hControl) noexcept
        {
            const std::uintptr_t hKey = reinterpret_cast<std::uintptr_t>(hControl);
            const auto bucketIt = g_bucketByHwnd.find(hKey);
            if (bucketIt == g_bucketByHwnd.end())
                return;

            const std::uintptr_t bucketKey = bucketIt->second;
            g_bucketByHwnd.erase(bucketIt);

            const auto setIt = g_inkHwndByBucket.find(bucketKey);
            if (setIt != g_inkHwndByBucket.end())
            {
                setIt->second.erase(hKey);
                if (setIt->second.empty())
                    g_inkHwndByBucket.erase(setIt);
            }
        }

        [[nodiscard]] bool SoftInvalidateClientHwnd(HWND hCtrl) noexcept
        {
            if (!hCtrl || !IsWindow(hCtrl) || !IsWindowVisible(hCtrl))
                return false;
            return InvalidateRect(hCtrl, nullptr, FALSE) != FALSE;
        }

        [[nodiscard]] std::size_t SoftInvalidateTextInkClientsForBucketImpl(HWND hBucket) noexcept
        {
            if (!hBucket || !IsWindow(hBucket))
                return 0;

            std::vector<HWND> copy;
            {
                std::lock_guard lock(g_inkMutex);
                const auto it = g_inkHwndByBucket.find(reinterpret_cast<std::uintptr_t>(hBucket));
                if (it == g_inkHwndByBucket.end())
                {
                    DebugLog(DBG_IMPORTANT,
                        L"[GlassCore][TextInk] SoftInvalidate bucket=0x%p count=0 (нет записей)",
                        hBucket);
                    return 0;
                }
                copy.reserve(it->second.size());
                for (const std::uintptr_t p : it->second)
                    copy.push_back(reinterpret_cast<HWND>(p));
            }

            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][TextInk] SoftInvalidate bucket=0x%p count=%zu",
                hBucket,
                copy.size());

            std::size_t invalidated = 0;
            for (HWND h : copy)
            {
                if (!h || !IsWindow(h))
                    continue;
                if (SoftInvalidateClientHwnd(h))
                    ++invalidated;
            }
            return invalidated;
        }
    } // namespace

    bool IsTextInkRegisterCandidate(GlassShared::GlassControlKind kind) noexcept
    {
        using GK = GlassShared::GlassControlKind;
        switch (kind)
        {
        case GK::Edit:
        case GK::WinForms_Edit:
        case GK::RichEdit20W:
        case GK::WinForms_RichEdit20W:
        case GK::RichEdit50W:
        case GK::WinForms_RichEdit50W:
            return true;
        default:
            return false;
        }
    }

    void RegisterTextInkClient(HWND hControl) noexcept
    {
        if (!hControl || !IsWindow(hControl))
            return;

        const GlassShared::GlassControlKind kind = GlassShared::GlassGetWindowKind(hControl);
        if (!IsTextInkRegisterCandidate(kind))
            return;

        const HWND bucket = ResolveTextInkRegistryBucket(hControl, true);
        if (!bucket)
            return;

        const std::uintptr_t bucketKey = reinterpret_cast<std::uintptr_t>(bucket);
        const std::uintptr_t hKey      = reinterpret_cast<std::uintptr_t>(hControl);
        const HWND gaRoot              = GetAncestor(hControl, GA_ROOT);

        std::lock_guard lock(g_inkMutex);
        EraseHwndFromMaps(hControl);
        g_bucketByHwnd[hKey] = bucketKey;
        g_inkHwndByBucket[bucketKey].insert(hKey);

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][TextInk] Register hCtrl=0x%p kind=%u bucket=0x%p gaRoot=0x%p",
            hControl,
            static_cast<unsigned>(static_cast<std::uint8_t>(kind)),
            bucket,
            gaRoot);
    }

    void UnregisterTextInkClient(HWND hControl) noexcept
    {
        if (!hControl)
            return;
        std::lock_guard lock(g_inkMutex);
        EraseHwndFromMaps(hControl);
        DebugLog(DBG_IMPORTANT, L"[GlassCore][TextInk] Unregister hCtrl=0x%p", hControl);
    }

    void TryRegisterTextInkClient(HWND hControl) noexcept
    {
        if (!hControl || !IsWindow(hControl))
            return;
        if (!IsTextInkRegisterCandidate(GlassShared::GlassGetWindowKind(hControl)))
            return;
        RegisterTextInkClient(hControl);
    }

    void EnsureTextInkClientRegistered(HWND hControl) noexcept
    {
        if (!hControl || !IsWindow(hControl))
            return;
        if (!IsTextInkRegisterCandidate(GlassShared::GlassGetWindowKind(hControl)))
            return;
        {
            std::lock_guard lock(g_inkMutex);
            const auto it = g_bucketByHwnd.find(reinterpret_cast<std::uintptr_t>(hControl));
            if (it != g_bucketByHwnd.end())
                return;
        }
        RegisterTextInkClient(hControl);
    }

    bool IsTextInkClientRegistered(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return false;
        std::lock_guard lock(g_inkMutex);
        return g_bucketByHwnd.find(reinterpret_cast<std::uintptr_t>(hWnd)) != g_bucketByHwnd.end();
    }

    void SoftInvalidateTextInkClientsForBucket(HWND hBucket) noexcept
    {
        (void)SoftInvalidateTextInkClientsForBucketImpl(hBucket);
    }

    void SoftInvalidateTextInkClientsForGaRootTree(HWND hRoot) noexcept
    {
        if (!hRoot || !IsWindow(hRoot))
            return;

        HWND gaRoot = hRoot;
        if (const HWND a = GetAncestor(hRoot, GA_ROOT))
            gaRoot = a;

        std::vector<HWND> buckets;
        buckets.push_back(gaRoot);
        if (hRoot != gaRoot)
            buckets.push_back(hRoot);

        {
            std::lock_guard lock(g_inkMutex);
            for (const auto& entry : g_inkHwndByBucket)
            {
                const HWND bucket = reinterpret_cast<HWND>(entry.first);
                if (bucket == gaRoot || bucket == hRoot)
                    continue;
                if (IsInkBucketUnderGaRoot(bucket, gaRoot))
                    buckets.push_back(bucket);
            }
        }

        std::sort(buckets.begin(), buckets.end());
        buckets.erase(std::unique(buckets.begin(), buckets.end()), buckets.end());

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][TextInk] SoftInvalidateTree hRoot=0x%p gaRoot=0x%p buckets=%zu",
            hRoot,
            gaRoot,
            buckets.size());

        std::size_t totalInvalidated = 0;
        for (HWND bucket : buckets)
            totalInvalidated += SoftInvalidateTextInkClientsForBucketImpl(bucket);

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][TextInk] SoftInvalidateTree end gaRoot=0x%p invalidated=%zu",
            gaRoot,
            totalInvalidated);
    }

    void RunDeferredTextInkSoftInvalidate(HWND hRoot) noexcept
    {
        if (!hRoot || !IsWindow(hRoot))
            return;
        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][TextInk] DeferredSoftInvalidate hRoot=0x%p gaRoot=0x%p",
            hRoot,
            GetAncestor(hRoot, GA_ROOT));
        SoftInvalidateTextInkClientsForGaRootTree(hRoot);
    }

    void RemoveTextInkClientsForRoot(HWND hRoot) noexcept
    {
        if (!hRoot)
            return;

        HWND gaRoot = hRoot;
        if (const HWND a = GetAncestor(hRoot, GA_ROOT))
            gaRoot = a;

        std::vector<std::uintptr_t> purgeBuckets;
        {
            std::lock_guard lock(g_inkMutex);
            for (const auto& entry : g_inkHwndByBucket)
            {
                const HWND bucket = reinterpret_cast<HWND>(entry.first);
                if (IsInkBucketUnderGaRoot(bucket, gaRoot))
                    purgeBuckets.push_back(entry.first);
            }

            for (const std::uintptr_t bucketKey : purgeBuckets)
            {
                const auto setIt = g_inkHwndByBucket.find(bucketKey);
                if (setIt == g_inkHwndByBucket.end())
                    continue;
                for (const std::uintptr_t hKey : setIt->second)
                    g_bucketByHwnd.erase(hKey);
                g_inkHwndByBucket.erase(setIt);
            }
        }

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][TextInk] RemoveForRoot hRoot=0x%p gaRoot=0x%p buckets=%zu",
            hRoot,
            gaRoot,
            purgeBuckets.size());
    }

    void ClearAllTextInkClients() noexcept
    {
        std::lock_guard lock(g_inkMutex);
        g_inkHwndByBucket.clear();
        g_bucketByHwnd.clear();
    }
} // namespace GlassCore::TextInk
