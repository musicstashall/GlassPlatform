#include "SharedPch.h"
#include <InjectModuleScan.h>
#include <Helpers/SystemHelper.h>

#include <TlHelp32.h>
#include <cwchar>

namespace Shared::InjectModuleScan
{
    namespace
    {
        constexpr ULONG kWin11Build = 22000u;

        [[nodiscard]] bool IsWin11() noexcept
        {
            return Shared::SystemHelper::g_buildNumber >= kWin11Build;
        }

        void CopyExeName(wchar_t* dest, const wchar_t* src) noexcept
        {
            if (!dest)
                return;
            if (!src || !src[0])
            {
                dest[0] = L'\0';
                return;
            }
            wcsncpy_s(dest, kExeNameChars, src, _TRUNCATE);
        }

        [[nodiscard]] bool IsExplorerExe(const wchar_t* name) noexcept
        {
            return name && _wcsicmp(name, L"explorer.exe") == 0;
        }

        [[nodiscard]] std::uint32_t MatchModuleBits(
            const wchar_t* moduleBase,
            bool allowExplorerModules) noexcept
        {
            if (!moduleBase || !moduleBase[0])
                return 0;
            if (_wcsicmp(moduleBase, L"GlassCompat.dll") == 0)
                return ModuleCompat;
            if (_wcsicmp(moduleBase, L"GlassCore.dll") == 0)
                return ModuleCore;
            if (_wcsicmp(moduleBase, L"WndCompat.dll") == 0)
                return ModuleWnd;
            // SEH/ShellHost/SMEH — не explorer; всё равно сканируем.
            if (_wcsicmp(moduleBase, L"UICompat.dll") == 0)
                return ModuleUICompat;
            if (allowExplorerModules)
            {
                if (_wcsicmp(moduleBase, L"ExplorerCompat.dll") == 0)
                    return ModuleExplorer;
                if (_wcsicmp(moduleBase, L"TaskbarCompat.dll") == 0)
                    return ModuleTaskbar;
            }
            return 0;
        }

        /// WOW64 (GS x86 на OS x64) не может безопасно Module32 по native x64 PID — AV/PARTIAL_COPY.
        [[nodiscard]] bool CanScanProcessModules(DWORD pid) noexcept
        {
#if defined(_WIN64)
            (void)pid;
            return true;
#else
            BOOL selfWow = FALSE;
            if (!::IsWow64Process(::GetCurrentProcess(), &selfWow) || !selfWow)
                return true;

            HANDLE hProc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!hProc)
                return false;

            BOOL targetWow = FALSE;
            const BOOL ok = ::IsWow64Process(hProc, &targetWow);
            ::CloseHandle(hProc);
            return ok == TRUE && targetWow == TRUE;
#endif
        }

