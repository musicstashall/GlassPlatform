#include "ForceUnloadGlassModules.h"

#include <InjectProcessMap.h>

#include <TlHelp32.h>
#include <cstring>

namespace GlassSettingsNative
{
    namespace
    {
        constexpr const wchar_t* kGlassModules[] = {
            L"WndCompat.dll",
            L"GlassCore.dll",
            L"GlassCompat.dll",
        };
        // ExplorerCompat / TaskbarCompat: IXDE LoadLibrary-pin — не Remote FreeLibrary.

        [[nodiscard]] HMODULE FindModuleBase(DWORD pid, const wchar_t* moduleFile) noexcept
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
            if (snap == INVALID_HANDLE_VALUE)
                return nullptr;

            MODULEENTRY32W me{};
            me.dwSize = sizeof(me);
            HMODULE found = nullptr;
            if (Module32FirstW(snap, &me))
            {
                do
                {
                    if (_wcsicmp(me.szModule, moduleFile) == 0)
                    {
                        found = me.hModule;
                        break;
                    }
                } while (Module32NextW(snap, &me));
            }
            CloseHandle(snap);
            return found;
        }

        [[nodiscard]] bool RemoteFreeLibrary(HANDLE hProcess, HMODULE hMod) noexcept
        {
            if (!hProcess || !hMod)
                return false;

            const HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
            if (!hKernel)
                return false;
            const auto pFreeLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
                GetProcAddress(hKernel, "FreeLibrary"));
            if (!pFreeLibrary)
                return false;

            HANDLE hThread = CreateRemoteThread(
                hProcess,
                nullptr,
                0,
                pFreeLibrary,
                hMod,
                0,
                nullptr);
            if (!hThread)
                return false;

            // Короткий таймаут: длинный Wait на UI-потоке GS выглядел как «намертво».
            const DWORD wait = WaitForSingleObject(hThread, 1500);
            CloseHandle(hThread);
            return wait == WAIT_OBJECT_0;
        }
    }

    bool ForceUnloadGlassModules(DWORD pid) noexcept
    {
        if (pid == 0 || pid == GetCurrentProcessId())
            return false;

        // Не трогать живой хост GlassCompat (rundll32 Main) — CreateRemoteThread+FreeLibrary
        // вешает UI GS (WaitForSingleObject) и ломает платформу.
        auto readHostPid = [](const wchar_t* mapName) noexcept -> DWORD
        {
            HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, mapName);
            if (!hMap)
                return 0;
            DWORD hostPid = 0;
            if (void* view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(DWORD)))
            {
                hostPid = *static_cast<const DWORD*>(view);
                UnmapViewOfFile(view);
            }
            CloseHandle(hMap);
            return hostPid;
        };
        if (pid == readHostPid(L"Local\\GlassHostPid.x64")
            || pid == readHostPid(L"Local\\GlassHostPid.x86"))
            return false;

        HANDLE hProcess = OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_DUP_HANDLE,
            FALSE,
            pid);
        if (!hProcess)
            return false;

        bool any = false;
        for (int pass = 0; pass < 4; ++pass)
        {
            bool freedThisPass = false;
            for (const wchar_t* name : kGlassModules)
            {
                const HMODULE hMod = FindModuleBase(pid, name);
                if (!hMod)
                    continue;
                if (RemoteFreeLibrary(hProcess, hMod))
                {
                    any = true;
                    freedThisPass = true;
                }
            }
            if (!freedThisPass)
                break;
            Sleep(30);
        }

        CloseHandle(hProcess);

        if (any)
        {
            (void)Shared::InjectProcessMap::TryClearPidFromMapping(
                Shared::InjectProcessMap::kMappingNameX64, pid);
            (void)Shared::InjectProcessMap::TryClearPidFromMapping(
                Shared::InjectProcessMap::kMappingNameX86, pid);
            (void)Shared::InjectProcessMap::TryClearPidFromMapping(
                Shared::InjectProcessMap::kMappingNameLegacy, pid);
        }

        return any;
    }
}
