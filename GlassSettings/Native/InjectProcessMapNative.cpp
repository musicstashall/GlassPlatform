#include <Windows.h>

#include "ForceUnloadGlassModules.h"

#include <InjectModuleScan.h>
#include <InjectProcessMap.h>
#include <GlassPlatformTask.h>

namespace
{
    [[nodiscard]] BOOL ReadMap(
        const wchar_t* name,
        Shared::InjectProcessMap::Slot* outSlots,
        DWORD maxSlots,
        DWORD* outCount) noexcept
    {
        return Shared::InjectProcessMap::TryReadSlotsFromMapping(
            name, outSlots, maxSlots, outCount) ? TRUE : FALSE;
    }

    // Совпадает с GlassPlatformArch / GlassPlatformTask::PlatformArch (X86=0, X64=1).
    // Не путать с InjectArch слотов карты (там X64=0, X86=1).
    [[nodiscard]] GlassShared::GlassPlatformTask::PlatformArch ToPlatformArch(int arch) noexcept
    {
        return arch == static_cast<int>(GlassShared::GlassPlatformTask::PlatformArch::X86)
            ? GlassShared::GlassPlatformTask::PlatformArch::X86
            : GlassShared::GlassPlatformTask::PlatformArch::X64;
    }
}

extern "C"
{
    __declspec(dllexport) BOOL WINAPI GlassSettings_IsInjectMapAvailable() noexcept
    {
        return (Shared::InjectProcessMap::IsMappingAvailable(Shared::InjectProcessMap::kMappingNameX64)
                || Shared::InjectProcessMap::IsMappingAvailable(Shared::InjectProcessMap::kMappingNameX86)
                || Shared::InjectProcessMap::IsMappingAvailable(Shared::InjectProcessMap::kMappingNameLegacy))
            ? TRUE
            : FALSE;
    }

    /// arch: 0 = x64 (+ legacy fallback), 1 = x86.
    __declspec(dllexport) BOOL WINAPI GlassSettings_TryReadInjectMapSlotsForArch(
        DWORD arch,
        void* outSlots,
        DWORD maxSlots,
        DWORD* outCount) noexcept
    {
        if (!outSlots || maxSlots == 0 || !outCount)
            return FALSE;

        auto* slots = static_cast<Shared::InjectProcessMap::Slot*>(outSlots);
        BOOL ok;
        if (arch == 1)
            ok = ReadMap(Shared::InjectProcessMap::kMappingNameX86, slots, maxSlots, outCount);
        else if (ReadMap(Shared::InjectProcessMap::kMappingNameX64, slots, maxSlots, outCount))
            ok = TRUE;
        else
            // Миграция: старое имя без суффикса.
            ok = ReadMap(Shared::InjectProcessMap::kMappingNameLegacy, slots, maxSlots, outCount);

        return ok;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_TryReadInjectMapSlots(
        void* outSlots,
        DWORD maxSlots,
        DWORD* outCount) noexcept
    {
        // Совместимость: только x64 (+ legacy).
        return GlassSettings_TryReadInjectMapSlotsForArch(0, outSlots, maxSlots, outCount);
    }

    /// arch: 0 = x64 (+ legacy), 1 = x86. Слот мёртвого PID после exit без FreeSelf.
    __declspec(dllexport) BOOL WINAPI GlassSettings_ClearInjectMapPidForArch(
        DWORD arch,
        DWORD pid) noexcept
    {
        if (pid == 0)
            return FALSE;
        BOOL ok;
        if (arch == 1)
        {
            ok = Shared::InjectProcessMap::TryClearPidFromMapping(
                Shared::InjectProcessMap::kMappingNameX86, pid)
                ? TRUE
                : FALSE;
        }
        else
        {
            ok = Shared::InjectProcessMap::TryClearPidFromMapping(
                Shared::InjectProcessMap::kMappingNameX64, pid)
                ? TRUE
                : FALSE;
            if (!ok)
            {
                ok = Shared::InjectProcessMap::TryClearPidFromMapping(
                    Shared::InjectProcessMap::kMappingNameLegacy, pid)
                    ? TRUE
                    : FALSE;
            }
        }
        return ok;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_RequestProcessUnload(DWORD pid) noexcept
    {
        // Soft: UnloadRequest event (живой GUI-поток GlassCompat).
        // Force: remote FreeLibrary (мёртвый residual / OS-pin без слушателя) — Unlocker-стиль.
        if (Shared::InjectProcessMap::SignalUnloadRequest(pid))
            return TRUE;
        return GlassSettingsNative::ForceUnloadGlassModules(pid) ? TRUE : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_ForceUnloadProcess(DWORD pid) noexcept
    {
        return GlassSettingsNative::ForceUnloadGlassModules(pid) ? TRUE : FALSE;
    }

    /// arch: GlassPlatformArch / PlatformArch — 0 = x86, 1 = x64.
    __declspec(dllexport) BOOL WINAPI GlassSettings_HasShellInjectResidualsForArch(int arch) noexcept
    {
        return GlassShared::GlassPlatformTask::HasShellInjectResiduals(ToPlatformArch(arch))
            ? TRUE
            : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_HasShellInjectResiduals() noexcept
    {
        return GlassSettings_HasShellInjectResidualsForArch(
                   static_cast<int>(GlassShared::GlassPlatformTask::PlatformArch::X86))
            || GlassSettings_HasShellInjectResidualsForArch(
                   static_cast<int>(GlassShared::GlassPlatformTask::PlatformArch::X64));
    }

    __declspec(dllexport) int WINAPI GlassSettings_QueryInjectLifecycleUiForArch(int arch) noexcept
    {
        return static_cast<int>(
            GlassShared::GlassPlatformTask::QueryInjectLifecycleUi(ToPlatformArch(arch)));
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_RefreshInjectResidualStickyForArch(
        int arch) noexcept
    {
        return GlassShared::GlassPlatformTask::RefreshInjectResidualSticky(ToPlatformArch(arch))
            ? TRUE
            : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_ScanGlassModules(
        const DWORD* pids,
        DWORD pidsCount,
        void* outResults,
        DWORD maxResults,
        DWORD* outCount) noexcept
    {
        if (!outResults || maxResults == 0 || !outCount)
            return FALSE;
        const BOOL ok = Shared::InjectModuleScan::Scan(
            pids,
            pidsCount,
            static_cast<Shared::InjectModuleScan::Result*>(outResults),
            maxResults,
            outCount) ? TRUE : FALSE;
        return ok;
    }
}
