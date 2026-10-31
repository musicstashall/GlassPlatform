#include <Windows.h>

#include <ForceUnloadAgent.h>
#include <cstdio>
#include <string>
#include <vector>
#include <ForceUnloadGlassModules.h>
#include <GlassModulePath.h>

#include <InjectModuleScan.h>
#include <InjectProcessMap.h>
#include <GlassPlatformTask.h>
#include <GlassUlTrace.h>

namespace
{
    [[nodiscard]] BOOL ReadMap(
        const wchar_t* name,
        Shared::InjectProcessMap::Slot* outSlots,
        DWORD maxSlots,
        DWORD* outCount) noexcept
    {
        return Shared::InjectProcessMap::TryReadSlotsFromMapping(
            name, outSlots, maxSlots, outCount) ? TRUE : FALSE;
    }

    // Совпадает с GlassPlatformArch / GlassPlatformTask::PlatformArch (X86=0, X64=1).
    // Не путать с InjectArch слотов карты (там X64=0, X86=1).
    [[nodiscard]] GlassShared::GlassPlatformTask::PlatformArch ToPlatformArch(int arch) noexcept
    {
        return arch == static_cast<int>(GlassShared::GlassPlatformTask::PlatformArch::X86)
            ? GlassShared::GlassPlatformTask::PlatformArch::X86
            : GlassShared::GlassPlatformTask::PlatformArch::X64;
    }

