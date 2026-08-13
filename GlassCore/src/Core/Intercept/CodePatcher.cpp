#include "CorePch.h"
#include <Core/Intercept/CodePatcher.h>
#include <Core/Intercept/HookHelper.h>

namespace GlassCore::Hooks
{
    std::optional<E8Patch> InspectE8Call(void* callSite)
    {
        if (!callSite)
            return std::nullopt;

        auto p = static_cast<unsigned char*>(callSite);
        if (*p != 0xE8)
            return std::nullopt;

        std::int32_t rel = *reinterpret_cast<std::int32_t*>(p + 1);
        auto next = p + 5;
        void* target = next + rel;

        E8Patch patch{};
        patch.callSite = callSite;
        patch.originalRel32 = rel;
        patch.originalTarget = target;
        patch.applied = false;
        return patch;
    }

    // Выделяет страницу памяти в диапазоне ±2GB от targetAddr
    static void* AllocNearAddress(void* targetAddr)
    {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);

        auto base = reinterpret_cast<uintptr_t>(targetAddr);
        constexpr uintptr_t kRange = 0x7F000000; // ~2GB с запасом
        constexpr size_t    kPageSize = 0x1000;

        uintptr_t lo = (base > kRange) ? (base - kRange) : 0;
        uintptr_t hi = base + kRange;

        // Ищем свободный регион рядом, начиная с адреса выше targetAddr
        for (uintptr_t addr = (base + kPageSize) & ~(kPageSize - 1); addr < hi; addr += kPageSize)
        {
            void* p = VirtualAlloc(
                reinterpret_cast<void*>(addr),
                kPageSize,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_EXECUTE_READWRITE
            );
            if (p)
                return p;
        }

        // Пробуем ниже
        for (uintptr_t addr = (base - kPageSize) & ~(kPageSize - 1); addr > lo; addr -= kPageSize)
        {
            void* p = VirtualAlloc(
                reinterpret_cast<void*>(addr),
                kPageSize,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_EXECUTE_READWRITE
            );
            if (p)
                return p;
        }

        return nullptr;
    }

    bool ApplyE8CallPatch(E8Patch& patch, void* detourTarget)
    {
        if (!patch.callSite || !detourTarget)
            return false;

        auto p = static_cast<unsigned char*>(patch.callSite);
        if (*p != 0xE8)
        {
            DebugLog(L"[GlassCore][CodePatcher] ApplyE8CallPatch: byte 0x%02X != 0xE8", *p);
            return false;
        }

        auto next = p + 5;
        auto rel64 = reinterpret_cast<unsigned char*>(detourTarget) - next;

        void* trampolinePage = nullptr;

        if (rel64 < INT32_MIN || rel64 > INT32_MAX)
        {
            // Нужен трамплин рядом с call site
            trampolinePage = AllocNearAddress(patch.callSite);
            if (!trampolinePage)
            {
                DebugLog(L"[GlassCore][CodePatcher] ApplyE8CallPatch: AllocNearAddress FAILED for %p", patch.callSite);
                return false;
            }

            // Пишем FF 25 + 32bit offset + abs64 адрес = JMP [rip+0] <detourTarget>
            auto stub = static_cast<unsigned char*>(trampolinePage);
            stub[0] = 0xFF;
            stub[1] = 0x25;
            *reinterpret_cast<int32_t*>(stub + 2) = 0; // RIP+0 → следующий QWORD
            *reinterpret_cast<void**>(stub + 6) = detourTarget;

            // Теперь near rel32 — от call site до трамплина
            rel64 = stub - next;
            if (rel64 < INT32_MIN || rel64 > INT32_MAX)
            {
                DebugLog(L"[GlassCore][CodePatcher] ApplyE8CallPatch: trampoline still out of range: callSite=%p stub=%p",
                    patch.callSite, stub);
                VirtualFree(trampolinePage, 0, MEM_RELEASE);
                return false;
            }

            patch.trampolinePage = trampolinePage;
            DebugLog(L"[GlassCore][CodePatcher] ApplyE8CallPatch: trampoline at %p for callSite=%p", stub, patch.callSite);
        }

        auto rel32 = static_cast<std::int32_t>(rel64);

        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(p, &mbi, sizeof(mbi));

        DWORD oldProtect{};
        if (!VirtualProtect(mbi.BaseAddress, mbi.RegionSize, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            DebugLog(L"[GlassCore][CodePatcher] ApplyE8CallPatch: VirtualProtect FAILED err=%lu", GetLastError());
            if (trampolinePage) VirtualFree(trampolinePage, 0, MEM_RELEASE);
            return false;
        }

        *p = 0xE8;
        *reinterpret_cast<std::int32_t*>(p + 1) = rel32;

        DWORD dummy{};
        VirtualProtect(mbi.BaseAddress, mbi.RegionSize, oldProtect, &dummy);

        patch.applied = true;
        DebugLog(L"[GlassCore][CodePatcher] ApplyE8CallPatch: SUCCESS callSite=%p rel32=0x%08X trampoline=%p",
            patch.callSite, (uint32_t)rel32, trampolinePage);
        return true;
    }

    bool RevertE8CallPatch(E8Patch& patch)
    {
        if (!patch.callSite)
            return false;

        auto p = static_cast<unsigned char*>(patch.callSite);

        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(p, &mbi, sizeof(mbi));
        DWORD oldProtect{};
        VirtualProtect(mbi.BaseAddress, mbi.RegionSize, PAGE_EXECUTE_READWRITE, &oldProtect);

        *p = 0xE8;
        *reinterpret_cast<std::int32_t*>(p + 1) = patch.originalRel32;

        DWORD dummy{};
        VirtualProtect(mbi.BaseAddress, mbi.RegionSize, oldProtect, &dummy);

        if (patch.trampolinePage)
        {
            VirtualFree(patch.trampolinePage, 0, MEM_RELEASE);
            patch.trampolinePage = nullptr;
        }

        patch.applied = false;
        return true;
    }
}
