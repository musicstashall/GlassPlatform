#pragma once

#include <Windows.h>
#include <cstdint>

namespace Shared::InjectModuleScan
{
    /// Совпадает с InjectProcessMap::ModuleBits.
    enum ModuleBits : std::uint32_t
    {
        ModuleCompat = 1u << 0,
        ModuleCore = 1u << 1,
        ModuleWnd = 1u << 2,
        ModuleExplorer = 1u << 3,
        ModuleTaskbar = 1u << 4,
        ModuleUICompat = 1u << 5,
    };

    inline constexpr DWORD kMaxResults = 512u;
    inline constexpr DWORD kExeNameChars = 64u;

    // Optional high bit of GlassSettings_ScanGlassModules' maxResults argument.
    // Installer callers use the existing Native export for a strict, local-arch
    // scan; the remaining bits are the actual capacity. Ordinary GlassSettings
    // callers omit it and retain their existing cross-architecture behaviour.
    inline constexpr DWORD kNativeLocalInstallerScan = 0x80000000u;
    // Capability handshake: call the Native export with null results, zero
    // capacity plus the option above, and a valid outCount. Older DLLs return
    // FALSE; supported DLLs return TRUE and this value without scanning.
    inline constexpr DWORD kNativeInstallerScanProtocol = 0x31495347u; // 'GSI1'
    // A versioned alias of the SAME scan export, not a second scanner. Setup
    // inspects the PE export table before stopping hosts or changing MSI, so an
    // older embedded DLL is rejected without executing it or touching products.
    inline constexpr char kNativeInstallerProtocolExport[] = "GlassSettings_InstallerScanProtocolV1";

    struct Result
    {
        DWORD pid;
        std::uint32_t moduleBits;
        wchar_t exeName[kExeNameChars];
    };

    /// Скан только указанных PID (лёгкий режим при живой карте).
    /// pidsCount==0 — полный Process32 + Module32 (после стопа).
    [[nodiscard]] bool Scan(
        const DWORD* pids,
        DWORD pidsCount,
        Result* outResults,
        DWORD maxResults,
        DWORD* outCount) noexcept;

    /// Installer safety scan: report an uninspectable live Explorer or explicitly
    /// requested PID, or a truncated result, instead of treating it as empty.
    [[nodiscard]] bool ScanForSetup(
        const DWORD* pids,
        DWORD pidsCount,
        Result* outResults,
        DWORD maxResults,
        DWORD* outCount) noexcept;

    /// GlassCompat residual в known hosts после стопа (!mutex).
    /// Не вызывать как блок меню при живой платформе — тогда Compat в explorer/SEH нормален.
    [[nodiscard]] bool HasGlassCompatInKnownHosts() noexcept;

    /// Known shell/CW/Widget hosts с GlassCompat той же разрядности, что arch.
    [[nodiscard]] bool HasGlassCompatResidualForArch(bool wantWow64) noexcept;

    /// Named sticky PID map: Local\GlassPlatform.InjectSticky.x64 / .x86
    inline constexpr wchar_t kStickyMappingNameX64[] = L"Local\\GlassPlatform.InjectSticky.x64";
    inline constexpr wchar_t kStickyMappingNameX86[] = L"Local\\GlassPlatform.InjectSticky.x86";
    inline constexpr DWORD kStickyMaxPids = 64u;

    /// Скан known hosts → обновить sticky; true если есть residual bits.
    [[nodiscard]] bool RefreshInjectResidualSticky(bool wantWow64) noexcept;

    /// Число PID в sticky (без скана). 0 если map нет.
    [[nodiscard]] DWORD ReadInjectResidualStickyCount(bool wantWow64) noexcept;
}