    [[nodiscard]] bool FileExists(const std::wstring& path) noexcept
    {
        const DWORD attrs = GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    [[nodiscard]] std::wstring NativeDllPath(bool wantX64) noexcept
    {
        using PA = GlassShared::GlassPlatformTask::PlatformArch;
        std::wstring root = GlassShared::GlassPlatformTask::TryGetAutorunGlassCompatInstallRoot(
            wantX64 ? PA::X64 : PA::X86);
        if (root.empty())
        {
            wchar_t buf[MAX_PATH]{};
            const wchar_t* env = wantX64 ? L"ProgramW6432" : L"ProgramFiles(x86)";
            if (GetEnvironmentVariableW(env, buf, MAX_PATH) == 0)
                return {};
            root = buf;
            root += L"\\GlassPlatform";
        }
        if (!root.empty() && root.back() != L'\\')
            root += L'\\';
        root += L"GlassSettings\\GlassSettings.Native.dll";
        return FileExists(root) ? root : std::wstring{};
    }

    [[nodiscard]] std::wstring Rundll32Path(bool wantX64Host) noexcept
    {
        wchar_t windows[MAX_PATH]{};
        if (!GetWindowsDirectoryW(windows, MAX_PATH))
            return {};
#ifdef _WIN64
        if (!wantX64Host)
        {
            wchar_t wow[MAX_PATH]{};
            if (GetSystemWow64DirectoryW(wow, MAX_PATH) == 0)
                return {};
            return std::wstring(wow) + L"\\rundll32.exe";
        }
        return std::wstring(windows) + L"\\System32\\rundll32.exe";
#else
        if (wantX64Host)
        {
            const std::wstring sysnative = std::wstring(windows) + L"\\Sysnative\\rundll32.exe";
            if (FileExists(sysnative))
                return sysnative;
        }
        return std::wstring(windows) + L"\\System32\\rundll32.exe";
#endif
    }

    // rundll32 сам показывает MessageBox, если экспорта нет. Не запускать такой DLL.
    [[nodiscard]] bool PeHasUnlocker(const std::wstring& path, bool wantX64) noexcept
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart < 512 || size.QuadPart > 64 * 1024 * 1024)
        {
            CloseHandle(file);
            return false;
        }
        HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        CloseHandle(file);
        if (!mapping)
            return false;
        const auto* base = static_cast<const BYTE*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        CloseHandle(mapping);
        if (!base)
            return false;
        bool ok = false;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto fileSize = static_cast<size_t>(size.QuadPart);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0
            && static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) < fileSize)
        {
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            const WORD expect = wantX64 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386;
            if (nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == expect)
            {
                DWORD exportRva = 0;
                const IMAGE_SECTION_HEADER* section = nullptr;
                const WORD sectionCount = nt->FileHeader.NumberOfSections;
                if (expect == IMAGE_FILE_MACHINE_AMD64)
                {
                    const auto* nt64 = reinterpret_cast<const IMAGE_NT_HEADERS64*>(nt);
                    exportRva = nt64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
                    section = IMAGE_FIRST_SECTION(nt64);
                }
                else
                {
                    const auto* nt32 = reinterpret_cast<const IMAGE_NT_HEADERS32*>(nt);
                    exportRva = nt32->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
                    section = IMAGE_FIRST_SECTION(nt32);
                }
                auto rvaToOff = [&](DWORD rva) -> const BYTE* {
                    if (!section || rva == 0 || sectionCount == 0 || sectionCount > 96)
                        return nullptr;
                    const auto* sectionBytes = reinterpret_cast<const BYTE*>(section);
                    if (sectionBytes < base
                        || sectionBytes + static_cast<size_t>(sectionCount) * sizeof(IMAGE_SECTION_HEADER) > base + fileSize)
                        return nullptr;
                    for (WORD i = 0; i < sectionCount; ++i)
                    {
                        const DWORD va = section[i].VirtualAddress;
                        const DWORD raw = section[i].SizeOfRawData;
                        if (rva < va || rva >= va + raw)
                            continue;
                        const size_t off = static_cast<size_t>(section[i].PointerToRawData) + (rva - va);
                        if (off >= fileSize)
                            return nullptr;
                        return base + off;
                    }
                    return nullptr;
                };
                const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(rvaToOff(exportRva));
                const auto* names = exp ? reinterpret_cast<const DWORD*>(rvaToOff(exp->AddressOfNames)) : nullptr;
                const DWORD nameCount = exp && exp->NumberOfNames < 8192 ? exp->NumberOfNames : 0;
                const size_t namesOff = names ? static_cast<size_t>(reinterpret_cast<const BYTE*>(names) - base) : 0;
                if (names && namesOff + static_cast<size_t>(nameCount) * sizeof(DWORD) <= fileSize)
                {
                    for (DWORD i = 0; i < nameCount; ++i)
                    {
                        const char* item = reinterpret_cast<const char*>(rvaToOff(names[i]));
                        if (!item)
                            continue;
                        const char kExport[] = "GlassSettings_UnlockerW";
                        const size_t off = static_cast<size_t>(item - reinterpret_cast<const char*>(base));
                        bool same = off + sizeof(kExport) <= fileSize;
                        for (size_t n = 0; same && kExport[n]; ++n)
                            same = item[n] == kExport[n];
                        if (same && item[sizeof(kExport) - 1] == '\0')
                        {
                            ok = true;
                            break;
                        }
                    }
                }
            }
        }
        UnmapViewOfFile(base);
        return ok;
    }

    // exit code процесса rundll32. 0 — успех. DLL грузится в rundll32, не в целевой процесс.
    [[nodiscard]] DWORD RunUnlocker(bool wantX64Dll, const std::wstring& args) noexcept
    {
        const std::wstring dll = NativeDllPath(wantX64Dll);
        const std::wstring rundll = Rundll32Path(wantX64Dll);
        if (dll.empty() || rundll.empty() || !PeHasUnlocker(dll, wantX64Dll))
            return 1;
        std::wstring cmd = L"\"" + rundll + L"\" \"" + dll + L"\",GlassSettings_UnlockerW " + args;
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(L'\0');
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, nullptr, &si, &pi))
            return GetLastError() ? GetLastError() : 1;
        WaitForSingleObject(pi.hProcess, 120000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return code;
    }

    [[nodiscard]] std::wstring ModuleNamesFromBits(std::uint32_t bits) noexcept
    {
        using namespace Shared::InjectModuleScan;
        std::wstring text;
        auto add = [&](std::uint32_t flag, const wchar_t* name) {
            if ((bits & flag) == 0) return;
            if (!text.empty()) text += L", ";
            text += name;
        };
        add(ModuleCompat, L"GlassCompat.dll");
        add(ModuleCore, L"GlassCore.dll");
        add(ModuleWnd, L"WndCompat.dll");
        add(ModuleUICompat, L"UICompat.dll");
        add(ModuleExplorer, L"ExplorerCompat.dll");
        add(ModuleTaskbar, L"TaskbarCompat.dll");
        return text;
    }

    [[nodiscard]] bool AppendUtf8Field(std::string& row, const wchar_t* wide) noexcept
    {
        if (!wide || !wide[0]) return false;
        const int need = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
        if (need <= 1) return false;
        std::string chunk(static_cast<size_t>(need), '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, chunk.data(), need, nullptr, nullptr) != need)
            return false;
        chunk.pop_back();
        row += chunk;
        return true;
    }

    [[nodiscard]] bool WriteScanRowUtf8(
        HANDLE dest,
        const Shared::InjectModuleScan::Result& row) noexcept
    {
        const std::wstring modules = ModuleNamesFromBits(row.moduleBits);
        if (modules.empty())
            return false;
        char line[640]{};
        const int n = std::snprintf(line, sizeof(line), "%lu\t",
            static_cast<unsigned long>(row.pid));
        if (n <= 0)
            return false;
        std::string bytes(line);
        if (!AppendUtf8Field(bytes, row.exeName)) return false;
        bytes += '\t';
        if (!AppendUtf8Field(bytes, modules.c_str())) return false;
        bytes += '\n';
        DWORD written = 0;
        return WriteFile(dest, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE &&
            written == bytes.size();
    }

    [[nodiscard]] DWORD CountScanHolders() noexcept
    {
        Shared::InjectModuleScan::Result results[Shared::InjectModuleScan::kMaxResults]{};
        DWORD count = 0;
        if (!Shared::InjectModuleScan::Scan(
                nullptr, 0, results, Shared::InjectModuleScan::kMaxResults, &count))
            return 0;
        DWORD holders = 0;
        for (DWORD i = 0; i < count; ++i)
        {
            if (ModuleNamesFromBits(results[i].moduleBits).empty())
                continue;
            ++holders;
        }
        return holders;
    }

    [[nodiscard]] bool ValidateTrackedProcesses(const wchar_t* mapName) noexcept
    {
        Shared::InjectProcessMap::Slot slots[Shared::InjectProcessMap::kSlotCount]{};
        DWORD slotCount = 0;
        if (!Shared::InjectProcessMap::TryReadSlotsFromMapping(
                mapName, slots, Shared::InjectProcessMap::kSlotCount, &slotCount))
            return true; // absent host/map is normal
        std::vector<DWORD> pids;
        for (DWORD i = 0; i < slotCount; ++i)
            if (slots[i].pid && slots[i].moduleBits) pids.push_back(slots[i].pid);
        if (pids.empty()) return true;
        Shared::InjectModuleScan::Result tracked[Shared::InjectModuleScan::kMaxResults]{};
        DWORD count = 0;
        return Shared::InjectModuleScan::ScanForSetup(pids.data(), static_cast<DWORD>(pids.size()),
            tracked, Shared::InjectModuleScan::kMaxResults, &count);
    }

    [[nodiscard]] bool WriteScanResultsUtf8(
        HANDLE dest,
        bool* anyOut,
        bool forSetup) noexcept
    {
        if (anyOut) *anyOut = false;
        Shared::InjectModuleScan::Result results[Shared::InjectModuleScan::kMaxResults]{};
        DWORD count = 0;
        const bool scanned = forSetup
            ? Shared::InjectModuleScan::ScanForSetup(nullptr, 0, results, Shared::InjectModuleScan::kMaxResults, &count)
            : Shared::InjectModuleScan::Scan(nullptr, 0, results, Shared::InjectModuleScan::kMaxResults, &count);
        if (!scanned) return false;
        if (forSetup &&
            (!ValidateTrackedProcesses(Shared::InjectProcessMap::kMappingNameX64) ||
             !ValidateTrackedProcesses(Shared::InjectProcessMap::kMappingNameX86) ||
             !ValidateTrackedProcesses(Shared::InjectProcessMap::kMappingNameLegacy))) return false;
        bool any = false;
        for (DWORD i = 0; i < count; ++i)
        {
            if (ModuleNamesFromBits(results[i].moduleBits).empty()) continue;
            if (!WriteScanRowUtf8(dest, results[i])) return false;
            any = true;
        }
        if (anyOut) *anyOut = any;
        return true;
    }

    [[nodiscard]] bool WriteScanTsv(const wchar_t* outPath, bool forSetup) noexcept
    {
        HANDLE file = CreateFileW(outPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        bool any = false;
        const bool ok = WriteScanResultsUtf8(file, &any, forSetup);
        (void)any;
        CloseHandle(file);
        return ok;
    }

    [[nodiscard]] bool WritePipeMarker(HANDLE pipe, const char* marker) noexcept
    {
        if (!marker)
            return false;
        const size_t len = std::strlen(marker);
        DWORD written = 0;
        if (!WriteFile(pipe, marker, static_cast<DWORD>(len), &written, nullptr) || written != len)
            return false;
        FlushFileBuffers(pipe);
        return true;
    }

    [[nodiscard]] bool WatchGlassModulesPipe(const wchar_t* pipeName, DWORD timeoutMs, bool forSetup) noexcept
    {
        if (!pipeName || !pipeName[0])
            ExitProcess(1);
        HANDLE pipe = INVALID_HANDLE_VALUE;
        const DWORD connectStart = GetTickCount();
        while (GetTickCount() - connectStart < 5000)
        {
            pipe = CreateFileW(pipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE)
                break;
            Sleep(50);
        }
        if (pipe == INVALID_HANDLE_VALUE)
            ExitProcess(1);
        const DWORD watchStart = GetTickCount();
        for (;;)
        {
            if (GetTickCount() - watchStart >= timeoutMs)
            {
                CloseHandle(pipe);
                ExitProcess(2);
            }
            bool any = false;
            if (!WriteScanResultsUtf8(pipe, &any, forSetup))
            {
                CloseHandle(pipe);
                ExitProcess(2);
            }
            if (!WritePipeMarker(pipe, any ? "#end\n" : "#empty\n"))
            {
                CloseHandle(pipe);
                ExitProcess(0);
            }
            // Setup keeps watching both architectures while even one holder
            // remains; otherwise an early empty frame could become stale.
            if (!any && !forSetup)
            {
                CloseHandle(pipe);
                ExitProcess(0);
            }
            const DWORD wait = 500;
            if (timeoutMs - (GetTickCount() - watchStart) <= wait)
            {
                CloseHandle(pipe);
                ExitProcess(2);
            }
            Sleep(wait);
        }
    }

    [[nodiscard]] std::uint32_t BitsFromNames(const std::wstring& modules) noexcept
    {
        using namespace Shared::InjectModuleScan;
        std::uint32_t bits = 0;
        auto hit = [&](const wchar_t* name, std::uint32_t flag) {
            if (modules.find(name) != std::wstring::npos)
                bits |= flag;
        };
        hit(L"GlassCompat.dll", ModuleCompat);
        hit(L"GlassCore.dll", ModuleCore);
        hit(L"WndCompat.dll", ModuleWnd);
        hit(L"UICompat.dll", ModuleUICompat);
        hit(L"ExplorerCompat.dll", ModuleExplorer);
        hit(L"TaskbarCompat.dll", ModuleTaskbar);
        return bits;
    }

    void MergeScanResult(
        Shared::InjectModuleScan::Result* out,
        DWORD maxResults,
        DWORD* count,
        const Shared::InjectModuleScan::Result& add) noexcept
    {
        if (!out || !count || add.pid == 0 || add.moduleBits == 0)
            return;
        for (DWORD i = 0; i < *count; ++i)
        {
            if (out[i].pid != add.pid)
                continue;
            out[i].moduleBits |= add.moduleBits;
            if (!out[i].exeName[0] && add.exeName[0])
                wcsncpy_s(out[i].exeName, add.exeName, _TRUNCATE);
            return;
        }
        if (*count >= maxResults)
            return;
        out[(*count)++] = add;
    }

    // WOW64 GS не видит native x64 через Module32 — тот же скан делает x64 Native.dll.
    [[nodiscard]] bool MergeOppositeX64Scan(
        Shared::InjectModuleScan::Result* out,
        DWORD maxResults,
        DWORD* count,
        const DWORD* onlyPids,
        DWORD onlyCount) noexcept
    {
        wchar_t tempDir[MAX_PATH]{};
        wchar_t outPath[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, tempDir) || !GetTempFileNameW(tempDir, L"gsu", 0, outPath))
            return false;
        const DWORD code = RunUnlocker(true, L"/scanGlassModules \"" + std::wstring(outPath) + L"\"");
        HANDLE file = CreateFileW(outPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        bool any = false;
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD size = GetFileSize(file, nullptr);
            std::string bytes(size && size != INVALID_FILE_SIZE ? size : 0, '\0');
            DWORD got = 0;
            if (!bytes.empty())
                ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr);
            CloseHandle(file);
            bytes.resize(got);
            int wideCount = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            std::wstring text(wideCount > 0 ? static_cast<size_t>(wideCount) : 0, L'\0');
            if (wideCount > 0)
                MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), wideCount);
            size_t pos = 0;
            while (pos < text.size())
            {
                size_t end = text.find(L'\n', pos);
                if (end == std::wstring::npos) end = text.size();
                std::wstring line = text.substr(pos, end - pos);
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                pos = end + 1;
                const size_t tab1 = line.find(L'\t');
                const size_t tab2 = tab1 == std::wstring::npos ? std::wstring::npos : line.find(L'\t', tab1 + 1);
                if (tab1 == std::wstring::npos || tab2 == std::wstring::npos)
                    continue;
                Shared::InjectModuleScan::Result row{};
                row.pid = static_cast<DWORD>(wcstoul(line.substr(0, tab1).c_str(), nullptr, 10));
                wcsncpy_s(row.exeName, line.substr(tab1 + 1, tab2 - tab1 - 1).c_str(), _TRUNCATE);
                row.moduleBits = BitsFromNames(line.substr(tab2 + 1));
                if (onlyPids && onlyCount > 0)
                {
                    bool wanted = false;
                    for (DWORD n = 0; n < onlyCount; ++n)
                    {
                        if (onlyPids[n] == row.pid)
                        {
                            wanted = true;
                            break;
                        }
                    }
                    if (!wanted)
                        continue;
                }
                MergeScanResult(out, maxResults, count, row);
                any = true;
            }
        }
        DeleteFileW(outPath);
        return code == 0 || any;
    }

    [[nodiscard]] BOOL ForceUnloadWithArchRouting(DWORD pid) noexcept
    {
        const auto result = Shared::ForceUnloadGlassModules::ForceUnload(pid);
        switch (result)
        {
        case Shared::ForceUnloadGlassModules::ForceUnloadResult::Ok:
            return TRUE;
        case Shared::ForceUnloadGlassModules::ForceUnloadResult::NeedsX86Agent:
        {
            if (RunUnlocker(false, L"/forceUnload " + std::to_wstring(pid)) == 0)
                return TRUE;
            const std::wstring root = Shared::ForceUnloadAgent::TryGetX86InstallRoot();
            if (root.empty())
                return FALSE;
            return Shared::ForceUnloadAgent::SpawnX86ForceUnloadAgent(pid, root.c_str()) ? TRUE : FALSE;
        }
        case Shared::ForceUnloadGlassModules::ForceUnloadResult::BlockedHost:
        case Shared::ForceUnloadGlassModules::ForceUnloadResult::Failed:
        default:
            return FALSE;
        }
    }
}

