#include "SharedPch.h"
#include <InjectModuleScan.h>
#include <Helpers/SystemHelper.h>

#include <TlHelp32.h>
#include <atomic>
#include <cwchar>

#pragma comment(lib, "advapi32.lib") // OpenProcessToken / AdjustTokenPrivileges (strict scans)

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

        enum class ScanAbility { Supported, OtherArchitecture, Unknown };

        // Diagnostics of the last strict scan on this thread (see GetLastScanDiag).
        thread_local ScanDiag t_diag{};
        thread_local bool t_debugPrivilegeUsed = false;

        void ResetDiag() noexcept
        {
            t_diag = {};
            t_debugPrivilegeUsed = false;
        }

        void RecordDiag(ScanDiagStage stage, DWORD pid, const wchar_t* exe, DWORD error,
            std::uint32_t flags) noexcept
        {
            // The fatal entry wins; otherwise keep the first note.
            if ((t_diag.flags & ScanDiagFatal) != 0) return;
            if (t_diag.stage != ScanDiagStage::None && (flags & ScanDiagFatal) == 0) return;
            t_diag = {};
            t_diag.stage = stage;
            t_diag.flags = flags
                | (IsExplorerExe(exe) ? ScanDiagExplorer : 0u)
                | (t_debugPrivilegeUsed ? ScanDiagDebugPrivilege : 0u);
            t_diag.pid = pid;
            t_diag.error = error;
            CopyExeName(t_diag.exeName, exe);
        }

        [[nodiscard]] bool CallerIsWow64() noexcept
        {
#if defined(_WIN64)
            return false;
#else
            BOOL selfWow = FALSE;
            return ::IsWow64Process(::GetCurrentProcess(), &selfWow) && selfWow;
#endif
        }

        // Elevated installers hold SeDebugPrivilege disabled. With it enabled,
        // OpenProcess ignores the DACL of processes that belong to other
        // accounts (second signed-in user, OOBE leftovers, separated admin
        // profile). Strict scans only; GlassSettings keeps the plain behaviour.
        [[nodiscard]] bool EnableDebugPrivilegeOnce() noexcept
        {
            static std::atomic<int> s_state{ 0 }; // 0 untried, 1 enabled, 2 unavailable
            int state = s_state.load(std::memory_order_acquire);
            if (state != 0) return state == 1;

            bool enabled = false;
            HANDLE token = nullptr;
            if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
            {
                TOKEN_PRIVILEGES tp{};
                tp.PrivilegeCount = 1;
                tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                if (::LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid))
                {
                    enabled = ::AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr) != FALSE
                        && ::GetLastError() == ERROR_SUCCESS; // ERROR_NOT_ALL_ASSIGNED = not held
                }
                ::CloseHandle(token);
            }
            s_state.store(enabled ? 1 : 2, std::memory_order_release);
            return enabled;
        }

        [[nodiscard]] HANDLE OpenForQuery(DWORD pid, bool strict, DWORD& error) noexcept
        {
            error = ERROR_SUCCESS;
            HANDLE hProc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hProc) return hProc;
            error = ::GetLastError();
            if (!strict || error != ERROR_ACCESS_DENIED || !EnableDebugPrivilegeOnce())
                return nullptr;
            t_debugPrivilegeUsed = true;
            hProc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!hProc) error = ::GetLastError();
            return hProc;
        }

        // WOW64 cannot inspect native x64 modules; the installer's native
        // helper handles those PIDs. Access errors are different from an
        // intentionally skipped architecture and must not look like success.
        // On Unknown, failStage/failError tell which call failed and why.
        [[nodiscard]] ScanAbility CanScanProcessModules(DWORD pid, bool localArchitectureOnly,
            ScanDiagStage& failStage, DWORD& failError) noexcept
        {
            failStage = ScanDiagStage::None;
            failError = ERROR_SUCCESS;
#if defined(_WIN64)
            // The ordinary GlassSettings scan keeps its historical ability to
            // inspect WOW64 processes. The installer has a separate x86 Native
            // monitor, so the strict x64 helper reports only native processes.
            if (!localArchitectureOnly) return ScanAbility::Supported;
            HANDLE hProc = OpenForQuery(pid, localArchitectureOnly, failError);
            if (!hProc)
            {
                failStage = ScanDiagStage::OpenProcess;
                return ScanAbility::Unknown;
            }
            BOOL targetWow = FALSE;
            const BOOL ok = ::IsWow64Process(hProc, &targetWow);
            failError = ok ? ERROR_SUCCESS : ::GetLastError();
            ::CloseHandle(hProc);
            if (!ok)
            {
                failStage = ScanDiagStage::IsWow64;
                return ScanAbility::Unknown;
            }
            return targetWow ? ScanAbility::OtherArchitecture : ScanAbility::Supported;
#else
            BOOL selfWow = FALSE;
            if (!::IsWow64Process(::GetCurrentProcess(), &selfWow))
            {
                failStage = ScanDiagStage::IsWow64;
                failError = ::GetLastError();
                return ScanAbility::Unknown;
            }
            if (!selfWow)
                return ScanAbility::Supported;
            HANDLE hProc = OpenForQuery(pid, localArchitectureOnly, failError);
            if (!hProc)
            {
                failStage = ScanDiagStage::OpenProcess;
                return ScanAbility::Unknown;
            }
            BOOL targetWow = FALSE;
            const BOOL ok = ::IsWow64Process(hProc, &targetWow);
            failError = ok ? ERROR_SUCCESS : ::GetLastError();
            ::CloseHandle(hProc);
            if (!ok)
            {
                failStage = ScanDiagStage::IsWow64;
                return ScanAbility::Unknown;
            }
            return targetWow ? ScanAbility::Supported : ScanAbility::OtherArchitecture;
#endif
        }

        [[nodiscard]] bool ProcessStillAliveOrUnknown(DWORD pid) noexcept
        {
            HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
            if (!process) return GetLastError() != ERROR_INVALID_PARAMETER;
            const DWORD wait = WaitForSingleObject(process, 0);
            CloseHandle(process);
            return wait != WAIT_OBJECT_0;
        }

        struct IncompleteInfo
        {
            bool incomplete = false;
            ScanDiagStage stage = ScanDiagStage::None;
            DWORD error = ERROR_SUCCESS;

            void Set(ScanDiagStage s, DWORD e) noexcept
            {
                incomplete = true;
                stage = s;
                error = e;
            }
        };

        [[nodiscard]] bool ScanProcessModules(
            DWORD pid,
            const wchar_t* exeHint,
            Result& out,
            IncompleteInfo* incomplete = nullptr,
            bool scanAllExplorerModules = false) noexcept
        {
            out = {};
            out.pid = pid;
            CopyExeName(out.exeName, exeHint);
            if (incomplete) *incomplete = {};

            ScanDiagStage failStage = ScanDiagStage::None;
            DWORD failError = ERROR_SUCCESS;
            const ScanAbility ability = CanScanProcessModules(pid, scanAllExplorerModules, failStage, failError);
            if (ability != ScanAbility::Supported)
            {
                if (incomplete && ability == ScanAbility::Unknown) incomplete->Set(failStage, failError);
                return false;
            }
            const bool allowExplorer = IsExplorerExe(out.exeName) && (scanAllExplorerModules || IsWin11());

            HANDLE snap = INVALID_HANDLE_VALUE;
            for (int attempt = 0; attempt < 5; ++attempt)
            {
                snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
                if (snap != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
                Sleep(20); // documented transient when the target's module list changes
            }
            if (snap == INVALID_HANDLE_VALUE)
            {
                if (incomplete) incomplete->Set(ScanDiagStage::ModuleSnapshot, GetLastError());
                return false;
            }

            MODULEENTRY32W me{};
            me.dwSize = sizeof(me);
            std::uint32_t bits = 0;
            if (Module32FirstW(snap, &me))
            {
                for (;;)
                {
                    bits |= MatchModuleBits(me.szModule, allowExplorer);
                    const std::uint32_t needed = allowExplorer
                        ? (ModuleCompat | ModuleCore | ModuleWnd | ModuleExplorer | ModuleTaskbar
                            | ModuleUICompat)
                        : (ModuleCompat | ModuleCore | ModuleWnd | ModuleUICompat);
                    if ((bits & needed) == needed) break;
                    if (!Module32NextW(snap, &me))
                    {
                        const DWORD error = GetLastError();
                        if (incomplete && error != ERROR_NO_MORE_FILES)
                            incomplete->Set(ScanDiagStage::ModuleEnum, error);
                        break;
                    }
                }
            }
            else if (incomplete) incomplete->Set(ScanDiagStage::ModuleEnum, GetLastError());
            CloseHandle(snap);

            if (bits == 0) return false;
            out.moduleBits = bits;
            return true;
        }
    } // namespace

    namespace
    {
        bool ScanImpl(
            const DWORD* pids, DWORD pidsCount, Result* outResults,
            DWORD maxResults, DWORD* outCount, bool strict, std::uint32_t options) noexcept
        {
            if (!outResults || maxResults == 0 || !outCount) return false;
            *outCount = 0;
            if (strict) ResetDiag();
            const bool callerWow64 = strict && CallerIsWow64();
            const std::uint32_t baseFlags = callerWow64 ? ScanDiagCallerWow64 : 0u;
            // explorer.exe on 64-bit Windows is native: a WOW64 scanner that
            // cannot even open it leaves it to the native strict scan (option).
            const bool deferForeignExplorer =
                strict && callerWow64 && (options & ScanOptionDeferForeignExplorer) != 0;

            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap == INVALID_HANDLE_VALUE)
            {
                if (strict)
                    RecordDiag(ScanDiagStage::ProcessSnapshot, 0, nullptr, GetLastError(),
                        baseFlags | ScanDiagFatal);
                return false;
            }

            PROCESSENTRY32W pe{};
            pe.dwSize = sizeof(pe);
            DWORD written = 0;
            const DWORD selfPid = GetCurrentProcessId();
            if (!Process32FirstW(snap, &pe))
            {
                const DWORD error = GetLastError();
                CloseHandle(snap);
                if (!strict || error == ERROR_NO_MORE_FILES) return true;
                RecordDiag(ScanDiagStage::ProcessSnapshot, 0, nullptr, error, baseFlags | ScanDiagFatal);
                return false;
            }
            bool ok = true;
            for (;;)
            {
                if (pe.th32ProcessID != 0 && pe.th32ProcessID != selfPid &&
                    _wcsicmp(pe.szExeFile, L"GlassSettings.exe") != 0)
                {
                    bool selected = !pids || pidsCount == 0;
                    if (!selected)
                        for (DWORD i = 0; i < pidsCount; ++i)
                            if (pids[i] == pe.th32ProcessID) { selected = true; break; }
                    if (selected)
                    {
                        Result row{};
                        IncompleteInfo incomplete;
                        const bool found = ScanProcessModules(pe.th32ProcessID, pe.szExeFile, row,
                            strict ? &incomplete : nullptr, strict);
                        // Other protected OS processes commonly deny inspection.
                        // Explorer and explicitly tracked PIDs must not be silently
                        // skipped while their Glass modules may still be loaded.
                        if (strict && incomplete.incomplete &&
                            (IsExplorerExe(pe.szExeFile) || (pids && pidsCount)) &&
                            ProcessStillAliveOrUnknown(pe.th32ProcessID))
                        {
                            const bool deferred = deferForeignExplorer && IsExplorerExe(pe.szExeFile) &&
                                (incomplete.stage == ScanDiagStage::OpenProcess ||
                                 incomplete.stage == ScanDiagStage::IsWow64);
                            if (!deferred)
                            {
                                RecordDiag(incomplete.stage, pe.th32ProcessID, pe.szExeFile,
                                    incomplete.error, baseFlags | ScanDiagFatal);
                                ok = false;
                                break;
                            }
                            RecordDiag(incomplete.stage, pe.th32ProcessID, pe.szExeFile,
                                incomplete.error, baseFlags | ScanDiagDeferred);
                        }
                        if (found)
                        {
                            if (written == maxResults)
                            {
                                if (strict)
                                {
                                    RecordDiag(ScanDiagStage::Truncated, pe.th32ProcessID, pe.szExeFile,
                                        ERROR_SUCCESS, baseFlags | ScanDiagFatal);
                                    ok = false;
                                }
                                break;
                            }
                            outResults[written++] = row;
                        }
                    }
                }
                if (!Process32NextW(snap, &pe))
                {
                    const DWORD error = GetLastError();
                    if (strict && error != ERROR_NO_MORE_FILES)
                    {
                        RecordDiag(ScanDiagStage::ProcessNext, 0, nullptr, error, baseFlags | ScanDiagFatal);
                        ok = false;
                    }
                    break;
                }
            }
            CloseHandle(snap);
            *outCount = written;
            return ok;
        }

        [[nodiscard]] const wchar_t* StageName(ScanDiagStage stage) noexcept
        {
            switch (stage)
            {
            case ScanDiagStage::None: return L"None";
            case ScanDiagStage::ProcessSnapshot: return L"ProcessSnapshot";
            case ScanDiagStage::ProcessNext: return L"ProcessNext";
            case ScanDiagStage::OpenProcess: return L"OpenProcess";
            case ScanDiagStage::IsWow64: return L"IsWow64";
            case ScanDiagStage::ModuleSnapshot: return L"ModuleSnapshot";
            case ScanDiagStage::ModuleEnum: return L"ModuleEnum";
            case ScanDiagStage::Truncated: return L"Truncated";
            }
            return L"?";
        }
    }

    bool Scan(const DWORD* pids, DWORD pidsCount, Result* outResults,
        DWORD maxResults, DWORD* outCount) noexcept
    {
        return ScanImpl(pids, pidsCount, outResults, maxResults, outCount, false, ScanOptionNone);
    }

    bool ScanForSetup(const DWORD* pids, DWORD pidsCount, Result* outResults,
        DWORD maxResults, DWORD* outCount, std::uint32_t options) noexcept
    {
        return ScanImpl(pids, pidsCount, outResults, maxResults, outCount, true, options);
    }

    void GetLastScanDiag(ScanDiag& out) noexcept
    {
        out = t_diag;
    }

    DWORD FormatScanDiag(const ScanDiag& diag, wchar_t* buffer, DWORD capacity) noexcept
    {
        if (!buffer || capacity == 0) return 0;
        buffer[0] = L'\0';
        if (diag.stage == ScanDiagStage::None)
        {
            const int n = swprintf_s(buffer, capacity, L"stage=None");
            return n > 0 ? static_cast<DWORD>(n) : 0;
        }
        wchar_t flags[96]{};
        auto add = [&](std::uint32_t bit, const wchar_t* name) {
            if ((diag.flags & bit) == 0) return;
            if (flags[0]) wcscat_s(flags, L",");
            wcscat_s(flags, name);
        };
        add(ScanDiagFatal, L"fatal");
        add(ScanDiagDeferred, L"deferred");
        add(ScanDiagExplorer, L"explorer");
        add(ScanDiagCallerWow64, L"wow64");
        add(ScanDiagDebugPrivilege, L"debugpriv");
        wchar_t exe[kExeNameChars]{};
        wcsncpy_s(exe, diag.exeName, _TRUNCATE);
        const int n = swprintf_s(buffer, capacity, L"stage=%s pid=%lu exe=%s gle=%lu flags=%s",
            StageName(diag.stage),
            static_cast<unsigned long>(diag.pid),
            exe[0] ? exe : L"-",
            static_cast<unsigned long>(diag.error),
            flags[0] ? flags : L"-");
        return n > 0 ? static_cast<DWORD>(n) : 0;
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
