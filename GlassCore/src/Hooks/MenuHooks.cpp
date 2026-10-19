#include "CorePch.h"
#include <Hooks/MenuHooks.h>
#include <Core/Intercept/HookHelper.h>
#include <Helpers/RegHelper.h>

#include <algorithm>

namespace GlassCore::Hooks::MenuHooks
{
    namespace
    {
#ifdef _WIN64
        constexpr BYTE g_CreateStoreIcon_FlagsOpCode[] =
        {
            0x41, 0xB8,
            0x04, 0x00, 0x00, 0x20,
            0x48, 0x8B, 0xD7
        };
        constexpr size_t g_opCodeFlagsDataOffset{ 2 };
#else
        constexpr BYTE g_CreateStoreIcon_FlagsOpCode[] =
        {
            0x68,
            0x04, 0x00, 0x00, 0x20,
            0xFF, 0x75, 0x0C
        };
        constexpr size_t g_opCodeFlagsDataOffset{ 1 };
#endif

        wil::srwlock g_lock{};
        bool g_shell32Scanned{ false };
        bool g_iconHooksEnabled{ false };
        bool g_flagsPatched{ false };
        bool g_colorDetourAttached{ false };
        uintptr_t g_backgroundColorRva{ 0 };
        uintptr_t g_flagsImmediateRva{ 0 };
        PVOID g_actualGetBackgroundColorForAppUserModelId{ nullptr };

        HRESULT WINAPI MyGetBackgroundColorForAppUserModelId(
            PCWSTR /*pszItem*/,
            COLORREF* color)
        {
            if (color)
                *color = 0;
            return E_FAIL;
        }

        [[nodiscard]] void* RvaToPtr(HMODULE module, uintptr_t rva) noexcept
        {
            if (!module || rva == 0)
                return nullptr;
            return reinterpret_cast<BYTE*>(module) + rva;
        }