namespace
{
    [[nodiscard]] BOOL UnloadPid(DWORD pid) noexcept
    {
#if !defined(_WIN64)
        BOOL selfWow = FALSE;
        if (pid != 0 && IsWow64Process(GetCurrentProcess(), &selfWow) && selfWow)
        {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (process)
            {
                BOOL targetWow = TRUE;
                const BOOL ok = IsWow64Process(process, &targetWow);
                CloseHandle(process);
                if (ok && !targetWow)
                    return RunUnlocker(true, L"/forceUnload " + std::to_wstring(pid)) == 0 ? TRUE : FALSE;
            }
        }
#endif
        return ForceUnloadWithArchRouting(pid);
    }
}

extern "C"
{
    __declspec(dllexport) BOOL WINAPI GlassSettings_IsInjectMapAvailable() noexcept
    {
        return (Shared::InjectProcessMap::IsMappingAvailable(Shared::InjectProcessMap::kMappingNameX64)
                || Shared::InjectProcessMap::IsMappingAvailable(Shared::InjectProcessMap::kMappingNameX86)
                || Shared::InjectProcessMap::IsMappingAvailable(Shared::InjectProcessMap::kMappingNameLegacy))
            ? TRUE
            : FALSE;
    }

    /// arch: 0 = x64 (+ legacy fallback), 1 = x86.
    __declspec(dllexport) BOOL WINAPI GlassSettings_TryReadInjectMapSlotsForArch(
        DWORD arch,
        void* outSlots,
        DWORD maxSlots,
        DWORD* outCount) noexcept
    {
        if (!outSlots || maxSlots == 0 || !outCount)
            return FALSE;

        auto* slots = static_cast<Shared::InjectProcessMap::Slot*>(outSlots);
        BOOL ok;
        if (arch == 1)
            ok = ReadMap(Shared::InjectProcessMap::kMappingNameX86, slots, maxSlots, outCount);
        else if (ReadMap(Shared::InjectProcessMap::kMappingNameX64, slots, maxSlots, outCount))
            ok = TRUE;
        else
            // Миграция: старое имя без суффикса.
            ok = ReadMap(Shared::InjectProcessMap::kMappingNameLegacy, slots, maxSlots, outCount);

        return ok;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_TryReadInjectMapSlots(
        void* outSlots,
        DWORD maxSlots,
        DWORD* outCount) noexcept
    {
        // Совместимость: только x64 (+ legacy).
        return GlassSettings_TryReadInjectMapSlotsForArch(0, outSlots, maxSlots, outCount);
    }

    /// arch: 0 = x64 (+ legacy), 1 = x86. Слот мёртвого PID после exit без FreeSelf.
    __declspec(dllexport) BOOL WINAPI GlassSettings_ClearInjectMapPidForArch(
        DWORD arch,
        DWORD pid) noexcept
    {
        if (pid == 0)
            return FALSE;
        BOOL ok;
        if (arch == 1)
        {
            ok = Shared::InjectProcessMap::TryClearPidFromMapping(
                Shared::InjectProcessMap::kMappingNameX86, pid)
                ? TRUE
                : FALSE;
        }
        else
        {
            ok = Shared::InjectProcessMap::TryClearPidFromMapping(
                Shared::InjectProcessMap::kMappingNameX64, pid)
                ? TRUE
                : FALSE;
            if (!ok)
            {
                ok = Shared::InjectProcessMap::TryClearPidFromMapping(
                    Shared::InjectProcessMap::kMappingNameLegacy, pid)
                    ? TRUE
                    : FALSE;
            }
        }
        return ok;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_RequestProcessUnload(DWORD pid) noexcept
    {
        // Soft: UnloadRequest event (живой GUI-поток GlassCompat).
        // Force: remote FreeLibrary (мёртвый residual / OS-pin без слушателя) — Unlocker-стиль.
        const bool soft = Shared::InjectProcessMap::SignalUnloadRequest(pid);
        // #region agent log
        // GLASS-ULTRACE
        {
            char before[80]{};
            GlassUlTrace::FormatModuleMask(GlassUlTrace::RemoteModuleMask(pid), before, sizeof(before));
            GlassUlTrace::Logf(
                "H90",
                "InjectProcessMapNative.cpp:GlassSettings_RequestProcessUnload",
                "gs_request",
                "\"target_pid\":%lu,\"soft\":%d,\"modules_before\":\"%s\"",
                static_cast<unsigned long>(pid),
                soft ? 1 : 0,
                before);
        }
        // #endregion
        if (soft)
        {
            // #region agent log
            // GLASS-ULTRACE
            (void)GlassUlTrace::ScheduleVerify();
            // #endregion
            return TRUE;
        }
        const BOOL force = UnloadPid(pid);
        // #region agent log
        // GLASS-ULTRACE
        {
            char left[80]{};
            const unsigned mask = GlassUlTrace::RemoteModuleMask(pid);
            GlassUlTrace::FormatModuleMask(mask, left, sizeof(left));
            GlassUlTrace::Logf(
                "H90",
                "InjectProcessMapNative.cpp:GlassSettings_RequestProcessUnload",
                "gs_request",
                "\"target_pid\":%lu,\"soft\":0,\"force\":%d,\"left\":\"%s\",\"mask\":%u",
                static_cast<unsigned long>(pid),
                force ? 1 : 0,
                left,
                mask);
            (void)GlassUlTrace::ScheduleVerify();
        }
        // #endregion
        return force;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_ForceUnloadProcess(DWORD pid) noexcept
    {
#ifdef _M_IX86
#pragma comment(linker, "/EXPORT:GlassSettings_ForceUnloadProcess=_GlassSettings_ForceUnloadProcess@4")
#endif
        return UnloadPid(pid);
    }

    /// GS x64: x86 InstallPath или /TR x86-задачи + GlassCompat.dll для SysWOW64 rundll32 /forceUnload.
    __declspec(dllexport) BOOL WINAPI GlassSettings_IsX86ForceUnloadAgentAvailable() noexcept
    {
#if defined(_WIN64)
        if (!NativeDllPath(false).empty())
            return TRUE;
        return Shared::ForceUnloadAgent::TryGetX86InstallRoot().empty() ? FALSE : TRUE;
#else
        return TRUE;
#endif
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_TryGetAutorunGlassCompatInstallRootForArch(
        int arch,
        wchar_t* buf,
        DWORD cch) noexcept
    {
        if (!buf || cch == 0)
            return FALSE;
        buf[0] = L'\0';
        const std::wstring root = GlassShared::GlassPlatformTask::TryGetAutorunGlassCompatInstallRoot(
            ToPlatformArch(arch));
        if (root.empty())
            return FALSE;
        if (wcscpy_s(buf, cch, root.c_str()) != 0)
            return FALSE;
        return TRUE;
    }

    /// arch: GlassPlatformArch / PlatformArch — 0 = x86, 1 = x64.
    __declspec(dllexport) BOOL WINAPI GlassSettings_HasShellInjectResidualsForArch(int arch) noexcept
    {
        return GlassShared::GlassPlatformTask::HasShellInjectResiduals(ToPlatformArch(arch))
            ? TRUE
            : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_HasShellInjectResiduals() noexcept
    {
        return GlassSettings_HasShellInjectResidualsForArch(
                   static_cast<int>(GlassShared::GlassPlatformTask::PlatformArch::X86))
            || GlassSettings_HasShellInjectResidualsForArch(
                   static_cast<int>(GlassShared::GlassPlatformTask::PlatformArch::X64));
    }

    __declspec(dllexport) int WINAPI GlassSettings_QueryInjectLifecycleUiForArch(int arch) noexcept
    {
        return static_cast<int>(
            GlassShared::GlassPlatformTask::QueryInjectLifecycleUi(ToPlatformArch(arch)));
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_RefreshInjectResidualStickyForArch(
        int arch) noexcept
    {
        return GlassShared::GlassPlatformTask::RefreshInjectResidualSticky(ToPlatformArch(arch))
            ? TRUE
            : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_ScanGlassModules(
        const DWORD* pids,
        DWORD pidsCount,
        void* outResults,
        DWORD maxResults,
        DWORD* outCount) noexcept
    {
#ifdef _M_IX86
#pragma comment(linker, "/EXPORT:GlassSettings_ScanGlassModules=_GlassSettings_ScanGlassModules@20")
#pragma comment(linker, "/EXPORT:GlassSettings_InstallerScanProtocolV1=_GlassSettings_ScanGlassModules@20")
#elif defined(_M_X64)
#pragma comment(linker, "/EXPORT:GlassSettings_InstallerScanProtocolV1=GlassSettings_ScanGlassModules")
#endif
        // The versioned alias above points to this same existing scan export;
        // it lets Setup reject an older embedded DLL before altering products.
        // Preserve the original public export for GlassSettings. Setup also uses
        // this export, but monitors x64 in its own resident rundll32 process.
        // Its explicit option requests a strict scan of only this architecture.
        const bool installerLocal = (maxResults & Shared::InjectModuleScan::kNativeLocalInstallerScan) != 0;
        maxResults &= ~Shared::InjectModuleScan::kNativeLocalInstallerScan;
        if (installerLocal && !pids && pidsCount == 0 && !outResults && maxResults == 0 && outCount) {
            *outCount = Shared::InjectModuleScan::kNativeInstallerScanProtocol;
            return TRUE;
        }
        if (!outResults || maxResults == 0 || !outCount)
            return FALSE;
        if (installerLocal)
            return Shared::InjectModuleScan::ScanForSetup(pids, pidsCount,
                static_cast<Shared::InjectModuleScan::Result*>(outResults), maxResults, outCount) ? TRUE : FALSE;
        const BOOL ok = Shared::InjectModuleScan::Scan(
            pids,
            pidsCount,
            static_cast<Shared::InjectModuleScan::Result*>(outResults),
            maxResults,
            outCount) ? TRUE : FALSE;
#if !defined(_WIN64)
        BOOL selfWow = FALSE;
        if (IsWow64Process(GetCurrentProcess(), &selfWow) && selfWow)
        {
            auto* rows = static_cast<Shared::InjectModuleScan::Result*>(outResults);
            if (MergeOppositeX64Scan(rows, maxResults, outCount, pids, pidsCount))
                return TRUE;
        }
#endif
        return ok;
    }
}

namespace
{
    [[nodiscard]] std::wstring UnlockerToken(wchar_t*& cursor) noexcept
    {
        while (cursor && *cursor == L' ')
            ++cursor;
        if (!cursor || !*cursor)
            return {};
        if (*cursor == L'"')
        {
            ++cursor;
            const wchar_t* start = cursor;
            while (*cursor && *cursor != L'"')
                ++cursor;
            std::wstring token(start, static_cast<size_t>(cursor - start));
            if (*cursor == L'"')
                ++cursor;
            return token;
        }
        const wchar_t* start = cursor;
        while (*cursor && *cursor != L' ')
            ++cursor;
        return std::wstring(start, static_cast<size_t>(cursor - start));
    }
}

// rundll32 передаёт хвост командной строки как ANSI (char*), даже если имя
// экспорта заканчивается на W. На Windows 11 26100 указатель "/scan..." читается
// как wchar 0x732F, глагол не совпадает, выход 1. Настоящий UTF-16 начинается с L'/'.
std::wstring UnlockerCommandText(LPWSTR cmdLine) noexcept
{
    if (!cmdLine || !cmdLine[0])
        return {};
    const auto first = static_cast<unsigned>(cmdLine[0]);
    const bool wideAscii = first < 128
        && (cmdLine[0] == L'/' || cmdLine[0] == L'"' || cmdLine[0] == L' ');
    if (wideAscii)
        return cmdLine;
    const char* ansi = reinterpret_cast<const char*>(cmdLine);
    const int n = MultiByteToWideChar(CP_ACP, 0, ansi, -1, nullptr, 0);
    if (n <= 1)
        return {};
    std::wstring text(static_cast<size_t>(n), L'\0');
    if (MultiByteToWideChar(CP_ACP, 0, ansi, -1, text.data(), n) <= 0)
        return {};
    if (!text.empty() && text.back() == L'\0')
        text.pop_back();
    return text;
}

extern "C" __declspec(dllexport) void CALLBACK GlassSettings_UnlockerW(HWND, HINSTANCE, LPWSTR cmdLine, int)
{
#ifdef _M_IX86
#pragma comment(linker, "/EXPORT:GlassSettings_UnlockerW=_GlassSettings_UnlockerW@16")
#endif
    std::wstring text = UnlockerCommandText(cmdLine);
    wchar_t* cursor = text.empty() ? const_cast<wchar_t*>(L"") : text.data();
    const std::wstring verb = UnlockerToken(cursor);
    const bool setupScan = _wcsicmp(verb.c_str(), L"/scanGlassModulesSetup") == 0;
    if (setupScan || _wcsicmp(verb.c_str(), L"/scanGlassModules") == 0)
    {
        const std::wstring path = UnlockerToken(cursor);
        ExitProcess(path.empty() || !WriteScanTsv(path.c_str(), setupScan) ? 2 : 0);
    }
    if (_wcsicmp(verb.c_str(), L"/forceUnload") == 0)
    {
        const std::wstring pidText = UnlockerToken(cursor);
        const DWORD pid = static_cast<DWORD>(wcstoul(pidText.c_str(), nullptr, 10));
        ExitProcess(pid != 0 && UnloadPid(pid) ? 0 : 3);
    }
    const bool setupWatch = _wcsicmp(verb.c_str(), L"/watchGlassModulesSetup") == 0;
    if (setupWatch || _wcsicmp(verb.c_str(), L"/watchGlassModules") == 0)
    {
        const std::wstring pipeName = UnlockerToken(cursor);
        const std::wstring timeoutText = UnlockerToken(cursor);
        const DWORD timeoutMs = timeoutText.empty()
            ? 30000u
            : static_cast<DWORD>(wcstoul(timeoutText.c_str(), nullptr, 10));
        (void)WatchGlassModulesPipe(pipeName.c_str(), timeoutMs == 0 ? 30000u : timeoutMs, setupWatch);
    }
    ExitProcess(1);
}
