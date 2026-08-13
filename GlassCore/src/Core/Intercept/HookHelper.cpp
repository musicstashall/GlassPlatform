#include "CorePch.h"
#include <Core/Intercept/HookHelper.h>
#include <detours.h>

namespace GlassCore::Hooks
{
    namespace
    {
        // H67: HWiNFO — битый/странный IAT → strcmp AV в GetIAT (дамп HookHelper:366).
        [[nodiscard]] bool IsPlausibleCStringPtr(LPCSTR p) noexcept
        {
            if (!p)
                return false;
            const auto addr = reinterpret_cast<UINT_PTR>(p);
            // NULL-page / ordinal-as-pointer
            if (addr < 0x10000ull)
                return false;
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
                return false;
            if (mbi.State != MEM_COMMIT)
                return false;
            const DWORD prot = mbi.Protect & 0xFFu;
            switch (prot)
            {
            case PAGE_READONLY:
            case PAGE_READWRITE:
            case PAGE_WRITECOPY:
            case PAGE_EXECUTE_READ:
            case PAGE_EXECUTE_READWRITE:
            case PAGE_EXECUTE_WRITECOPY:
                return true;
            default:
                return false;
            }
        }

        [[nodiscard]] bool SafeCStringEqual(LPCSTR a, LPCSTR b) noexcept
        {
            if (!IsPlausibleCStringPtr(a) || !IsPlausibleCStringPtr(b))
                return false;
            __try
            {
                return std::strcmp(a, b) == 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        [[nodiscard]] bool SafeCStringEqualI(LPCSTR a, LPCSTR b) noexcept
        {
            if (!IsPlausibleCStringPtr(a) || !IsPlausibleCStringPtr(b))
                return false;
            __try
            {
                return _stricmp(a, b) == 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    //
    // ---------------------- ThreadSnapshotExcludeSelf ----------------------
    //

    ThreadSnapshotExcludeSelf::ThreadSnapshotExcludeSelf()
    {
        if (PssCaptureSnapshot(GetCurrentProcess(), PSS_CAPTURE_THREADS, 0, &m_snapshot) != ERROR_SUCCESS)
            m_snapshot = nullptr;
    }

    ThreadSnapshotExcludeSelf::~ThreadSnapshotExcludeSelf()
    {
        if (m_snapshot)
        {
            PssFreeSnapshot(GetCurrentProcess(), m_snapshot);
            m_snapshot = nullptr;
        }
    }

    void ThreadSnapshotExcludeSelf::ForEach(const std::function<bool(const PSS_THREAD_ENTRY&)>& callback)
    {
        if (!m_snapshot || !callback)
            return;

        HPSSWALK walk{ nullptr };
        if (PssWalkMarkerCreate(nullptr, &walk) != ERROR_SUCCESS)
            return;

        auto cleanup = wil::scope_exit([&]
            {
                if (walk)
                {
                    PssWalkMarkerFree(walk);
                    walk = nullptr;
                }
            });

        PSS_THREAD_ENTRY threadEntry{};
        while (PssWalkSnapshot(m_snapshot, PSS_WALK_THREADS, walk, &threadEntry, sizeof(threadEntry)) == ERROR_SUCCESS)
        {
            if (threadEntry.ThreadId == GetCurrentThreadId())
                continue;

            if (!callback(threadEntry))
                break;
        }
    }

    void ThreadSnapshotExcludeSelf::Suspend()
    {
        ForEach([](const PSS_THREAD_ENTRY& threadEntry)
            {
                wil::unique_handle threadHandle{ OpenThread(THREAD_SUSPEND_RESUME, FALSE, threadEntry.ThreadId) };
                if (threadHandle)
                    SuspendThread(threadHandle.get());

                return true;
            });
    }

    void ThreadSnapshotExcludeSelf::Resume()
    {
        ForEach([](const PSS_THREAD_ENTRY& threadEntry)
            {
                wil::unique_handle threadHandle{ OpenThread(THREAD_SUSPEND_RESUME, FALSE, threadEntry.ThreadId) };
                if (threadHandle)
                    ResumeThread(threadHandle.get());

                return true;
            });
    }

    DWORD ThreadSnapshotExcludeSelf::GetMainThreadId()
    {
        DWORD result{ 0 };
        ForEach([&](const PSS_THREAD_ENTRY& entry)
            {
                result = entry.ThreadId;
                return false;
            });
        return result;
    }

    //
    // --------------------------- Low-level helpers -------------------------
    //

    void WriteMemory(void* memoryAddress, const std::function<void()>& callback) noexcept
    {
        if (!memoryAddress || !callback)
            return;

        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(memoryAddress, &mbi, sizeof(mbi)))
            return;

        DWORD oldProtect{};
        if (!VirtualProtect(mbi.BaseAddress, mbi.RegionSize, PAGE_EXECUTE_READWRITE, &oldProtect))
            return;

        callback();

        DWORD dummy{};
        VirtualProtect(mbi.BaseAddress, mbi.RegionSize, oldProtect, &dummy);
    }

    void* InjectCallbackToThread(DWORD threadId, const std::function<void()>& callback) noexcept
    {
#ifndef _WIN64
        (void)threadId;
        (void)callback;
        return nullptr;
#else
        if (!callback)
            return nullptr;

        wil::unique_handle threadHandle{
            OpenThread(THREAD_SET_CONTEXT | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, threadId)
        };
        if (!threadHandle)
            return nullptr;

        if (SuspendThread(threadHandle.get()) == static_cast<DWORD>(-1))
            return nullptr;

        auto resumeGuard = wil::scope_exit([&]
            {
                ResumeThread(threadHandle.get());
            });

        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_FULL;
        if (!GetThreadContext(threadHandle.get(), &ctx))
            return nullptr;

        auto proxyCallback = [](ULONG_PTR param)
            {
                std::unique_ptr<std::function<void()>> cb{ reinterpret_cast<std::function<void()>*>(param) };
                (*cb)();
            };

        static unsigned char s_shellCode[] =
        {
            0x48, 0x83, 0xEC, 0x28,
            0x48, 0x89, 0x44, 0x24, 0x18,
            0x48, 0x89, 0x4C, 0x24, 0x10,
            0x48, 0xB9, 0,0,0,0,0,0,0,0,      // callback ptr
            0x48, 0xB8, 0,0,0,0,0,0,0,0,      // proxy ptr
            0xFF, 0xD0,
            0x48, 0x8B, 0x4C, 0x24, 0x10,
            0x48, 0x8B, 0x44, 0x24, 0x18,
            0x48, 0x83, 0xC4, 0x28,
            0x49, 0xBB, 0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33, // RIP
            0x41, 0xFF, 0xE3
        };

        auto shell = static_cast<unsigned char*>(VirtualAlloc(
            nullptr,
            sizeof(s_shellCode),
            MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE
        ));
        if (!shell)
            return nullptr;

        memcpy(shell, s_shellCode, sizeof(s_shellCode));

        *reinterpret_cast<void**>(shell + 16) = new std::function<void()>{ callback };
        *reinterpret_cast<void**>(shell + 26) = reinterpret_cast<void*>(static_cast<void(WINAPI*)(ULONG_PTR)>(proxyCallback));
        *reinterpret_cast<ULONG_PTR*>(shell + 52) = ctx.Rip;

        ctx.Rip = reinterpret_cast<ULONG_PTR>(shell);
        if (!SetThreadContext(threadHandle.get(), &ctx))
            return nullptr;

        return shell;
#endif
    }

    HMODULE GetProcessModule(HANDLE processHandle, std::wstring_view dllPath)
    {
        if (!processHandle || dllPath.empty())
            return nullptr;

        DWORD bytesNeeded{ 0 };
        if (!EnumProcessModules(processHandle, nullptr, 0, &bytesNeeded))
            return nullptr;

        auto count = bytesNeeded / sizeof(HMODULE);
        std::vector<HMODULE> modules(count);

        if (!EnumProcessModules(processHandle, modules.data(), bytesNeeded, &bytesNeeded))
            return nullptr;

        for (auto hmod : modules)
        {
            wchar_t path[MAX_PATH + 1]{};
            if (GetModuleFileNameExW(processHandle, hmod, path, MAX_PATH))
            {
                if (_wcsicmp(path, dllPath.data()) == 0)
                    return hmod;
            }
        }

        return nullptr;
    }

    //
    // --------------------------- IAT / Delay-IAT ---------------------------
    //

    void WalkIAT(
        void* baseAddress,
        std::string_view dllName,
        const std::function<bool(void** functionAddress, LPCSTR functionNameOrOrdinal, BOOL importedByName)>& callback
    ) noexcept
    {
        if (!baseAddress || dllName.empty() || !callback)
            return;

        ULONG size{ 0 };
        auto importDesc = static_cast<PIMAGE_IMPORT_DESCRIPTOR>(
            ImageDirectoryEntryToData(baseAddress, TRUE, IMAGE_DIRECTORY_ENTRY_IMPORT, &size)
            );
        if (!importDesc)
            return;

        bool found = false;
        auto desc = importDesc;
        while (desc->Name)
        {
            auto moduleName = reinterpret_cast<LPCSTR>(
                reinterpret_cast<UINT_PTR>(baseAddress) + desc->Name
                );
            if (SafeCStringEqualI(moduleName, dllName.data()))
            {
                found = true;
                break;
            }
            ++desc;
        }
        if (!found)
            return;

        auto thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(
            reinterpret_cast<UINT_PTR>(baseAddress) + desc->FirstThunk
            );
        auto nameThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(
            reinterpret_cast<UINT_PTR>(baseAddress) + desc->OriginalFirstThunk
            );

        while (thunk->u1.Function)
        {
            // H67: один битый thunk в HWiNFO не должен валить весь WalkIAT.
            bool cont = true;
            __try
            {
                LPCSTR funcName{};
                BOOL importedByName = !IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal);

                if (importedByName)
                {
                    auto byName = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(
                        reinterpret_cast<UINT_PTR>(baseAddress) + nameThunk->u1.AddressOfData
                        );
                    funcName = byName->Name;
                }
                else
                {
                    funcName = MAKEINTRESOURCEA(IMAGE_ORDINAL(nameThunk->u1.Ordinal));
                }

                auto funcAddr = reinterpret_cast<void**>(&thunk->u1.Function);
                cont = callback(funcAddr, funcName, importedByName);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                cont = true;
            }

            if (!cont)
                break;

            ++thunk;
            ++nameThunk;
        }
    }

    void WalkDelayloadIAT(
        void* baseAddress,
        std::string_view dllName,
        const std::function<bool(HMODULE* moduleHandle, void** functionAddress, LPCSTR functionNameOrOrdinal, BOOL importedByName)>& callback
    ) noexcept
    {
        if (!baseAddress || dllName.empty() || !callback)
            return;

        ULONG size{ 0 };
        auto importDesc = static_cast<PIMAGE_DELAYLOAD_DESCRIPTOR>(
            ImageDirectoryEntryToData(baseAddress, TRUE, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT, &size)
            );
        if (!importDesc)
            return;

        bool found = false;
        auto desc = importDesc;
        while (desc->DllNameRVA)
        {
            auto moduleName = reinterpret_cast<LPCSTR>(
                reinterpret_cast<UINT_PTR>(baseAddress) + desc->DllNameRVA
                );
            if (SafeCStringEqualI(moduleName, dllName.data()))
            {
                found = true;
                break;
            }
            ++desc;
        }
        if (!found)
            return;

        if (desc->Attributes.RvaBased != 1)
            return;

        auto moduleHandle = reinterpret_cast<HMODULE*>(
            reinterpret_cast<UINT_PTR>(baseAddress) + desc->ModuleHandleRVA
            );
        auto thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(
            reinterpret_cast<UINT_PTR>(baseAddress) + desc->ImportAddressTableRVA
            );
        auto nameThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(
            reinterpret_cast<UINT_PTR>(baseAddress) + desc->ImportNameTableRVA
            );

        while (thunk->u1.Function)
        {
            bool cont = true;
            __try
            {
                LPCSTR funcName{};
                BOOL importedByName = !IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal);

                if (importedByName)
                {
                    auto byName = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(
                        reinterpret_cast<UINT_PTR>(baseAddress) + nameThunk->u1.AddressOfData
                        );
                    funcName = byName->Name;
                }
                else
                {
                    funcName = MAKEINTRESOURCEA(IMAGE_ORDINAL(nameThunk->u1.Ordinal));
                }

                auto funcAddr = reinterpret_cast<void**>(&thunk->u1.Function);
                cont = callback(moduleHandle, funcAddr, funcName, importedByName);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                cont = true;
            }

            if (!cont)
                break;

            ++thunk;
            ++nameThunk;
        }
    }

    void** GetIAT(void* baseAddress, std::string_view dllName, LPCSTR targetFunctionNameOrOrdinal)
    {
        void** result{ nullptr };

        WalkIAT(baseAddress, dllName,
            [&](void** funcAddr, LPCSTR nameOrOrdinal, BOOL importedByName) -> bool
            {
                if (importedByName)
                {
                    if (targetFunctionNameOrOrdinal &&
                        (reinterpret_cast<UINT_PTR>(targetFunctionNameOrOrdinal) & 0xFFFF0000) &&
                        SafeCStringEqual(nameOrOrdinal, targetFunctionNameOrOrdinal))
                    {
                        result = funcAddr;
                        return false;
                    }
                }
                else
                {
                    if (nameOrOrdinal == targetFunctionNameOrOrdinal)
                    {
                        result = funcAddr;
                        return false;
                    }
                }
                return true;
            });

        return result;
    }

    std::pair<HMODULE*, void**> GetDelayloadIAT(
        void* baseAddress,
        std::string_view dllName,
        LPCSTR targetFunctionNameOrOrdinal,
        bool resolveAPI
    )
    {
        std::pair<HMODULE*, void**> result{ nullptr, nullptr };

        WalkDelayloadIAT(baseAddress, dllName,
            [&](HMODULE* moduleHandle, void** funcAddr, LPCSTR nameOrOrdinal, BOOL importedByName) -> bool
            {
                if (importedByName)
                {
                    if (targetFunctionNameOrOrdinal &&
                        (reinterpret_cast<UINT_PTR>(targetFunctionNameOrOrdinal) & 0xFFFF0000) &&
                        SafeCStringEqual(nameOrOrdinal, targetFunctionNameOrOrdinal))
                    {
                        result.first = moduleHandle;
                        result.second = funcAddr;
                        if (resolveAPI)
                            ResolveDelayloadIAT(result, baseAddress, dllName, targetFunctionNameOrOrdinal);
                        return false;
                    }
                }
                else
                {
                    if (nameOrOrdinal == targetFunctionNameOrOrdinal)
                    {
                        result.first = moduleHandle;
                        result.second = funcAddr;
                        if (resolveAPI)
                            ResolveDelayloadIAT(result, baseAddress, dllName, targetFunctionNameOrOrdinal);
                        return false;
                    }
                }
                return true;
            });

        return result;
    }

    void* WriteIAT(
        void* baseAddress,
        std::string_view dllName,
        LPCSTR targetFunctionNameOrOrdinal,
        void* detourFunction
    )
    {
        if (!baseAddress || !detourFunction)
            return nullptr;

        auto slot = GetIAT(baseAddress, dllName, targetFunctionNameOrOrdinal);
        if (!slot)
            return nullptr;

        void* original = *slot;
        WriteMemory(slot, [&]
            {
                InterlockedExchangePointer(slot, detourFunction);
            });

        return original;
    }

    std::pair<HMODULE, void*> WriteDelayloadIAT(
        void* baseAddress,
        std::string_view dllName,
        LPCSTR targetFunctionNameOrOrdinal,
        void* detourFunction,
        std::optional<HMODULE> newModuleHandle
    )
    {
        std::pair<HMODULE, void*> result{ nullptr, nullptr };

        if (!baseAddress || !detourFunction)
            return result;

        auto info = GetDelayloadIAT(baseAddress, dllName, targetFunctionNameOrOrdinal, true);
        auto moduleHandle = info.first;
        auto funcAddr = info.second;

        if (!funcAddr)
            return result;

        result.first = moduleHandle ? *moduleHandle : nullptr;
        result.second = *funcAddr;

        if (newModuleHandle)
        {
            WriteMemory(moduleHandle, [&]
                {
                    InterlockedExchangePointer(reinterpret_cast<void**>(moduleHandle), newModuleHandle.value());
                });
        }

        WriteMemory(funcAddr, [&]
            {
                InterlockedExchangePointer(funcAddr, detourFunction);
            });

        return result;
    }

    void ResolveDelayloadIAT(
        const std::pair<HMODULE*, void**>& info,
        void* baseAddress,
        std::string_view dllName,
        LPCSTR targetFunctionNameOrOrdinal
    )
    {
        auto moduleHandle = info.first;
        auto funcAddr = info.second;

        if (!moduleHandle || !funcAddr || !*funcAddr)
            return;

        auto func = *funcAddr;
        auto containing = DetourGetContainingModule(func);
        if (containing != baseAddress)
            containing = DetourGetContainingModule(DetourCodeFromPointer(func, nullptr));

        if (containing != baseAddress)
            return;

        HMODULE importModule = *moduleHandle;
        if (!importModule)
        {
            importModule = LoadLibraryA(dllName.data());
            if (!importModule)
                return;

            WriteMemory(moduleHandle, [&]
                {
                    InterlockedExchangePointer(reinterpret_cast<void**>(moduleHandle), importModule);
                });
        }

        auto real = GetProcAddress(importModule, targetFunctionNameOrOrdinal);
        if (!real)
            return;

        WriteMemory(funcAddr, [&]
            {
                InterlockedExchangePointer(funcAddr, real);
            });
    }

    //
    // ---------------------- Code pattern / .text helpers -------------------
    //

    unsigned char* MatchAsmCode(
        unsigned char* startAddress,
        std::pair<unsigned char*, size_t> asmCodeInfo,
        std::vector<std::pair<size_t, size_t>> asmSliceInfo,
        size_t maxSearchBytes
    )
    {
        if (!startAddress || !asmCodeInfo.first || asmCodeInfo.second == 0 || asmSliceInfo.empty() || maxSearchBytes == 0)
            return nullptr;

        unsigned char* matchedAddress{ nullptr };
        size_t currentMaxSearchBytes{ 0 };
        size_t matchedBytesLength{ 0 };
        size_t sliceIndex{ 0 };

        auto code = asmCodeInfo.first;
        auto codeLen = asmCodeInfo.second;

        while (matchedBytesLength < codeLen)
        {
            auto [sectionLen, gapLen] = asmSliceInfo[sliceIndex];

            if (sectionLen)
            {
                if (sliceIndex == 0)
                {
                    while (std::memcmp(startAddress, code + matchedBytesLength, sectionLen) != 0)
                    {
                        ++startAddress;
                        if (--maxSearchBytes == 0)
                            return nullptr;
                    }
                }
                else if (std::memcmp(startAddress, code + matchedBytesLength, sectionLen) != 0)
                {
                    startAddress = matchedAddress + 1;
                    if (--maxSearchBytes == 0)
                        return nullptr;

                    matchedAddress = nullptr;
                    matchedBytesLength = 0;
                    sliceIndex = 0;
                    maxSearchBytes = currentMaxSearchBytes;
                    continue;
                }

                if (sliceIndex == 0)
                {
                    matchedAddress = startAddress;
                    currentMaxSearchBytes = maxSearchBytes;
                }
            }

            startAddress += sectionLen + gapLen;
            maxSearchBytes -= std::min(maxSearchBytes, sectionLen + gapLen);
            matchedBytesLength += sectionLen + gapLen;
            ++sliceIndex;

            if (maxSearchBytes == 0 || sliceIndex >= asmSliceInfo.size())
                break;
        }

        return matchedAddress;
    }

    std::pair<unsigned char*, size_t> GetTextSectionInfo(void* baseAddress)
    {
        if (!baseAddress)
            return { nullptr, 0 };

        auto nt = ImageNtHeader(baseAddress);
        if (!nt)
            return { nullptr, 0 };

        auto sectionCount = nt->FileHeader.NumberOfSections;
        auto section = IMAGE_FIRST_SECTION(nt);

        for (WORD i = 0; i < sectionCount; ++i, ++section)
        {
            if (std::strcmp(reinterpret_cast<const char*>(section->Name), ".text") == 0)
            {
                auto addr = reinterpret_cast<unsigned char*>(
                    reinterpret_cast<UINT_PTR>(baseAddress) + section->VirtualAddress
                    );
                return { addr, section->Misc.VirtualSize };
            }
        }

        return { nullptr, 0 };
    }

    //
    // --------------------------- VTable helpers ----------------------------
    //

    static int __stdcall CaptureVTableOffset(size_t* out, EXCEPTION_POINTERS* ep)
    {
        if (out && ep && ep->ExceptionRecord && ep->ExceptionRecord->NumberParameters >= 2)
            *out = static_cast<size_t>(ep->ExceptionRecord->ExceptionInformation[1]);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    size_t InvokeAccessViolation(void(*vtable_function)(void*))
    {
        struct Fake
        {
            const void* vtable{ nullptr };
        } fake{};

        size_t offset = static_cast<size_t>(-1);

        __try
        {
            vtable_function(&fake);
        }
        __except (CaptureVTableOffset(&offset, GetExceptionInformation()))
        {
        }

        return offset / sizeof(void*);
    }

    //
    // --------------------------- ImageMapper -------------------------------
    //

    ImageMapper::ImageMapper(std::wstring_view dllPath)
    {
        if (dllPath.empty())
            return;

        m_fileHandle.reset(
            CreateFileW(
                dllPath.data(),
                GENERIC_READ,
                FILE_SHARE_READ,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            )
        );
        if (!m_fileHandle)
            return;

        m_fileMapping.reset(
            CreateFileMappingW(
                m_fileHandle.get(),
                nullptr,
                PAGE_READONLY | SEC_IMAGE,
                0,
                0,
                nullptr
            )
        );
        if (!m_fileMapping)
            return;

        m_baseAddress.reset(
            MapViewOfFile(
                m_fileMapping.get(),
                FILE_MAP_READ,
                0,
                0,
                0
            )
        );
    }

    //
    // ------------------------------ Detours API ----------------------------
    //

    HRESULT Detours::Begin()
    {
        DetourSetIgnoreTooSmall(TRUE);
        RETURN_IF_WIN32_ERROR(DetourTransactionBegin());
        RETURN_IF_WIN32_ERROR(DetourUpdateThread(GetCurrentThread()));
        return S_OK;
    }

    HRESULT Detours::End(bool commit)
    {
        return commit
            ? HRESULT_FROM_WIN32(DetourTransactionCommit())
            : HRESULT_FROM_WIN32(DetourTransactionAbort());
    }

    HRESULT Detours::Write(const std::function<void()>& callback) noexcept
    {
        if (!callback)
            return E_INVALIDARG;

        HRESULT hr = Begin();
        if (FAILED(hr))
            return hr;

        try
        {
            callback();
            return End(true);
        }
        catch (...)
        {
            End(false);
            return wil::ResultFromCaughtException();
        }
    }

    void Detours::Attach(std::string_view dllName, std::string_view funcName, void** realFuncAddr, void* hookFuncAddr)
    {
        THROW_HR_IF_NULL(E_INVALIDARG, realFuncAddr);
        THROW_HR_IF_NULL(E_INVALIDARG, hookFuncAddr);

        *realFuncAddr = DetourFindFunction(dllName.data(), funcName.data());
        THROW_LAST_ERROR_IF_NULL(*realFuncAddr);

        Attach(realFuncAddr, hookFuncAddr);
    }

    void Detours::Attach(void** realFuncAddr, void* hookFuncAddr)
    {
        THROW_HR_IF_NULL(E_INVALIDARG, realFuncAddr);
        THROW_HR_IF_NULL(E_INVALIDARG, *realFuncAddr);
        THROW_HR_IF_NULL(E_INVALIDARG, hookFuncAddr);

        THROW_IF_WIN32_ERROR(DetourAttach(realFuncAddr, hookFuncAddr));
    }

    void Detours::Detach(void** realFuncAddr, void* hookFuncAddr)
    {
        THROW_HR_IF_NULL(E_INVALIDARG, realFuncAddr);
        THROW_HR_IF_NULL(E_INVALIDARG, *realFuncAddr);
        THROW_HR_IF_NULL(E_INVALIDARG, hookFuncAddr);

        THROW_IF_WIN32_ERROR(DetourDetach(realFuncAddr, hookFuncAddr));
    }
}