        [[nodiscard]] bool ScanProcessModules(
            DWORD pid,
            const wchar_t* exeHint,
            Result& out) noexcept
        {
            out = {};
            out.pid = pid;
            CopyExeName(out.exeName, exeHint);

            if (!CanScanProcessModules(pid))
                return false;

            const bool allowExplorer = IsWin11() && IsExplorerExe(out.exeName[0] ? out.exeName : exeHint);

            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
            if (snap == INVALID_HANDLE_VALUE)
                return false;

            MODULEENTRY32W me{};
            me.dwSize = sizeof(me);
            std::uint32_t bits = 0;
            if (Module32FirstW(snap, &me))
            {
                do
                {
                    bits |= MatchModuleBits(me.szModule, allowExplorer);
                    const std::uint32_t needed = allowExplorer
                        ? (ModuleCompat | ModuleCore | ModuleWnd | ModuleExplorer | ModuleTaskbar
                            | ModuleUICompat)
                        : (ModuleCompat | ModuleCore | ModuleWnd | ModuleUICompat);
                    if ((bits & needed) == needed)
                        break;
                } while (Module32NextW(snap, &me));
            }
            CloseHandle(snap);

            if (bits == 0)
                return false;

            out.moduleBits = bits;
            return true;
        }
    } // namespace

    bool Scan(
        const DWORD* pids,
        DWORD pidsCount,
        Result* outResults,
        DWORD maxResults,
        DWORD* outCount) noexcept
    {
        if (!outResults || maxResults == 0 || !outCount)
            return false;
        *outCount = 0;

        if (pids && pidsCount > 0)
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap == INVALID_HANDLE_VALUE)
                return false;

            // Один Process32 на весь light-scan; Module32 только для PID из карты.
            PROCESSENTRY32W pe{};
            pe.dwSize = sizeof(pe);
            DWORD written = 0;
            const DWORD selfPid = GetCurrentProcessId();
            if (Process32FirstW(snap, &pe))
            {
                do
                {
                    if (written >= maxResults)
                        break;
                    if (pe.th32ProcessID == 0 || pe.th32ProcessID == selfPid)
                        continue;
                    if (_wcsicmp(pe.szExeFile, L"GlassSettings.exe") == 0)
                        continue;

                    bool match = false;
                    for (DWORD i = 0; i < pidsCount; ++i)
                    {
                        if (pids[i] == pe.th32ProcessID)
                        {
                            match = true;
                            break;
                        }
                    }
                    if (!match)
                        continue;

                    Result r{};
                    if (!ScanProcessModules(pe.th32ProcessID, pe.szExeFile, r))
                        continue;
                    outResults[written++] = r;
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
            *outCount = written;
            return true;
        }

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return false;

        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        DWORD written = 0;
        const DWORD selfPid = GetCurrentProcessId();
        if (Process32FirstW(snap, &pe))
        {
            do
            {
                if (pe.th32ProcessID == 0 || written >= maxResults)
                    continue;
                if (pe.th32ProcessID == selfPid)
                    continue;
                if (_wcsicmp(pe.szExeFile, L"GlassSettings.exe") == 0)
                    continue;

                Result r{};
                if (!ScanProcessModules(pe.th32ProcessID, pe.szExeFile, r))
                    continue;
                outResults[written++] = r;
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        *outCount = written;
        return true;
    }

    namespace
    {
        constexpr DWORD kStickyMagic = 0x4B545347u; // 'GSTK' LE
        constexpr DWORD kStickyVersion = 1u;

#pragma pack(push, 4)
        struct StickyHeader
        {
            DWORD magic;
            DWORD version;
            DWORD count;
            DWORD reserved;
            DWORD pids[kStickyMaxPids];
        };
#pragma pack(pop)

        [[nodiscard]] const wchar_t* StickyMappingName(bool wantWow64) noexcept
        {
            return wantWow64 ? kStickyMappingNameX86 : kStickyMappingNameX64;
        }

        [[nodiscard]] bool IsKnownResidualHostExe(const wchar_t* exe) noexcept
        {
            return exe
                && (_wcsicmp(exe, L"explorer.exe") == 0
                    || _wcsicmp(exe, L"ShellHost.exe") == 0
                    || _wcsicmp(exe, L"ShellExperienceHost.exe") == 0
                    || _wcsicmp(exe, L"StartMenuExperienceHost.exe") == 0
                    || _wcsicmp(exe, L"SearchHost.exe") == 0
                    || _wcsicmp(exe, L"SearchApp.exe") == 0
                    || _wcsicmp(exe, L"WidgetService.exe") == 0
                    || _wcsicmp(exe, L"Widgets.exe") == 0
                    || _wcsicmp(exe, L"DllHost.exe") == 0);
        }

        constexpr std::uint32_t kResidualBits =
            ModuleCompat | ModuleCore | ModuleWnd | ModuleUICompat
            | ModuleExplorer | ModuleTaskbar;

        /// Скан known hosts; при outPids пишет до kStickyMaxPids.
        [[nodiscard]] DWORD CollectResidualPids(
            bool wantWow64,
            DWORD* outPids,
            DWORD maxPids) noexcept
        {
            DWORD written = 0;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap == INVALID_HANDLE_VALUE)
                return 0;

            PROCESSENTRY32W pe{};
            pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe))
            {
                do
                {
                    if (!IsKnownResidualHostExe(pe.szExeFile))
                        continue;

#if defined(_WIN64)
                    BOOL wow = FALSE;
                    HANDLE hp = OpenProcess(
                        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                    if (hp)
                    {
                        (void)IsWow64Process(hp, &wow);
                        CloseHandle(hp);
                    }
                    if (wantWow64 != (wow != FALSE))
                        continue;
#else
                    if (wantWow64)
                        continue;
#endif

                    Result r{};
                    if (!ScanProcessModules(pe.th32ProcessID, pe.szExeFile, r))
                        continue;
                    if ((r.moduleBits & kResidualBits) == 0)
                        continue;

                    if (outPids && written < maxPids)
                        outPids[written] = pe.th32ProcessID;
                    ++written;
                    if (!outPids)
                        break;
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
            return written;
        }

        [[nodiscard]] bool WriteStickyPids(
            bool wantWow64,
            const DWORD* pids,
            DWORD count) noexcept
        {
            HANDLE mapping = CreateFileMappingW(
                INVALID_HANDLE_VALUE,
                nullptr,
                PAGE_READWRITE,
                0,
                sizeof(StickyHeader),
                StickyMappingName(wantWow64));
            if (!mapping)
                return false;

            auto* view = static_cast<StickyHeader*>(
                MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(StickyHeader)));
            if (!view)
            {
                CloseHandle(mapping);
                return false;
            }

            ZeroMemory(view, sizeof(StickyHeader));
            view->magic = kStickyMagic;
            view->version = kStickyVersion;
            const DWORD n = count > kStickyMaxPids ? kStickyMaxPids : count;
            view->count = n;
            if (pids && n > 0)
                CopyMemory(view->pids, pids, n * sizeof(DWORD));

            UnmapViewOfFile(view);
            CloseHandle(mapping);
            return true;
        }
    } // namespace

    bool HasGlassCompatInKnownHosts() noexcept
    {
        return HasGlassCompatResidualForArch(/*wantWow64=*/false)
            || HasGlassCompatResidualForArch(/*wantWow64=*/true);
    }

    bool HasGlassCompatResidualForArch(bool wantWow64) noexcept
    {
        return CollectResidualPids(wantWow64, nullptr, 0) > 0;
    }

    bool RefreshInjectResidualSticky(bool wantWow64) noexcept
    {
        DWORD pids[kStickyMaxPids]{};
        const DWORD n = CollectResidualPids(wantWow64, pids, kStickyMaxPids);
        (void)WriteStickyPids(wantWow64, pids, n);
        return n > 0;
    }

    DWORD ReadInjectResidualStickyCount(bool wantWow64) noexcept
    {
        HANDLE mapping = OpenFileMappingW(
            FILE_MAP_READ, FALSE, StickyMappingName(wantWow64));
        if (!mapping)
            return 0;

        auto* view = static_cast<const StickyHeader*>(
            MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(StickyHeader)));
        if (!view)
        {
            CloseHandle(mapping);
            return 0;
        }

        DWORD count = 0;
        if (view->magic == kStickyMagic && view->version == kStickyVersion)
            count = view->count > kStickyMaxPids ? kStickyMaxPids : view->count;

        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return count;
    }
}
