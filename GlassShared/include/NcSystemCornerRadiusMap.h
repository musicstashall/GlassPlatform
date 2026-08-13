#pragma once

// Системный CornerRadius CoreWindow NC (UICompat → TaskbarCompat).
// Named mapping: AppContainer SEH пишет, explorer читает (HKCU из AC недоступен).

#include <Windows.h>
#include <aclapi.h>
#include <sddl.h>

#pragma comment(lib, "Advapi32.lib")

namespace Shared::NcSystemCornerRadiusMap
{
    inline constexpr wchar_t kMappingName[] = L"Local\\GlassPlatform.NcSystemCornerRadius";

    inline constexpr DWORD kMagic   = 0x52434E47u; // 'GNCR'
    inline constexpr DWORD kVersion = 1u;

#pragma pack(push, 4)
    struct Snapshot
    {
        DWORD magic;
        DWORD version;
        DWORD centiDip; // R * 100
        DWORD flags;
    };
#pragma pack(pop)

    static_assert(sizeof(Snapshot) == 16, "NcSystemCornerRadius Snapshot size");

    namespace detail
    {
        [[nodiscard]] inline bool MakeSa(SECURITY_ATTRIBUTES& sa, PSECURITY_DESCRIPTOR& sd) noexcept
        {
            sa = {};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = FALSE;
            sd = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    L"D:(A;;GA;;;WD)(A;;GA;;;AC)",
                    SDDL_REVISION_1,
                    &sd,
                    nullptr)
                || !sd)
            {
                return false;
            }
            sa.lpSecurityDescriptor = sd;
            return true;
        }
    }

    inline bool PublishCentiDip(DWORD centiDip) noexcept
    {
        if (centiDip == 0)
            return false;

        SECURITY_ATTRIBUTES sa{};
        PSECURITY_DESCRIPTOR sd = nullptr;
        SECURITY_ATTRIBUTES* pSa = detail::MakeSa(sa, sd) ? &sa : nullptr;

        HANDLE hMap = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            pSa,
            PAGE_READWRITE,
            0,
            sizeof(Snapshot),
            kMappingName);

        if (sd)
            LocalFree(sd);

        if (!hMap)
            return false;

        void* view = MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, sizeof(Snapshot));
        if (!view)
        {
            CloseHandle(hMap);
            return false;
        }

        auto* dest = static_cast<Snapshot*>(view);
        dest->magic = kMagic;
        dest->version = kVersion;
        dest->centiDip = centiDip;
        dest->flags = 1u;
        UnmapViewOfFile(view);
        // Не закрываем hMap: именованный объект должен жить после выхода SEH.
        // Дубликаты CreateFileMapping на то же имя шарят секцию; handle утекает
        // на процесс SEH — приемлемо (один раз на жизнь host DLL).
        // Закрываем handle: объект живёт, пока есть другой opener или пока
        // OpenFileMapping удерживает. Для надёжности держим process-static.
        static HANDLE s_keepAlive = nullptr;
        if (!s_keepAlive)
            s_keepAlive = hMap;
        else
            CloseHandle(hMap);
        return true;
    }

    [[nodiscard]] inline bool TryReadCentiDip(DWORD& outCenti) noexcept
    {
        outCenti = 0;
        HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, kMappingName);
        if (!hMap)
            return false;

        void* view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(Snapshot));
        CloseHandle(hMap);
        if (!view)
            return false;

        const auto* src = static_cast<const Snapshot*>(view);
        const bool ok =
            src->magic == kMagic && src->version == kVersion && src->centiDip > 0u;
        if (ok)
            outCenti = src->centiDip;
        UnmapViewOfFile(view);
        return ok;
    }
}
