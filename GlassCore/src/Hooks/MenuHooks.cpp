#include "CorePch.h"
#include <Hooks/MenuHooks.h>
#include <Core/Intercept/HookHelper.h>
#include <Helpers/RegHelper.h>

#include <detours.h>

namespace GlassCore::Hooks::MenuHooks
{
    namespace
    {
        using OffsetStorage = GlassCore::Hooks::OffsetStorage;

#pragma data_seg(".shared")
        OffsetStorage g_GetBackgroundColorForAppUserModelId_Offset{};
        OffsetStorage g_CreateStoreIcon_FlagsData_Offset{};
#pragma data_seg()
#pragma comment(linker, "/SECTION:.shared,RWS")

#ifdef _WIN64
        constexpr BYTE g_CreateStoreIcon_FlagsOpCode[] =
        {
            0x41, 0xB8,
            0x04, 0x00, 0x00, 0x20,
            0x48, 0x8B, 0xD7
        };
        constexpr DWORD64 g_opCodeFlagsDataOffset{ 2 };
#else
        constexpr BYTE g_CreateStoreIcon_FlagsOpCode[] =
        {
            0x68,
            0x04, 0x00, 0x00, 0x20,
            0xFF, 0x75, 0x0C
        };
        constexpr DWORD64 g_opCodeFlagsDataOffset{ 1 };
#endif

        wil::srwlock g_lock{};
        bool g_iconHooksEnabled{ false };
        PVOID g_actualGetBackgroundColorForAppUserModelId{ nullptr };

        HRESULT WINAPI MyGetBackgroundColorForAppUserModelId(
            PCWSTR /*pszItem*/,
            COLORREF* color)
        {
            if (color)
                *color = 0;
            return E_FAIL;
        }

        bool IsApiOffsetReady() noexcept
        {
            return g_GetBackgroundColorForAppUserModelId_Offset.IsValid();
        }

        void ScanCreateStoreIconFlags(HMODULE shell32Module, OffsetStorage createStoreIconOffset)
        {
            if (!shell32Module || !createStoreIconOffset.IsValid())
                return;

            auto* functionBytes = reinterpret_cast<UCHAR*>(createStoreIconOffset.To(shell32Module));
            if (!functionBytes)
                return;

            for (int i = 0; i < 65535; ++i)
            {
                if (memcmp(&functionBytes[i], g_CreateStoreIcon_FlagsOpCode, sizeof(g_CreateStoreIcon_FlagsOpCode)) != 0)
                    continue;

                g_CreateStoreIcon_FlagsData_Offset = OffsetStorage::From<void*>(
                    static_cast<void*>(shell32Module),
                    static_cast<void*>(&functionBytes[i + g_opCodeFlagsDataOffset]));
                break;
            }
        }

        void TryResolveOffsetsFromShell32()
        {
            if (IsApiOffsetReady())
                return;

            g_CreateStoreIcon_FlagsData_Offset.value = static_cast<long long>(
                Shared::RegHelper::__Get<unsigned long long>({}, L"CreateStoreIconFlagsData_Offset", 0ull));
            g_GetBackgroundColorForAppUserModelId_Offset.value = static_cast<long long>(
                Shared::RegHelper::__Get<unsigned long long>({}, L"GetBackgroundColorForAppUserModelId_Offset", 0ull));

            if (IsApiOffsetReady())
                return;

            if (Shared::RegHelper::Get<DWORD>({ L"Menu" }, L"NoModernAppBackgroundColor", 1) == 0)
                return;

            wil::unique_hmodule shell32Module{ LoadLibraryW(L"shell32.dll") };
            if (!shell32Module)
                return;

            const auto createOffset = Shared::RegHelper::__TryGet<unsigned long long>(
                {}, L"CreateStoreIcon_Offset");
            if (createOffset.has_value())
            {
                OffsetStorage createStoreIcon{};
                createStoreIcon.value = static_cast<long long>(createOffset.value());
                ScanCreateStoreIconFlags(shell32Module.get(), createStoreIcon);
            }
        }

        void EnableIconHooksInternal(bool enable)
        {
            auto lock = g_lock.lock_exclusive();
            const auto flagsDataAddress = g_CreateStoreIcon_FlagsData_Offset.To(GetModuleHandleW(L"shell32.dll"));

            if (enable && !g_iconHooksEnabled)
            {
                if (flagsDataAddress)
                {
                    WriteMemory(flagsDataAddress, [&]()
                    {
                        constexpr DWORD flags{ SIIGBF_BIGGERSIZEOK };
                        memcpy_s(flagsDataAddress, sizeof(flags), &flags, sizeof(flags));
                    });
                }

                if (g_actualGetBackgroundColorForAppUserModelId)
                {
                    Detours::Write([&]()
                    {
                        Detours::Attach(
                            &g_actualGetBackgroundColorForAppUserModelId,
                            reinterpret_cast<void*>(MyGetBackgroundColorForAppUserModelId));
                    });
                    g_iconHooksEnabled = true;
                }
            }

            if (!enable && g_iconHooksEnabled)
            {
                if (flagsDataAddress)
                {
                    WriteMemory(flagsDataAddress, [&]()
                    {
                        constexpr DWORD flags{ 0x20000004u };
                        memcpy_s(flagsDataAddress, sizeof(flags), &flags, sizeof(flags));
                    });
                }

                if (g_actualGetBackgroundColorForAppUserModelId)
                {
                    Detours::Write([&]()
                    {
                        Detours::Detach(
                            &g_actualGetBackgroundColorForAppUserModelId,
                            reinterpret_cast<void*>(MyGetBackgroundColorForAppUserModelId));
                    });
                }

                g_iconHooksEnabled = false;
            }
        }
    }

    void Prepare() noexcept
    {
        TryResolveOffsetsFromShell32();
    }

    void Startup() noexcept
    {
        g_actualGetBackgroundColorForAppUserModelId =
            g_GetBackgroundColorForAppUserModelId_Offset.To(GetModuleHandleW(L"shell32.dll"));
        SyncIconHooksFromRegistry();
    }

    void Shutdown() noexcept
    {
        EnableIconHooksInternal(false);
        g_actualGetBackgroundColorForAppUserModelId = nullptr;
    }

    void SyncIconHooksFromRegistry() noexcept
    {
        if (!IsApiOffsetReady())
        {
            TryResolveOffsetsFromShell32();
            g_actualGetBackgroundColorForAppUserModelId =
                g_GetBackgroundColorForAppUserModelId_Offset.To(GetModuleHandleW(L"shell32.dll"));
        }

        const bool removeBg = Shared::RegHelper::Get<DWORD>({ L"Menu" }, L"NoModernAppBackgroundColor", 1) != 0;
        EnableIconHooksInternal(removeBg && g_actualGetBackgroundColorForAppUserModelId != nullptr);
    }
}