        template <typename Fn>
        void ForEachExecutableSpan(HMODULE module, Fn&& visitor) noexcept
        {
            if (!module)
                return;

            const auto* base = reinterpret_cast<const BYTE*>(module);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
                return;

            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
                return;

            const DWORD imageSize = nt->OptionalHeader.SizeOfImage;
            if (imageSize == 0 || static_cast<DWORD>(dos->e_lfanew) >= imageSize)
                return;

            const auto* section = IMAGE_FIRST_SECTION(nt);
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            {
                if ((section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
                    continue;

                const DWORD va = section[i].VirtualAddress;
                const DWORD virtualSize = section[i].Misc.VirtualSize;
                if (va >= imageSize || virtualSize == 0)
                    continue;

                const size_t span = std::min<size_t>(virtualSize, static_cast<size_t>(imageSize - va));
                visitor(base + va, span);
            }
        }

#ifdef _WIN64
        [[nodiscard]] const BYTE* FindUnique(
            HMODULE module,
            size_t window,
            bool (*match)(const BYTE*, const BYTE*) noexcept) noexcept
        {
            const BYTE* found = nullptr;
            int count = 0;
            ForEachExecutableSpan(module, [&](const BYTE* begin, size_t size)
            {
                if (size < window || count > 1)
                    return;

                const BYTE* const end = begin + size;
                for (size_t i = 0; i + window <= size; ++i)
                {
                    if (!match(begin + i, end))
                        continue;
                    found = begin + i;
                    if (++count > 1)
                        return;
                }
            });
            return count == 1 ? found : nullptr;
        }

        // Пролог shell32!GetBackgroundColorForAppUserModelId (10.0.26100): обнуляет COLORREF*
        // и ставит security cookie. Смещение cookie маскируется. Ноль или несколько совпадений — хук не ставится.
        [[nodiscard]] bool MatchBackgroundColorPrologue(const BYTE* p, const BYTE* end) noexcept
        {
            constexpr BYTE kHead[] = {
                0x48, 0x89, 0x5C, 0x24, 0x18,
                0x57,
                0x48, 0x81, 0xEC, 0x70, 0x01, 0x00, 0x00
            };
            constexpr BYTE kAfterCookie[] = {
                0x48, 0x33, 0xC4,
                0x48, 0x89, 0x84, 0x24, 0x60, 0x01, 0x00, 0x00,
                0x83, 0x22, 0x00
            };
            constexpr size_t kCookie = sizeof(kHead);
            constexpr size_t kTotal = kCookie + 7 + sizeof(kAfterCookie);
            if (static_cast<size_t>(end - p) < kTotal)
                return false;
            if (memcmp(p, kHead, sizeof(kHead)) != 0)
                return false;
            if (p[kCookie] != 0x48 || p[kCookie + 1] != 0x8B || p[kCookie + 2] != 0x05)
                return false;
            return memcmp(p + kCookie + 7, kAfterCookie, sizeof(kAfterCookie)) == 0;
        }
#endif

        void TryResolveShell32Sites() noexcept
        {
            if (g_shell32Scanned)
                return;

            if (Shared::RegHelper::Get<DWORD>({ L"Menu" }, L"NoModernAppBackgroundColor", 1) == 0)
                return;

            HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
            wil::unique_hmodule loaded{};
            if (!shell32)
            {
                loaded.reset(LoadLibraryW(L"shell32.dll"));
                shell32 = loaded.get();
            }
            if (!shell32)
                return;

            g_shell32Scanned = true;
            const auto* base = reinterpret_cast<const BYTE*>(shell32);

#ifdef _WIN64
            if (const BYTE* hit = FindUnique(shell32, 34, &MatchBackgroundColorPrologue))
            {
                g_backgroundColorRva = static_cast<uintptr_t>(hit - base);
                DebugLog(L"[GlassCore][MenuHooks] GetBackgroundColor RVA=%llx",
                    static_cast<unsigned long long>(g_backgroundColorRva));
            }
            else
            {
                DebugLog(L"[GlassCore][MenuHooks] GetBackgroundColor signature miss — detour off");
            }
#else
            DebugLog(L"[GlassCore][MenuHooks] GetBackgroundColor signature is x64-only — detour off");
#endif

            const BYTE* flagsHit = nullptr;
            int flagsCount = 0;
            ForEachExecutableSpan(shell32, [&](const BYTE* begin, size_t size)
            {
                if (size < sizeof(g_CreateStoreIcon_FlagsOpCode) || flagsCount > 1)
                    return;

                for (size_t i = 0; i + sizeof(g_CreateStoreIcon_FlagsOpCode) <= size; ++i)
                {
                    if (memcmp(begin + i, g_CreateStoreIcon_FlagsOpCode, sizeof(g_CreateStoreIcon_FlagsOpCode)) != 0)
                        continue;
                    flagsHit = begin + i;
                    if (++flagsCount > 1)
                        return;
                }
            });

            if (flagsCount == 1 && flagsHit)
            {
                g_flagsImmediateRva = static_cast<uintptr_t>(
                    flagsHit + g_opCodeFlagsDataOffset - base);
                DebugLog(L"[GlassCore][MenuHooks] CreateStoreIcon flags RVA=%llx",
                    static_cast<unsigned long long>(g_flagsImmediateRva));
            }
            else
            {
                DebugLog(L"[GlassCore][MenuHooks] CreateStoreIcon flags signature miss (count=%d)", flagsCount);
            }
        }

        void EnableIconHooksInternal(bool enable)
        {
            auto lock = g_lock.lock_exclusive();
            HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
            void* const flagsDataAddress = RvaToPtr(shell32, g_flagsImmediateRva);
            if (!g_actualGetBackgroundColorForAppUserModelId)
                g_actualGetBackgroundColorForAppUserModelId = RvaToPtr(shell32, g_backgroundColorRva);

            if (enable && !g_iconHooksEnabled)
            {
                if (flagsDataAddress)
                {
                    WriteMemory(flagsDataAddress, [&]()
                    {
                        constexpr DWORD flags{ SIIGBF_BIGGERSIZEOK };
                        memcpy_s(flagsDataAddress, sizeof(flags), &flags, sizeof(flags));
                    });
                    g_flagsPatched = true;
                }

                if (g_actualGetBackgroundColorForAppUserModelId)
                {
                    Detours::Write([&]()
                    {
                        Detours::Attach(
                            &g_actualGetBackgroundColorForAppUserModelId,
                            reinterpret_cast<void*>(MyGetBackgroundColorForAppUserModelId));
                    });
                    g_colorDetourAttached = true;
                }

                g_iconHooksEnabled = g_flagsPatched || g_colorDetourAttached;
            }

            if (!enable && g_iconHooksEnabled)
            {
                if (g_flagsPatched && flagsDataAddress)
                {
                    WriteMemory(flagsDataAddress, [&]()
                    {
                        constexpr DWORD flags{ 0x20000004u };
                        memcpy_s(flagsDataAddress, sizeof(flags), &flags, sizeof(flags));
                    });
                }
                g_flagsPatched = false;

                if (g_colorDetourAttached && g_actualGetBackgroundColorForAppUserModelId)
                {
                    Detours::Write([&]()
                    {
                        Detours::Detach(
                            &g_actualGetBackgroundColorForAppUserModelId,
                            reinterpret_cast<void*>(MyGetBackgroundColorForAppUserModelId));
                    });
                }
                g_colorDetourAttached = false;
                g_iconHooksEnabled = false;
            }
        }
    }

    void Prepare() noexcept
    {
        TryResolveShell32Sites();
    }

    void Startup() noexcept
    {
        TryResolveShell32Sites();
        if (HMODULE shell32 = GetModuleHandleW(L"shell32.dll"))
        {
            g_actualGetBackgroundColorForAppUserModelId =
                RvaToPtr(shell32, g_backgroundColorRva);
        }
        SyncIconHooksFromRegistry();
    }

    void Shutdown() noexcept
    {
        EnableIconHooksInternal(false);
        g_actualGetBackgroundColorForAppUserModelId = nullptr;
    }

    void SyncIconHooksFromRegistry() noexcept
    {
        TryResolveShell32Sites();
        if (HMODULE shell32 = GetModuleHandleW(L"shell32.dll"))
        {
            if (!g_actualGetBackgroundColorForAppUserModelId)
            {
                g_actualGetBackgroundColorForAppUserModelId =
                    RvaToPtr(shell32, g_backgroundColorRva);
            }
        }

        const bool removeBg = Shared::RegHelper::Get<DWORD>({ L"Menu" }, L"NoModernAppBackgroundColor", 1) != 0;
        const bool haveSite = g_actualGetBackgroundColorForAppUserModelId != nullptr || g_flagsImmediateRva != 0;
        EnableIconHooksInternal(removeBg && haveSite);
    }
}
