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

    // Optional high bits of GlassSettings_ScanGlassModules' maxResults argument.
    // Installer callers use the existing Native export for a strict, local-arch
    // scan; the remaining bits are the actual capacity. Ordinary GlassSettings
    // callers omit them and retain their existing cross-architecture behaviour.
    inline constexpr DWORD kNativeLocalInstallerScan = 0x80000000u;
    // Strict scan + ScanOptionDeferForeignExplorer (see below). Only valid after
    // the 'GSI2' handshake: an older DLL would read the bit as buffer capacity.
    inline constexpr DWORD kNativeDeferForeignExplorer = 0x40000000u;
    inline constexpr DWORD kNativeOptionMask = kNativeLocalInstallerScan | kNativeDeferForeignExplorer;
    // Capability handshake: call the Native export with null results, zero
    // capacity plus kNativeLocalInstallerScan, and a valid outCount. Older DLLs
    // return FALSE or 'GSI1'; supported DLLs return TRUE and this value without
    // scanning. 'GSI2' = defer option + GlassSettings_GetLastScanDiag export.
    inline constexpr DWORD kNativeInstallerScanProtocol = 0x32495347u; // 'GSI2'
    // A versioned alias of the SAME scan export, not a second scanner. Setup
    // inspects the PE export table before stopping hosts or changing MSI, so an
    // older embedded DLL is rejected without executing it or touching products.
    inline constexpr char kNativeInstallerProtocolExport[] = "GlassSettings_InstallerScanProtocolV2";
    // Optional diagnostics export (same DLL generation as the V2 alias).
    inline constexpr char kNativeScanDiagExport[] = "GlassSettings_GetLastScanDiag";

    struct Result
    {
        DWORD pid;
        std::uint32_t moduleBits;
        wchar_t exeName[kExeNameChars];
    };

    /// Options of the strict installer scan (ScanForSetup).
    enum ScanOptions : std::uint32_t
    {
        ScanOptionNone = 0,
        /// Caller runs under WOW64 (32-bit scanner on 64-bit Windows). There
        /// explorer.exe is always native x64 and is inspected by the native x64
        /// strict scan that the installer runs alongside this one. If the 32-bit
        /// side cannot even open it (OpenProcess / IsWow64Process), it records a
        /// note (ScanDiagDeferred) and continues instead of failing; the native
        /// scan still fails closed on its own. No effect for native callers.
        ScanOptionDeferForeignExplorer = 1u << 0,
    };

    /// Where the last strict scan on this thread failed or left a note.
    enum class ScanDiagStage : std::uint32_t
    {
        None = 0,
        ProcessSnapshot,   // CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS) / Process32First
        ProcessNext,       // Process32Next ended with an error other than ERROR_NO_MORE_FILES
        OpenProcess,       // OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) failed
        IsWow64,           // IsWow64Process(target) failed
        ModuleSnapshot,    // CreateToolhelp32Snapshot(TH32CS_SNAPMODULE*) failed after retries
        ModuleEnum,        // Module32First / Module32Next failed
        Truncated,         // more holders than maxResults
    };

    enum ScanDiagFlags : std::uint32_t
    {
        ScanDiagExplorer = 1u << 0,        // the process is explorer.exe
        ScanDiagCallerWow64 = 1u << 1,     // the scanner itself runs under WOW64
        ScanDiagDeferred = 1u << 2,        // not fatal: left to the native x64 scan (ScanOptionDeferForeignExplorer)
        ScanDiagDebugPrivilege = 1u << 3,  // SeDebugPrivilege was enabled and OpenProcess retried
        ScanDiagFatal = 1u << 4,           // this entry is why the scan returned false
    };

    /// Fixed layout: also copied through the GlassSettings_GetLastScanDiag export.
    struct ScanDiag
    {
        ScanDiagStage stage;
        std::uint32_t flags;      // ScanDiagFlags
        DWORD pid;
        DWORD error;              // GetLastError() of the failing call
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
    /// On ERROR_ACCESS_DENIED the scanner enables SeDebugPrivilege once (if the
    /// token holds it) and retries OpenProcess. Details: GetLastScanDiag().
    [[nodiscard]] bool ScanForSetup(
        const DWORD* pids,
        DWORD pidsCount,
        Result* outResults,
        DWORD maxResults,
        DWORD* outCount,
        std::uint32_t options = ScanOptionNone) noexcept;

    /// Diagnostics of the most recent ScanForSetup on the calling thread: the
    /// fatal failure (ScanDiagFatal), otherwise the first deferred note,
    /// otherwise stage == None. Reset at the start of every strict scan.
    void GetLastScanDiag(ScanDiag& out) noexcept;

    /// "stage=OpenProcess pid=1234 exe=explorer.exe gle=5 flags=explorer,wow64,deferred"
    /// (or "stage=None"). Returns the number of characters written, 0 on error.
    DWORD FormatScanDiag(const ScanDiag& diag, wchar_t* buffer, DWORD capacity) noexcept;

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
