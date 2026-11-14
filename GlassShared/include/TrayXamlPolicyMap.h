#pragma once

// Snapshot Tray\BorderDrawMode + Tray\Xaml (+ Popup/Menu) для AppContainer (SEH/SMEH/ShellHost).
// Хост (rundll32) пишет; UICompat читает раньше HKCU / HKEY_USERS.
// См. TaskbarCompat/docs/TaskbarBorders.md (CoreWindow / UICompat).

#include <Windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <TrayBorderDrawMode.h>
#include <TrayXamlPaint.h>
#include <Helpers/BorderDisplayHelper.h>
#include <Helpers/ThemeHelper.h>

#pragma comment(lib, "Advapi32.lib")

namespace Shared::TrayXamlPolicyMap
{
    // v4: + SurfaceSlot Tooltip (тултипы Пуска).
    inline constexpr wchar_t kMappingName[] = L"Local\\GlassPlatform.TrayXamlPolicy.v4";

    inline constexpr DWORD kMagic   = 0x50584C47u; // 'GLXP'
    // v4: + SurfaceSlot Tooltip.
    inline constexpr DWORD kVersion = 4u;

    inline constexpr DWORD kFlagGradientPresent = 1u << 0;
    inline constexpr DWORD kFlagDarkMode        = 1u << 1;
    inline constexpr DWORD kFlagNoBorder        = 1u << 2;
    inline constexpr DWORD kFlagCornersPresent  = 1u << 3;
    inline constexpr DWORD kFlagPatchBorder     = 1u << 4;

#pragma pack(push, 4)
    // Один набор Effect/Gradient/Border/Corner (bar или surface).
    struct SurfaceSlot
    {
        DWORD effectType;
        DWORD tintArgb;
        DWORD flags; // gradient / noBorder / corners / patchBorder (+ dark на bar)
        DWORD cornerType;
        DWORD borderArgb;
    };

    struct Snapshot
    {
        DWORD magic;
        DWORD version;
        DWORD borderDrawMode; // 0 DWM, 1 XAML
        // Bar = Tray\Xaml (панель / Overflow / InputSwitch).
        DWORD effectType;
        DWORD tintArgb;
        DWORD flags;
        DWORD generation;
        DWORD cornerType;
        DWORD borderArgb;
        DWORD reserved0;
        DWORD reserved1;
        DWORD reserved2;
        SurfaceSlot popup;   // Tray\Xaml\Popup (fallback уже запечён)
        SurfaceSlot menu;    // Tray\Xaml\Menu
        SurfaceSlot tooltip; // Tray\Xaml\Tooltip (v4)
    };
#pragma pack(pop)

    static_assert(sizeof(SurfaceSlot) == 20, "TrayXamlPolicy SurfaceSlot size");
    static_assert(sizeof(Snapshot) == 108, "TrayXamlPolicy Snapshot size (v4: +tooltip)");

    namespace detail
    {
        // WD=Everyone, AC=ALL APPLICATION PACKAGES — AppContainer читает Local\ mapping.
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

        inline void FillSurfaceSlot(
            SurfaceSlot& slot,
            const wchar_t* bwmSubKey,
            bool dark,
            bool includeDarkFlag) noexcept
        {
            slot = {};
            slot.effectType = TrayXamlPaint::ReadEffectTypeFor(bwmSubKey);
            if (includeDarkFlag && dark)
                slot.flags |= kFlagDarkMode;

            DWORD tint = 0;
            if (TrayXamlPaint::TryReadGradientArgbFor(bwmSubKey, dark, tint))
            {
                slot.flags |= kFlagGradientPresent;
                slot.tintArgb = tint;
            }
            else
            {
                slot.tintArgb = dark ? 0xE42C2C2Cu : 0xE4F3F3F3u;
            }

            DWORD corner = TrayXamlPaint::kDefaultCornerTypeXaml;
            if (TrayXamlPaint::TryReadCornerTypeFor(bwmSubKey, corner))
            {
                slot.cornerType = TrayXamlPaint::ClampCornerTypeXaml(corner);
                if (slot.cornerType != 0)
                    slot.flags |= kFlagCornersPresent;
            }
            else
            {
                slot.cornerType = TrayXamlPaint::kDefaultCornerTypeXaml;
                slot.flags |= kFlagCornersPresent;
            }

            if (TrayXamlPaint::ReadNoBorderColorFor(bwmSubKey, false))
            {
                slot.flags |= kFlagNoBorder;
                slot.borderArgb = 0;
            }
            else
            {
                DWORD border = 0;
                if (TrayXamlPaint::TryReadBorderArgbFor(bwmSubKey, dark, border))
                {
                    slot.borderArgb = border;
                    slot.flags |= kFlagPatchBorder;
                }
                else if (TrayXamlPaint::TryReadCachedSystemStrokeArgb(border))
                {
                    // Паритет TaskbarCompat: CachedSystemStroke > ChartView (синий акцент).
                    slot.borderArgb = border;
                    slot.flags |= kFlagPatchBorder;
                }
                else if (slot.cornerType != 0)
                {
                    const auto ui =
                        BorderDisplayHelper::QueryForBwmSubKey(bwmSubKey, dark);
                    if (ui.displayArgb == 0 || ui.colorUseNone)
                    {
                        // ChartView на пустом surface — bar.
                        const auto barUi =
                            BorderDisplayHelper::QueryForBwmSubKey(L"Tray\\Xaml", dark);
                        if (barUi.displayArgb != 0 && !barUi.colorUseNone)
                        {
                            slot.borderArgb = barUi.displayArgb;
                            slot.flags |= kFlagPatchBorder;
                        }
                    }
                    else
                    {
                        slot.borderArgb = ui.displayArgb;
                        slot.flags |= kFlagPatchBorder;
                    }
                }
            }
        }

