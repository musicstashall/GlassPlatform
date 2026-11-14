#pragma once

// Loader refcount и маска Glass*-модулей в процессе (для shutdown / sweep).
// Не пишет логи.

#include <windows.h>
#include <tlhelp32.h>

namespace Shared::ModuleRef
{
    inline constexpr unsigned kRcUnknown = 0xFFFFFFFFu;

    struct LdrDdagNode
    {
        LIST_ENTRY Modules;
        PVOID ServiceTagList;
        ULONG LoadCount;
        ULONG LoadWhileUnloadingCount;
    };

    struct LdrEntry
    {
        LIST_ENTRY InLoadOrderLinks;
        LIST_ENTRY InMemoryOrderLinks;
        LIST_ENTRY InInitializationOrderLinks;
        PVOID DllBase;
        PVOID EntryPoint;
        ULONG SizeOfImage;
        struct { USHORT Length; USHORT MaximumLength; PWSTR Buffer; } FullDllName;
        struct { USHORT Length; USHORT MaximumLength; PWSTR Buffer; } BaseDllName;
        ULONG Flags;
        USHORT ObsoleteLoadCount;
        USHORT TlsIndex;
        LIST_ENTRY HashLinks;
        ULONG TimeDateStamp;
        PVOID EntryPointActivationContext;
        PVOID Lock;
        LdrDdagNode* DdagNode;
    };

    struct PebLdr
    {
        ULONG Length;
        BOOLEAN Initialized;
        PVOID SsHandle;
        LIST_ENTRY InLoadOrderModuleList;
    };

    struct Peb
    {
        BYTE Reserved1[2];
        BYTE BeingDebugged;
        BYTE Reserved2[1];
        PVOID Reserved3[2];
        PebLdr* Ldr;
    };

    // 0 = модуля нет; kRcUnknown = прочитать не удалось.
    inline unsigned QueryLoadCount(HMODULE hMod) noexcept
    {
        if (!hMod)
            return 0;
#if defined(_WIN64)
        auto* peb = reinterpret_cast<Peb*>(__readgsqword(0x60));
#else
        auto* peb = reinterpret_cast<Peb*>(__readfsdword(0x30));
#endif
        if (!peb || !peb->Ldr)
            return kRcUnknown;
        LIST_ENTRY* const head = &peb->Ldr->InLoadOrderModuleList;
        for (LIST_ENTRY* e = head->Flink; e && e != head; e = e->Flink)
        {
            auto* entry = CONTAINING_RECORD(e, LdrEntry, InLoadOrderLinks);
            if (entry->DllBase != hMod)
                continue;
            if (entry->DdagNode)
                return entry->DdagNode->LoadCount;
            return entry->ObsoleteLoadCount;
        }
        return 0;
    }

    inline unsigned QueryLoadCountByName(const wchar_t* name) noexcept
    {
        return QueryLoadCount(GetModuleHandleW(name));
    }

    inline constexpr const wchar_t* kGlassModules[] = {
        L"GlassCompat.dll",
        L"GlassCore.dll",
        L"WndCompat.dll",
        L"ExplorerCompat.dll",
        L"TaskbarCompat.dll",
        L"UICompat.dll",
    };

    // bit0=compat,1=core,2=wnd,3=explorer,4=taskbar,5=ui
    inline unsigned RemoteMask(DWORD pid) noexcept
    {
        unsigned mask = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE)
            return 0;
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me))
        {
            do
            {
                for (unsigned i = 0; i < 6; ++i)
                {
                    if (_wcsicmp(me.szModule, kGlassModules[i]) == 0)
                        mask |= (1u << i);
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
        return mask;
    }
}