        [[nodiscard]] inline bool FillFromRegistry(Snapshot& out) noexcept
        {
            out = {};
            out.magic = kMagic;
            out.version = kVersion;
            out.borderDrawMode = static_cast<DWORD>(TrayBorderDrawMode::ReadFromRegistry());

            const bool dark = ThemeHelper::IsTrayXamlDarkMode();
            SurfaceSlot bar{};
            FillSurfaceSlot(bar, TrayXamlPaint::kXamlSubKey, dark, true);
            out.effectType = bar.effectType;
            out.tintArgb = bar.tintArgb;
            out.flags = bar.flags;
            out.cornerType = bar.cornerType;
            out.borderArgb = bar.borderArgb;

            FillSurfaceSlot(out.popup, TrayXamlPaint::kXamlPopupSubKey, dark, false);
            // Dark только на bar.flags — surface наследует при чтении.
            if (dark)
                out.popup.flags |= kFlagDarkMode;

            FillSurfaceSlot(out.menu, TrayXamlPaint::kXamlMenuSubKey, dark, false);
            if (dark)
                out.menu.flags |= kFlagDarkMode;

            FillSurfaceSlot(out.tooltip, TrayXamlPaint::kXamlTooltipSubKey, dark, false);
            if (dark)
                out.tooltip.flags |= kFlagDarkMode;

            return true;
        }

        // Mapping мог быть создан v1/v2 — пересоздать под sizeof(Snapshot).
        [[nodiscard]] inline bool EnsureMappingSize(
            HANDLE* ioHostMapping,
            SECURITY_ATTRIBUTES* pSa) noexcept
        {
            if (!ioHostMapping)
                return false;

            if (*ioHostMapping)
            {
                void* view = MapViewOfFile(
                    *ioHostMapping, FILE_MAP_READ, 0, 0, sizeof(Snapshot));
                if (view)
                {
                    const auto* src = static_cast<const Snapshot*>(view);
                    const bool ok = src->magic == kMagic && src->version == kVersion;
                    UnmapViewOfFile(view);
                    if (ok)
                        return true;
                }
                CloseHandle(*ioHostMapping);
                *ioHostMapping = nullptr;
            }

            *ioHostMapping = CreateFileMappingW(
                INVALID_HANDLE_VALUE,
                pSa,
                PAGE_READWRITE,
                0,
                sizeof(Snapshot),
                kMappingName);
            return *ioHostMapping != nullptr;
        }
    }

    // Хост: создать/обновить snapshot. Держит именованный объект через *ioHostMapping.
    inline bool Publish(HANDLE* ioHostMapping) noexcept
    {
        if (!ioHostMapping)
            return false;

        Snapshot snap{};
        (void)detail::FillFromRegistry(snap);

        SECURITY_ATTRIBUTES sa{};
        PSECURITY_DESCRIPTOR sd = nullptr;
        SECURITY_ATTRIBUTES* pSa = detail::MakeSa(sa, sd) ? &sa : nullptr;

        if (!detail::EnsureMappingSize(ioHostMapping, pSa))
        {
            if (sd)
                LocalFree(sd);
            return false;
        }

        if (sd)
            LocalFree(sd);

        HANDLE hMap = *ioHostMapping;
        void* view = MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, sizeof(Snapshot));
        if (!view)
            return false;

        auto* dest = static_cast<Snapshot*>(view);
        snap.generation = dest->magic == kMagic ? dest->generation + 1u : 1u;
        *dest = snap;
        UnmapViewOfFile(view);
        return true;
    }

    inline void CloseHost(HANDLE* ioHostMapping) noexcept
    {
        if (!ioHostMapping || !*ioHostMapping)
            return;
        CloseHandle(*ioHostMapping);
        *ioHostMapping = nullptr;
    }

    // UICompat / любой клиент: FILE_MAP_READ.
    [[nodiscard]] inline bool TryRead(Snapshot& out) noexcept
    {
        out = {};
        HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, kMappingName);
        if (!hMap)
            return false;

        void* view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(Snapshot));
        CloseHandle(hMap);
        if (!view)
            return false;

        const auto* src = static_cast<const Snapshot*>(view);
        const bool ok = src->magic == kMagic && src->version == kVersion;
        if (ok)
            out = *src;
        UnmapViewOfFile(view);
        return ok;
    }

    // Клиент (GS / GlassPlatformTask): обновить snapshot до SignalReloadRender.
    // Не держит mapping — объект живёт за счёт handle хоста.
    [[nodiscard]] inline bool TryRepublishFromRegistry() noexcept
    {
        HANDLE hMap = OpenFileMappingW(FILE_MAP_WRITE, FALSE, kMappingName);
        if (!hMap)
            return false;

        Snapshot snap{};
        (void)detail::FillFromRegistry(snap);

        void* view = MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, sizeof(Snapshot));
        if (!view)
        {
            CloseHandle(hMap);
            return false;
        }

        auto* dest = static_cast<Snapshot*>(view);
        snap.generation = dest->magic == kMagic ? dest->generation + 1u : 1u;
        *dest = snap;
        UnmapViewOfFile(view);
        CloseHandle(hMap);
        return true;
    }
}
