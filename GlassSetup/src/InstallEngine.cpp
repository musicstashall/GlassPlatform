#include "InstallEngine.h"
#include "InstallLog.h"
#include "../res/resource.h"
#include <GlassPlatformTask.h>
#include <ForceUnloadAgent.h>
#include <ForceUnloadGlassModules.h>
#include <InjectModuleScan.h>
#include <InjectProcessMap.h>
#include <OpenGlassHostLaunch.h>
#include <msi.h>
#include <msiquery.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <aclapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <winsvc.h>
#include <cstring>
#include <iterator>
#include <vector>

namespace {

struct ResourceBlob {
    UINT id = 0;
    const wchar_t* fileName = nullptr;
};

bool WriteResource(HINSTANCE instance, UINT id, const std::wstring& dest) noexcept {
    HRSRC res = FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return false;
    HGLOBAL loaded = LoadResource(instance, res);
    if (!loaded) return false;
    const void* data = LockResource(loaded);
    const DWORD size = SizeofResource(instance, res);
    if (!data || size == 0) return false;
    HANDLE file = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, data, size, &written, nullptr) && written == size;
    CloseHandle(file);
    return ok != FALSE;
}

struct MsiProgress {
    InstallEngine* engine = nullptr;
    std::wstring label;
    int base = 0;
    int span = 0;
    int total = 0;
    int sofar = 0;
    bool script = false;
    bool forward = true;
};

int CALLBACK OnMsiUiRecord(LPVOID context, UINT messageType, MSIHANDLE record) {
    auto* progress = static_cast<MsiProgress*>(context);
    const UINT kind = messageType & 0xFF000000;
    if (!progress || !record) return IDOK;
    if (kind == INSTALLMESSAGE_ERROR || kind == INSTALLMESSAGE_FATALEXIT) {
        wchar_t text[1024]{};
        DWORD size = static_cast<DWORD>(std::size(text));
        if (MsiFormatRecordW(NULL, record, text, &size) == ERROR_SUCCESS && text[0])
            InstallLog::LineFormat(L"MSI: %s", text);
        return IDOK;
    }
    if (kind != INSTALLMESSAGE_PROGRESS) return IDOK;
    const int field1 = MsiRecordGetInteger(record, 1);
    if (field1 == 0) {
        progress->script = MsiRecordGetInteger(record, 4) == 1;
        if (!progress->script) return IDOK;
        progress->total = MsiRecordGetInteger(record, 2);
        if (progress->total < 1) progress->total = 1;
        progress->forward = MsiRecordGetInteger(record, 3) == 0;
        progress->sofar = progress->forward ? 0 : progress->total;
    } else if (field1 == 2 && progress->script) {
        const int delta = MsiRecordGetInteger(record, 2);
        if (delta > 0) {
            progress->sofar += progress->forward ? delta : -delta;
            if (progress->sofar < 0) progress->sofar = 0;
            if (progress->sofar > progress->total) progress->sofar = progress->total;
            const int units = progress->base + static_cast<int>(
                (static_cast<long long>(progress->sofar) * progress->span) / progress->total);
            progress->engine->SetStatus(progress->label, units);
        }
    }
    return IDOK;
}

std::wstring WithTrailingSlash(std::wstring path) {
    if (!path.empty() && path.back() != L'\\') path.push_back(L'\\');
    return path;
}

// CreateProcess + CREATE_NO_WINDOW: ShellExecuteEx даже с SW_HIDE на мгновение показывает окно rundll32/msiexec.
bool RunHiddenCommand(const std::wstring& commandLine, DWORD* exitCode, DWORD timeoutMs) noexcept {
    std::vector<wchar_t> cmdBuf(commandLine.begin(), commandLine.end());
    cmdBuf.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_FORCEOFFFEEDBACK;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &si, &pi)) {
        if (exitCode)
            *exitCode = GetLastError();
        return false;
    }
    CloseHandle(pi.hThread);
    const DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (exitCode)
        *exitCode = (wait == WAIT_OBJECT_0) ? code : wait;
    return wait == WAIT_OBJECT_0;
}

bool UninstallMsiProduct(const wchar_t* productCode) noexcept {
    if (!productCode || !productCode[0]) return true;
    const INSTALLSTATE state = MsiQueryProductStateW(productCode);
    if (state == INSTALLSTATE_ABSENT || state == INSTALLSTATE_UNKNOWN) return true;
    // msiexec.exe — GUI-процесс: CREATE_NO_WINDOW его окно не прячет, /qn прячет уже после показа.
    // Снятие идёт в этом потоке, без дочернего процесса и без окна.
    InstallLog::LineFormat(L"MsiConfigureProductEx uninstall %s", productCode);
    MsiSetInternalUI(INSTALLUILEVEL_NONE, nullptr);
    const UINT exitCode = MsiConfigureProductExW(
        productCode, 0, INSTALLSTATE_ABSENT, L"REBOOT=ReallySuppress");
    InstallLog::LineFormat(L"Uninstall exit code %u", exitCode);
    return exitCode == ERROR_SUCCESS || exitCode == ERROR_SUCCESS_REBOOT_REQUIRED ||
           exitCode == ERROR_UNKNOWN_PRODUCT;
}

bool RemoveMsiProductIfPresent(const wchar_t* productCode) noexcept {
    return UninstallMsiProduct(productCode);
}

std::wstring ProductCodeFromMsi(const std::wstring& msiPath) {
    MSIHANDLE pkg = 0;
    UINT open = MsiOpenPackageW(msiPath.c_str(), &pkg);
    if (open != ERROR_SUCCESS) {
        InstallLog::LineFormat(L"MsiOpenPackage failed %u for %s", open, msiPath.c_str());
        return L"";
    }
    wchar_t value[64]{};
    DWORD cch = static_cast<DWORD>(std::size(value));
    const UINT prop = MsiGetPropertyW(pkg, L"ProductCode", value, &cch);
    MsiCloseHandle(pkg);
    if (prop != ERROR_SUCCESS || !value[0]) {
        InstallLog::LineFormat(L"MsiGetProperty ProductCode failed %u", prop);
        return L"";
    }
    return value;
}

std::wstring KnownProductCodeFromFileName(const std::wstring& msiPath) {
    const wchar_t* name = PathFindFileNameW(msiPath.c_str());
    if (!name) return L"";
    if (_wcsicmp(name, L"Glass-x64.msi") == 0) return L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5071}";
    if (_wcsicmp(name, L"Glass-x86.msi") == 0) return L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5072}";
    if (_wcsicmp(name, L"OpenGlass-x64.msi") == 0) return L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5073}";
    return L"";
}

std::wstring ResolveProductCode(const std::wstring& msiPath) {
    std::wstring code = ProductCodeFromMsi(msiPath);
    if (code.empty()) code = KnownProductCodeFromFileName(msiPath);
    if (!code.empty()) InstallLog::LineFormat(L"MSI product code: %s", code.c_str());
    return code;
}

std::wstring BuildMsiPropertyString(const std::wstring& installDir, const std::wstring& productCode) {
    std::wstring props = L"INSTALLDIR=\"" + WithTrailingSlash(installDir) + L"\" REBOOT=ReallySuppress";
    if (!productCode.empty()) {
        const INSTALLSTATE state = MsiQueryProductStateW(productCode.c_str());
        if (state == INSTALLSTATE_DEFAULT) {
            props += L" REINSTALL=ALL REINSTALLMODE=vomus";
            InstallLog::LineFormat(L"Product %s already installed — using REINSTALL=ALL", productCode.c_str());
        }
    }
    return props;
}

bool RunHidden(const wchar_t* file, const std::wstring& args) noexcept {
    InstallLog::LineFormat(L"%s %s", file, args.c_str());
    const std::wstring command = std::wstring(L"\"") + file + L"\" " + args;
    DWORD code = 1;
    if (!RunHiddenCommand(command, &code, 20000)) {
        InstallLog::LineFormat(L"  CreateProcess failed %lu", code);
        return false;
    }
    InstallLog::LineFormat(L"  exit %lu", code);
    return true;
}

void EndScheduledTask(const wchar_t* taskName) noexcept {
    RunHidden(L"schtasks.exe", std::wstring(L"/End /TN \"") + taskName + L"\"");
}

bool ImageRunning(const wchar_t* imageName) noexcept {
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, imageName) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return found;
}

bool ServiceStopped(const wchar_t* serviceName) noexcept {
    const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return true;
    const SC_HANDLE svc = OpenServiceW(scm, serviceName, SERVICE_QUERY_STATUS);
    CloseServiceHandle(scm);
    if (!svc) return true;
    SERVICE_STATUS_PROCESS status{};
    DWORD needed = 0;
    const BOOL ok = QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
        reinterpret_cast<BYTE*>(&status), sizeof(status), &needed);
    CloseServiceHandle(svc);
    if (!ok) return false;
    return status.dwCurrentState == SERVICE_STOPPED;
}

bool MsiSuccess(UINT code) noexcept {
    return code == ERROR_SUCCESS || code == ERROR_SUCCESS_REBOOT_REQUIRED ||
           code == ERROR_SUCCESS_REBOOT_INITIATED;
}

UINT RunMsiInstall(const std::wstring& msiPath, const std::wstring& props, MsiProgress* progress) noexcept {
    MsiSetInternalUI(INSTALLUILEVEL_NONE, nullptr);
    MsiSetExternalUIRecord(OnMsiUiRecord,
        INSTALLLOGMODE_PROGRESS | INSTALLLOGMODE_ERROR | INSTALLLOGMODE_FATALEXIT | INSTALLLOGMODE_WARNING,
        progress, nullptr);
    const UINT exitCode = MsiInstallProductW(msiPath.c_str(), props.c_str());
    MsiSetExternalUIRecord(nullptr, 0, nullptr, nullptr);
    return exitCode;
}

} // namespace

InstallEngine::~InstallEngine() noexcept {
    if (holdGate_) {
        holdAction_ = HoldAction::Cancel;
        SetEvent(holdGate_);
    }
    if (thread_) {
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (holdGate_) {
        CloseHandle(holdGate_);
        holdGate_ = nullptr;
    }
    if (!tempDir_.empty()) {
        InstallLog::LineFormat(L"Removing temp workspace: %s", tempDir_.c_str());
        SHFILEOPSTRUCTW op{};
        std::wstring from = tempDir_;
        from.push_back(L'\0');
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        (void)SHFileOperationW(&op);
        tempDir_.clear();
    }
}

bool InstallEngine::Start(HINSTANCE instance, const std::wstring& x64Dir, const std::wstring& x86Dir,
    const std::wstring& openGlassDir, bool installOpenGlass, PhaseLabels labels, StatusFn onStatus) noexcept {
    if (started_) return false;
    started_ = true;
    instance_ = instance;
    labels_ = std::move(labels);
    status_ = std::move(onStatus);
    installOpenGlass_ = installOpenGlass;
    x64Dir_ = x64Dir;
    x86Dir_ = x86Dir;
    openGlassDir_ = openGlassDir;
    holdGate_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!holdGate_) {
        error_ = L"CreateEvent";
        finished_ = true;
        return false;
    }
    InstallLog::Initialize();
    InstallLog::Line(L"InstallEngine::Start");
    InstallLog::LineFormat(L"  x64Dir=%s", x64Dir.c_str());
    InstallLog::LineFormat(L"  x86Dir=%s", x86Dir.c_str());
    InstallLog::LineFormat(L"  openGlassDir=%s installOpenGlass=%d", openGlassDir.c_str(), installOpenGlass ? 1 : 0);
    progressUnits_ = 0;
    statusLine_ = labels_.extract;
    struct Context {
        InstallEngine* engine;
        HINSTANCE instance;
        std::wstring x64;
        std::wstring x86;
        std::wstring og;
        bool ogInstall;
    };
    auto* ctx = new Context{this, instance, x64Dir, x86Dir, openGlassDir, installOpenGlass};
    thread_ = CreateThread(nullptr, 0,
        [](LPVOID p) -> DWORD {
            auto* c = static_cast<Context*>(p);
            bool ok = c->engine->StopRunningHosts();
            if (ok && !c->engine->cleanInstall_) ok = c->engine->ReleaseLockedModules();
            if (c->engine->quietClose_) ok = false;
            if (ok) ok = c->engine->ExtractPayload(c->instance);
            if (ok) ok = c->engine->RunMsiSequence(c->x64, c->x86, c->og, c->ogInstall);
            else InstallLog::LineFormat(L"ExtractPayload failed: %s", c->engine->error_.c_str());
            c->engine->finished_ = true;
            c->engine->succeeded_ = ok;
            delete c;
            return 0;
        }, ctx, 0, nullptr);
    if (!thread_) {
        error_ = L"CreateThread";
        finished_ = true;
        return false;
    }
    return true;
}

bool InstallEngine::Poll() noexcept {
    if (!started_) return false;
    if (finished_) return true;
    if (thread_) {
        const DWORD wait = WaitForSingleObject(thread_, 0);
        if (wait == WAIT_OBJECT_0) {
            finished_ = true;
            CloseHandle(thread_);
            thread_ = nullptr;
            if (succeeded_) SetStatus(L"", 10000);
            return true;
        }
    }
    return false;
}

std::wstring InstallEngine::StatusLine() const noexcept {
    AcquireSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    const std::wstring copy = statusLine_;
    ReleaseSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    return copy;
}

bool InstallEngine::IsMarquee() const noexcept {
    AcquireSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    const bool copy = marquee_;
    ReleaseSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    return copy;
}

void InstallEngine::SetStatus(const std::wstring& line, int percent) noexcept {
    AcquireSRWLockExclusive(&statusLock_);
    if (!line.empty()) statusLine_ = line;
    if (percent >= 0) {
        progressUnits_ = percent;
        marquee_ = false;
    } else {
        marquee_ = true;
    }
    ReleaseSRWLockExclusive(&statusLock_);
    if (status_) status_(line, percent);
}

InstallEngine::HoldKind InstallEngine::CurrentHold() const noexcept {
    AcquireSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    const HoldKind copy = holdKind_;
    ReleaseSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    return copy;
}

std::wstring InstallEngine::HoldReport() const noexcept {
    AcquireSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    const std::wstring copy = holdReport_;
    ReleaseSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    return copy;
}

void InstallEngine::SignalHold(HoldAction action) noexcept {
    AcquireSRWLockExclusive(&statusLock_);
    holdAction_ = action;
    holdKind_ = HoldKind::None;
    ReleaseSRWLockExclusive(&statusLock_);
    if (holdGate_) SetEvent(holdGate_);
}

namespace GP = GlassShared::GlassPlatformTask;

bool InstallEngine::StopGlassPlatform(GP::PlatformArch arch, const std::wstring& statusLabel,
    const wchar_t* logTag, const std::function<bool(const wchar_t*)>& fail) noexcept {
    // Тот же порядок, что у пункта меню рабочего стола.
    // schtasks /End при живом хосте: планировщик после короткого WM_CLOSE делает
    // TerminateProcess, выгрузка в explorer обрывается и проводник падает на следующем кадре.
    // /End — только StopOrphan, когда мьютекса уже нет.
    SetStatus(statusLabel, -1);
    if (GP::IsGlassPlatformRunning(arch)) {
        const bool signaled = GP::SignalPlatformStop(arch);
        InstallLog::LineFormat(L"%s: SignalPlatformStop %s", logTag, signaled ? L"ok" : L"failed");
    } else {
        InstallLog::LineFormat(L"%s: host already stopped", logTag);
    }

    const DWORD deadline = GetTickCount() + 45000;
    int emptyResidualStreak = 0;
    while (GetTickCount() < deadline) {
        if (!GP::IsGlassPlatformRunning(arch)) {
            (void)GP::StopOrphanAutorunTaskInstance(arch);
            if (!GP::RefreshInjectResidualSticky(arch)) {
                if (++emptyResidualStreak >= 6)
                    break;
            } else {
                emptyResidualStreak = 0;
            }
        }
        Sleep(250);
    }
    if (GP::IsGlassPlatformRunning(arch)) {
        InstallLog::LineFormat(L"%s: host still running", logTag);
        return fail(L"GlassPlatform host");
    }
    InstallLog::LineFormat(L"%s: stopped", logTag);
    return true;
}

bool gScanNativeX64 = false;

bool RegInstallPath(bool x64, std::wstring& out) noexcept {
    HKEY key = nullptr;
    const REGSAM sam = KEY_READ | (x64 ? KEY_WOW64_64KEY : KEY_WOW64_32KEY);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\GlassPlatform", 0, sam, &key) != ERROR_SUCCESS)
        return false;
    wchar_t buf[MAX_PATH * 2]{};
    DWORD cb = sizeof(buf);
    DWORD type = 0;
    const LSTATUS st = RegQueryValueExW(key, L"InstallPath", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &cb);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS || type != REG_SZ || !buf[0])
        return false;
    out = buf;
    while (!out.empty() && (out.back() == L'\\' || out.back() == L'/'))
        out.pop_back();
    return true;
}

bool CompatDllExists(const std::wstring& root) noexcept {
    if (root.empty()) return false;
    const std::wstring dll = root + L"\\GlassCompat.dll";
    return PathFileExistsW(dll.c_str()) != FALSE;
}

std::wstring PlatformRoot(GP::PlatformArch arch) noexcept {
    const bool x64 = arch == GP::PlatformArch::X64;
    std::wstring root;
    if (RegInstallPath(x64, root) && CompatDllExists(root))
        return root;
    root = GP::TryGetAutorunGlassCompatInstallRoot(arch);
    if (CompatDllExists(root))
        return root;
    wchar_t buf[MAX_PATH]{};
    const wchar_t* env = x64 ? L"ProgramW6432" : L"ProgramFiles(x86)";
    if (GetEnvironmentVariableW(env, buf, MAX_PATH) == 0)
        return {};
    root = std::wstring(buf) + L"\\GlassPlatform";
    return CompatDllExists(root) ? root : std::wstring{};
}

bool PeMatches(const std::wstring& path, WORD machine, const char* exportName) noexcept {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 512 || size.QuadPart > 64ll * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }
    const auto fileSize = static_cast<size_t>(size.QuadPart);
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(file);
    if (!mapping) return false;
    const auto* base = static_cast<const BYTE*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    CloseHandle(mapping);
    if (!base) return false;
    bool ok = false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0
        && static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) < fileSize) {
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == machine) {
            if (!exportName || !exportName[0]) {
                ok = true;
            } else {
            DWORD exportRva = 0;
            const IMAGE_SECTION_HEADER* section = nullptr;
            const WORD sectionCount = nt->FileHeader.NumberOfSections;
            if (sectionCount > 96) {
                UnmapViewOfFile(base);
                return false;
            }
            if (machine == IMAGE_FILE_MACHINE_AMD64) {
                const auto* nt64 = reinterpret_cast<const IMAGE_NT_HEADERS64*>(nt);
                exportRva = nt64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
                section = IMAGE_FIRST_SECTION(nt64);
            } else {
                const auto* nt32 = reinterpret_cast<const IMAGE_NT_HEADERS32*>(nt);
                exportRva = nt32->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
                section = IMAGE_FIRST_SECTION(nt32);
            }
            const auto* sectionBytes = reinterpret_cast<const BYTE*>(section);
            if (!section || sectionBytes < base
                || sectionBytes + static_cast<size_t>(sectionCount) * sizeof(IMAGE_SECTION_HEADER) > base + fileSize) {
                UnmapViewOfFile(base);
                return false;
            }
            auto rvaToPtr = [&](DWORD rva) -> const BYTE* {
                if (!section || rva == 0) return nullptr;
                for (WORD i = 0; i < sectionCount; ++i) {
                    const DWORD va = section[i].VirtualAddress;
                    const DWORD raw = section[i].SizeOfRawData;
                    if (rva < va || rva >= va + raw) continue;
                    const size_t off = static_cast<size_t>(section[i].PointerToRawData) + (rva - va);
                    if (off >= fileSize) return nullptr;
                    return base + off;
                }
                return nullptr;
            };
            auto nameIs = [&](const char* item) -> bool {
                const size_t off = static_cast<size_t>(item - reinterpret_cast<const char*>(base));
                for (size_t n = 0; exportName[n]; ++n) {
                    if (off + n >= fileSize || item[n] != exportName[n]) return false;
                }
                const size_t len = std::strlen(exportName);
                return off + len < fileSize && item[len] == '\0';
            };
            const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(rvaToPtr(exportRva));
            const auto* names = exp ? reinterpret_cast<const DWORD*>(rvaToPtr(exp->AddressOfNames)) : nullptr;
            const DWORD nameCount = exp && exp->NumberOfNames < 8192 ? exp->NumberOfNames : 0;
            const size_t namesOff = names ? static_cast<size_t>(reinterpret_cast<const BYTE*>(names) - base) : 0;
            if (names && namesOff + nameCount * sizeof(DWORD) <= fileSize) {
                for (DWORD i = 0; i < nameCount; ++i) {
                    const char* item = reinterpret_cast<const char*>(rvaToPtr(names[i]));
                    if (item && nameIs(item)) {
                        ok = true;
                        break;
                    }
                }
            }
            }
        }
    }
    UnmapViewOfFile(base);
    return ok;
}

std::wstring gBundledNativeX64;

std::wstring NativeDllForArch(GP::PlatformArch arch) noexcept {
    const bool x64 = arch == GP::PlatformArch::X64;
    // Установленная DLL может экспортировать UnlockerW, но читать командную строку
    // как wchar. Скан установщика идёт только встроенной DLL той же сборки.
    if (x64 && !gBundledNativeX64.empty())
        return gBundledNativeX64;
    const WORD machine = x64 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386;
    auto accept = [&](const std::wstring& root) -> std::wstring {
        if (!PeMatches(root + L"\\GlassCompat.dll", machine, nullptr))
            return {};
        const std::wstring dll = root + L"\\GlassSettings\\GlassSettings.Native.dll";
        if (!PeMatches(dll, machine, "GlassSettings_UnlockerW"))
            return {};
        return dll;
    };
    std::wstring root;
    if (RegInstallPath(x64, root)) {
        const std::wstring dll = accept(root);
        if (!dll.empty()) return dll;
    }
    root = GP::TryGetAutorunGlassCompatInstallRoot(arch);
    {
        const std::wstring dll = accept(root);
        if (!dll.empty()) return dll;
    }
    wchar_t buf[MAX_PATH]{};
    const wchar_t* env = x64 ? L"ProgramW6432" : L"ProgramFiles(x86)";
    if (GetEnvironmentVariableW(env, buf, MAX_PATH) == 0)
        return x64 ? gBundledNativeX64 : std::wstring{};
    root = std::wstring(buf) + L"\\GlassPlatform";
    const std::wstring dll = accept(root);
    if (!dll.empty()) return dll;
    return x64 ? gBundledNativeX64 : std::wstring{};
}

bool OpenGlassPresent(const std::wstring& openGlassDir) noexcept {
    if (ImageRunning(L"OpenGlassHost.exe"))
        return true;
    if (!openGlassDir.empty() && PathFileExistsW((openGlassDir + L"\\OpenGlassHost.exe").c_str()))
        return true;
    const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    const SC_HANDLE svc = OpenServiceW(scm, L"OpenGlassHost", SERVICE_QUERY_STATUS);
    CloseServiceHandle(scm);
    if (!svc) return false;
    CloseServiceHandle(svc);
    return true;
}

enum class ArchPresence { Running, TaskStopped, Files, Absent };

ArchPresence ClassifyArch(GP::PlatformArch arch) noexcept {
    if (GP::IsGlassPlatformRunning(arch))
        return ArchPresence::Running;
    if (GP::GlassAutorunTaskExists(arch))
        return ArchPresence::TaskStopped;
    if (!PlatformRoot(arch).empty())
        return ArchPresence::Files;
    return ArchPresence::Absent;
}

bool InstallEngine::StopRunningHosts() noexcept {
    const bool ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    auto fail = [this, ru](const wchar_t* what) {
        error_ = ru ? L"Не удалось дождаться остановки: " : L"Timed out waiting to stop: ";
        error_ += what;
        InstallLog::Line(error_.c_str());
        SetStatus(L"", 0);
        return false;
    };

    const ArchPresence x64 = ClassifyArch(GP::PlatformArch::X64);
    const ArchPresence x86 = ClassifyArch(GP::PlatformArch::X86);
    const bool openGlass = OpenGlassPresent(openGlassDir_);
    cleanInstall_ = x64 == ArchPresence::Absent && x86 == ArchPresence::Absent && !openGlass;
    scanNativeX64_ = x64 != ArchPresence::Absent;
    gScanNativeX64 = scanNativeX64_;
    if (cleanInstall_) {
        InstallLog::Line(L"Clean install — skip stop and module scan");
        return true;
    }

    if (x64 == ArchPresence::Running) {
        if (!StopGlassPlatform(GP::PlatformArch::X64, labels_.stop64, L"x64", fail))
            return false;
    } else {
        InstallLog::Line(x64 == ArchPresence::Absent
            ? L"x64: no task and no files"
            : L"x64: host not running — skip stop");
    }
    if (x86 == ArchPresence::Running) {
        if (!StopGlassPlatform(GP::PlatformArch::X86, labels_.stop86, L"x86", fail))
            return false;
    } else {
        InstallLog::Line(x86 == ArchPresence::Absent
            ? L"x86: no task and no files"
            : L"x86: host not running — skip stop");
    }

    if (openGlass) {
        SetStatus(labels_.stopOg, -1);
        InstallLog::Line(L"OpenGlass: TryShutdown");
        (void)Shared::OpenGlassHostLaunch::TryShutdown();
        const DWORD ogStart = GetTickCount();
        while (ImageRunning(L"OpenGlassHost.exe") || !ServiceStopped(L"OpenGlassHost")) {
            if (GetTickCount() - ogStart >= 45000)
                return fail(L"OpenGlassHost");
            Sleep(250);
        }
    } else {
        InstallLog::Line(L"OpenGlass: not installed — skip shutdown");
    }

    InstallLog::Line(L"Hosts stopped");
    return true;
}

namespace {

struct ModuleHolder {
    DWORD pid = 0;
    std::wstring exe;
    std::wstring modules;
};

bool IsProtectedProcess(const wchar_t* exe) noexcept {
    if (!exe || !exe[0]) return true;
    const wchar_t* blocked[] = {
        L"System", L"Registry", L"smss.exe", L"csrss.exe", L"wininit.exe",
        L"services.exe", L"lsass.exe", L"winlogon.exe", L"dwm.exe",
        L"fontdrvhost.exe", L"Memory Compression", L"Secure System",
    };
    for (const wchar_t* name : blocked)
        if (_wcsicmp(exe, name) == 0) return true;
    return false;
}

std::wstring ModuleNamesFromBits(std::uint32_t bits) {
    std::wstring text;
    auto add = [&](std::uint32_t flag, const wchar_t* name) {
        if ((bits & flag) == 0) return;
        if (!text.empty()) text += L", ";
        text += name;
    };
    add(Shared::InjectModuleScan::ModuleCompat, L"GlassCompat.dll");
    add(Shared::InjectModuleScan::ModuleCore, L"GlassCore.dll");
    add(Shared::InjectModuleScan::ModuleWnd, L"WndCompat.dll");
    add(Shared::InjectModuleScan::ModuleUICompat, L"UICompat.dll");
    add(Shared::InjectModuleScan::ModuleExplorer, L"ExplorerCompat.dll");
    add(Shared::InjectModuleScan::ModuleTaskbar, L"TaskbarCompat.dll");
    return text;
}

// Из Win32-процесса 64-битный rundll32 виден только как Sysnative.
// System32 здесь — 32-битный rundll32, он не должен грузить x64 DLL.
std::wstring NativeRundll64() {
    wchar_t windows[MAX_PATH]{};
    if (!GetWindowsDirectoryW(windows, MAX_PATH)) return {};
    std::wstring path = std::wstring(windows) + L"\\Sysnative\\rundll32.exe";
    return PathFileExistsW(path.c_str()) ? path : std::wstring{};
}

void MergeModuleList(std::wstring& into, const std::wstring& add) {
    if (add.empty()) return;
    size_t start = 0;
    while (start < add.size()) {
        size_t comma = add.find(L", ", start);
        const std::wstring token = add.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
        if (!token.empty() && into.find(token) == std::wstring::npos) {
            if (!into.empty()) into += L", ";
            into += token;
        }
        if (comma == std::wstring::npos) break;
        start = comma + 2;
    }
}

void MergeHolder(std::vector<ModuleHolder>& holders, DWORD pid, const std::wstring& exe,
    const std::wstring& modules) {
    if (pid == 0 || modules.empty()) return;
    if (_wcsicmp(exe.c_str(), L"GlassSetup.exe") == 0) return;
    for (auto& holder : holders) {
        if (holder.pid != pid) continue;
        if (!exe.empty()) holder.exe = exe;
        MergeModuleList(holder.modules, modules);
        return;
    }
    holders.push_back({pid, exe, modules});
}

void AppendScanResults(std::vector<ModuleHolder>& holders,
    const Shared::InjectModuleScan::Result* results, DWORD count) {
    for (DWORD i = 0; i < count; ++i) {
        const std::wstring modules = ModuleNamesFromBits(results[i].moduleBits);
        MergeHolder(holders, results[i].pid, results[i].exeName, modules);
    }
}

void ScanInjectMapInto(std::vector<ModuleHolder>& holders, const wchar_t* mapName) {
    Shared::InjectProcessMap::Slot slots[Shared::InjectProcessMap::kSlotCount]{};
    DWORD slotCount = 0;
    if (!Shared::InjectProcessMap::TryReadSlotsFromMapping(
            mapName, slots, Shared::InjectProcessMap::kSlotCount, &slotCount))
        return;
    std::vector<DWORD> pids;
    pids.reserve(slotCount);
    for (DWORD i = 0; i < slotCount; ++i) {
        if (slots[i].pid == 0 || slots[i].moduleBits == 0) continue;
        pids.push_back(slots[i].pid);
    }
    if (pids.empty()) return;
    Shared::InjectModuleScan::Result results[Shared::InjectModuleScan::kMaxResults]{};
    DWORD count = 0;
    if (Shared::InjectModuleScan::Scan(
            pids.data(), static_cast<DWORD>(pids.size()), results,
            Shared::InjectModuleScan::kMaxResults, &count))
        AppendScanResults(holders, results, count);
}

bool IsExplorerExeName(const wchar_t* exe) noexcept {
    return exe && _wcsicmp(exe, L"explorer.exe") == 0;
}

// Любой ещё отображённый модуль, включая GlassCompat, блокирует замену файлов.
// Пустой список CollectHolders уже отбрасывает: здесь держатель = занятый модуль.

void ParseNativeScanTsv(const std::string& bytes, std::vector<ModuleHolder>& holders) {
    int wideCount = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring text(wideCount > 0 ? static_cast<size_t>(wideCount) : 0, L'\0');
    if (wideCount > 0)
        MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), wideCount);
    if (!text.empty() && text[0] == 0xFEFF) text.erase(0, 1);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        pos = end + 1;
        const size_t tab1 = line.find(L'\t');
        const size_t tab2 = tab1 == std::wstring::npos ? std::wstring::npos : line.find(L'\t', tab1 + 1);
        if (tab1 == std::wstring::npos || tab2 == std::wstring::npos) continue;
        const DWORD pid = static_cast<DWORD>(wcstoul(line.substr(0, tab1).c_str(), nullptr, 10));
        MergeHolder(holders, pid, line.substr(tab1 + 1, tab2 - tab1 - 1), line.substr(tab2 + 1));
    }
}

// Win32 не может Module32 по native x64. Скан делает установленный x64 GlassSettings.Native.dll.
std::vector<ModuleHolder> ScanNativeViaHelper(bool& failed) {
    failed = false;
    std::vector<ModuleHolder> holders;
    if (!gScanNativeX64)
        return holders;
    BOOL selfWow = FALSE;
    if (!IsWow64Process(GetCurrentProcess(), &selfWow) || !selfWow)
        return holders;

    const std::wstring dll = NativeDllForArch(GP::PlatformArch::X64);
    if (dll.empty()) {
        failed = true;
        InstallLog::Line(L"Unlocker: x64 GlassSettings.Native.dll not found under InstallPath\\GlassSettings");
        return holders;
    }
    const std::wstring rundll = NativeRundll64();
    wchar_t tempDir[MAX_PATH]{};
    wchar_t outPath[MAX_PATH]{};
    if (rundll.empty() || !GetTempPathW(MAX_PATH, tempDir) || !GetTempFileNameW(tempDir, L"gsh", 0, outPath)) {
        failed = true;
        return holders;
    }

    InstallLog::LineFormat(L"Unlocker: native x64 module scan via %s", dll.c_str());
    std::wstring cmd = L"\"" + rundll + L"\" \"" + dll + L"\",GlassSettings_UnlockerW /scanGlassModules \"" + outPath + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    DWORD exitCode = 1;
    if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 120000);
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        exitCode = GetLastError();
        InstallLog::LineFormat(L"Unlocker: x64 Native CreateProcess failed %lu", exitCode);
    }
    if (exitCode != 0) {
        failed = true;
        InstallLog::LineFormat(L"Unlocker: x64 scan failed, exit %lu", exitCode);
    }
    HANDLE out = CreateFileW(outPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out != INVALID_HANDLE_VALUE) {
        DWORD size = GetFileSize(out, nullptr);
        std::string bytes(size && size != INVALID_FILE_SIZE ? size : 0, '\0');
        DWORD got = 0;
        if (!bytes.empty())
            ReadFile(out, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr);
        CloseHandle(out);
        bytes.resize(got);
        ParseNativeScanTsv(bytes, holders);
        InstallLog::LineFormat(L"Unlocker: x64 Native reported %u holders",
            static_cast<unsigned>(holders.size()));
    } else if (!failed) {
        failed = true;
        InstallLog::Line(L"Unlocker: x64 scan output missing");
    }
    DeleteFileW(outPath);
    return holders;
}

bool gNativeScanFailed = false;

std::vector<ModuleHolder> CollectHolders() {
    gNativeScanFailed = false;
    (void)GP::RefreshInjectResidualSticky(GP::PlatformArch::X64);
    (void)GP::RefreshInjectResidualSticky(GP::PlatformArch::X86);
    std::vector<ModuleHolder> holders;
    Shared::InjectModuleScan::Result results[Shared::InjectModuleScan::kMaxResults]{};
    DWORD count = 0;
    if (Shared::InjectModuleScan::Scan(
            nullptr, 0, results, Shared::InjectModuleScan::kMaxResults, &count))
        AppendScanResults(holders, results, count);
    ScanInjectMapInto(holders, Shared::InjectProcessMap::kMappingNameX86);
    ScanInjectMapInto(holders, Shared::InjectProcessMap::kMappingNameX64);
    ScanInjectMapInto(holders, Shared::InjectProcessMap::kMappingNameLegacy);
    bool nativeFailed = false;
    for (const auto& native : ScanNativeViaHelper(nativeFailed))
        MergeHolder(holders, native.pid, native.exe, native.modules);
    gNativeScanFailed = nativeFailed;
    return holders;
}

bool TargetIsWow64(DWORD pid) noexcept {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return false;
    BOOL wow = FALSE;
    const BOOL ok = IsWow64Process(process, &wow);
    CloseHandle(process);
    return ok && wow == TRUE;
}

std::wstring FormatHolders(const std::vector<ModuleHolder>& holders) {
    std::wstring text;
    const size_t limit = holders.size() < 24 ? holders.size() : 24;
    for (size_t i = 0; i < limit; ++i) {
        if (!text.empty()) text += L"\r\n";
        text += holders[i].exe;
        if (TargetIsWow64(holders[i].pid))
            text += L" (86)";
        text += L" (PID ";
        text += std::to_wstring(holders[i].pid);
        text += L"): ";
        text += holders[i].modules;
    }
    if (holders.size() > limit)
        text += L"\r\n…";
    return text;
}

bool TargetIsNative64(DWORD pid) noexcept {
    BOOL selfWow = FALSE;
    if (!IsWow64Process(GetCurrentProcess(), &selfWow) || !selfWow)
        return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    BOOL targetWow = FALSE;
    const BOOL ok = IsWow64Process(process, &targetWow);
    CloseHandle(process);
    return ok && !targetWow;
}

bool ForceUnloadHolder(const ModuleHolder& holder, const std::wstring& x64Dir,
    const std::wstring& x86Dir) noexcept;

void CloseProcessWindows(DWORD pid) noexcept;

bool ForceUnloadHolder(const ModuleHolder& holder, const std::wstring& x64Dir,
    const std::wstring& x86Dir) noexcept {
    (void)x64Dir;
    const DWORD pid = holder.pid;
    if (_wcsicmp(holder.exe.c_str(), L"GlassSettings.exe") == 0) {
        // MSI снимает GlassCore из-под живого GlassSettings (c0000005, GlassCore.dll_unloaded).
        // Remote FreeLibrary здесь то же самое. Только закрыть окно.
        InstallLog::LineFormat(L"Unlocker: GlassSettings pid %lu — WM_CLOSE, no Remote FreeLibrary", pid);
        CloseProcessWindows(pid);
        return false;
    }
    if (IsExplorerExeName(holder.exe.c_str())) {
        InstallLog::Line(L"Unlocker: explorer — only soft UnloadRequest, no Remote FreeLibrary");
        if (Shared::InjectProcessMap::SignalUnloadRequest(pid))
            Sleep(1200);
        return false;
    }
    if (Shared::InjectProcessMap::SignalUnloadRequest(pid))
        Sleep(500);

    if (!TargetIsNative64(pid)) {
        const auto result = Shared::ForceUnloadGlassModules::ForceUnload(pid);
        if (result == Shared::ForceUnloadGlassModules::ForceUnloadResult::Ok)
            return true;
        if (result == Shared::ForceUnloadGlassModules::ForceUnloadResult::NeedsX86Agent) {
            std::wstring root = Shared::ForceUnloadAgent::TryGetX86InstallRoot();
            if (root.empty()) root = x86Dir;
            return Shared::ForceUnloadAgent::SpawnX86ForceUnloadAgent(pid, root.c_str());
        }
        return false;
    }

    std::wstring dll = NativeDllForArch(GP::PlatformArch::X64);
    if (dll.empty()) {
        InstallLog::LineFormat(L"Unlocker: no x64 GlassSettings.Native for pid %lu", pid);
        return false;
    }
    const std::wstring rundll = NativeRundll64();
    if (rundll.empty()) {
        InstallLog::Line(L"Unlocker: Sysnative\\rundll32.exe not found");
        return false;
    }
    const std::wstring command = L"\"" + rundll + L"\" \"" + dll + L"\",GlassSettings_UnlockerW /forceUnload "
        + std::to_wstring(pid);
    InstallLog::LineFormat(L"Unlocker: x64 Native forceUnload pid %lu", pid);
    DWORD exitCode = 1;
    if (!RunHiddenCommand(command, &exitCode, 20000)) {
        InstallLog::LineFormat(L"Unlocker: forceUnload CreateProcess failed %lu", exitCode);
        return false;
    }
    InstallLog::LineFormat(L"Unlocker: forceUnload exit %lu", exitCode);
    return exitCode == 0;
}

void CloseProcessWindows(DWORD pid) noexcept {
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        DWORD windowPid = 0;
        GetWindowThreadProcessId(hwnd, &windowPid);
        if (windowPid == static_cast<DWORD>(param))
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return TRUE;
    }, static_cast<LPARAM>(pid));
}

bool ProcessAlive(DWORD pid) noexcept {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return false;
    const DWORD wait = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return wait == WAIT_TIMEOUT;
}

void UnlockProcess(DWORD pid, const wchar_t* exe) noexcept {
    if (IsExplorerExeName(exe)) {
        InstallLog::Line(L"Unlocker: skip TerminateProcess for explorer.exe");
        CloseProcessWindows(pid);
        return;
    }
    CloseProcessWindows(pid);
    const DWORD start = GetTickCount();
    while (ProcessAlive(pid) && GetTickCount() - start < 8000)
        Sleep(200);
    if (!ProcessAlive(pid)) return;
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!process) {
        InstallLog::LineFormat(L"Unlocker: cannot terminate pid %lu (%lu)", pid, GetLastError());
        return;
    }
    InstallLog::LineFormat(L"Unlocker: TerminateProcess pid %lu", pid);
    TerminateProcess(process, 1);
    WaitForSingleObject(process, 5000);
    CloseHandle(process);
}

} // namespace

void InstallEngine::DisableAutostart() noexcept {
    InstallLog::Line(L"Unlocker: disable autorun tasks and OpenGlass service");
    (void)GP::SetGlassAutorunTaskSchedulerEnabled(GP::PlatformArch::X64, false);
    (void)GP::SetGlassAutorunTaskSchedulerEnabled(GP::PlatformArch::X86, false);
    RunHidden(L"schtasks.exe", L"/Change /TN \"OpenGlass Host\" /DISABLE");
    RunHidden(L"schtasks.exe", L"/Change /TN \"OpenGlass Legacy Host\" /DISABLE");
    const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;
    const SC_HANDLE svc = OpenServiceW(scm, L"OpenGlassHost", SERVICE_CHANGE_CONFIG | SERVICE_STOP);
    CloseServiceHandle(scm);
    if (!svc) return;
    (void)ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_DISABLED, SERVICE_NO_CHANGE,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    CloseServiceHandle(svc);
}

bool InstallEngine::ReleaseLockedModules() noexcept {
    auto publish = [&](HoldKind kind, const std::vector<ModuleHolder>& holders) {
        AcquireSRWLockExclusive(&statusLock_);
        holdKind_ = kind;
        holdReport_ = FormatHolders(holders);
        holdAction_ = HoldAction::None;
        ReleaseSRWLockExclusive(&statusLock_);
        if (holdGate_) ResetEvent(holdGate_);
    };

    SetStatus(labels_.unload.empty() ? L"…" : labels_.unload, -1);
#ifdef GLASS_EMBED_MSI
    if (gScanNativeX64 && EnsureTempWorkspace()) {
        wchar_t dest[MAX_PATH]{};
        if (PathCombineW(dest, tempDir_.c_str(), L"GlassSettings.Native.x64.dll")
            && WriteResource(instance_, IDR_NATIVE_X64, dest)
            && PeMatches(dest, IMAGE_FILE_MACHINE_AMD64, "GlassSettings_UnlockerW")) {
            gBundledNativeX64 = dest;
            InstallLog::Line(L"Unlocker: x64 scan uses Native.dll from setup payload");
        } else {
            InstallLog::Line(L"Unlocker: bundled x64 Native.dll missing, falling back to installed DLL");
        }
    }
#endif
    const DWORD start = GetTickCount();
    std::vector<DWORD> settingsClosed;
    auto nudgeSettings = [&](const std::vector<ModuleHolder>& list) {
        for (const auto& holder : list) {
            if (_wcsicmp(holder.exe.c_str(), L"GlassSettings.exe") != 0) continue;
            bool seen = false;
            for (DWORD pid : settingsClosed) {
                if (pid == holder.pid) { seen = true; break; }
            }
            if (seen) continue;
            settingsClosed.push_back(holder.pid);
            InstallLog::LineFormat(L"Unlocker: GlassSettings pid %lu — WM_CLOSE before MSI", holder.pid);
            CloseProcessWindows(holder.pid);
        }
    };
    std::vector<ModuleHolder> holders = CollectHolders();
    nudgeSettings(holders);
    if (!holders.empty()) {
        InstallLog::LineFormat(L"Unlocker: %u processes still hold Glass modules after stop",
            static_cast<unsigned>(holders.size()));
        for (const auto& holder : holders) {
            InstallLog::LineFormat(L"  holder %s pid %lu%s: %s",
                holder.exe.c_str(),
                holder.pid,
                TargetIsWow64(holder.pid) ? L" wow64" : L"",
                holder.modules.c_str());
        }
        // Хост уже остановлен. UnloadRequest в этом цикле не шлём.
        // GlassSettings закрываем один раз через WM_CLOSE: иначе MSI выгрузит GlassCore из-под процесса.
        while (!holders.empty() && GetTickCount() - start < 30000) {
            Sleep(2000);
            holders = CollectHolders();
            nudgeSettings(holders);
        }
    }
    if (gNativeScanFailed) {
        error_ = L"Не удалось просканировать 64-битные процессы. Установка остановлена.";
        InstallLog::Line(error_.c_str());
        return false;
    }
    if (holders.empty()) {
        InstallLog::Line(L"Unlocker: all Glass modules unloaded");
        return true;
    }

    for (;;) {
        InstallLog::Line(L"Unlocker: asking user to force-unload");
        publish(HoldKind::Modules, holders);
        WaitForSingleObject(holdGate_, INFINITE);
        if (holdAction_ == HoldAction::Cancel) {
            quietClose_ = true;
            return false;
        }
        SetStatus(labels_.unload.empty() ? L"…" : labels_.unload, -1);
        for (const auto& holder : holders)
            (void)ForceUnloadHolder(holder, x64Dir_, x86Dir_);
        const DWORD retryStart = GetTickCount();
        do {
            Sleep(1000);
            holders = CollectHolders();
        } while (!holders.empty() && GetTickCount() - retryStart < 8000);
        if (holders.empty()) {
            InstallLog::Line(L"Unlocker: force unload cleared modules");
            return true;
        }

        InstallLog::Line(L"Unlocker: modules remain after force unload");
        publish(HoldKind::Processes, holders);
        WaitForSingleObject(holdGate_, INFINITE);
        if (holdAction_ == HoldAction::Cancel) {
            DisableAutostart();
            quietClose_ = true;
            return false;
        }
        SetStatus(labels_.unload.empty() ? L"…" : labels_.unload, -1);
        for (const auto& holder : holders) {
            if (IsProtectedProcess(holder.exe.c_str())) {
                InstallLog::LineFormat(L"Unlocker: skip protected %s", holder.exe.c_str());
                continue;
            }
            UnlockProcess(holder.pid, holder.exe.c_str());
        }
        holders = CollectHolders();
        if (holders.empty()) {
            InstallLog::Line(L"Unlocker: processes released modules");
            return true;
        }
    }
}

bool InstallEngine::EnsureTempWorkspace() noexcept {
    if (!tempDir_.empty()) return true;
    wchar_t tempRoot[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tempRoot)) return false;
    wchar_t folder[MAX_PATH]{};
    swprintf_s(folder, L"%sGlassSetup-%u", tempRoot, GetTickCount());
    if (!CreateDirectoryW(folder, nullptr)) return false;
    tempDir_ = folder;
    InstallLog::LineFormat(L"Temp workspace: %s", tempDir_.c_str());
    return true;
}

bool InstallEngine::ExtractPayload(HINSTANCE instance) noexcept {
    if (!EnsureTempWorkspace()) {
        error_ = L"Не удалось создать каталог во временной папке.";
        return false;
    }

    SetStatus(labels_.extract, 0);
#ifdef GLASS_EMBED_MSI
    const ResourceBlob blobs[] = {
        {IDR_MSI_X64, L"Glass-x64.msi"},
        {IDR_MSI_X86, L"Glass-x86.msi"},
        {IDR_MSI_OPENGLASS, L"OpenGlass-x64.msi"},
    };
    int step = 0;
    for (const auto& blob : blobs) {
        wchar_t dest[MAX_PATH]{};
        if (!PathCombineW(dest, tempDir_.c_str(), blob.fileName)) return false;
        if (!WriteResource(instance, blob.id, dest)) {
            error_ = L"Missing embedded MSI: ";
            error_ += blob.fileName;
            InstallLog::LineFormat(L"Missing embedded MSI resource: %s", blob.fileName);
            return false;
        }
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (GetFileAttributesExW(dest, GetFileExInfoStandard, &info)) {
            const ULONGLONG size = (static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
            InstallLog::LineFormat(L"Extracted %s (%llu bytes) -> %s", blob.fileName, size, dest);
        }
        ++step;
        SetStatus(labels_.extract, (step * 400) / static_cast<int>(std::size(blobs)));
    }
    SetStatus(labels_.extract, 400);
    return true;
#else
    (void)instance;
    error_ = L"Release build required (embedded MSI missing).";
    return false;
#endif
}

bool InstallEngine::RunOneMsi(const std::wstring& msiPath, const std::wstring& installDir,
    const std::wstring& label, int progressBase, int progressSpan) noexcept {
    if (!PathFileExistsW(msiPath.c_str())) {
        error_ = L"MSI not found: ";
        error_ += msiPath;
        return false;
    }
    SetStatus(label, progressBase);

    const std::wstring msiLog = InstallLog::NextMsiVerboseLogPath(label.c_str());
    MsiEnableLogW(INSTALLLOGMODE_VERBOSE, msiLog.c_str(), INSTALLLOGATTRIBUTES_FLUSHEACHLINE);
    const std::wstring productCode = ResolveProductCode(msiPath);
    if (!productCode.empty()) {
        const INSTALLSTATE state = MsiQueryProductStateW(productCode.c_str());
        if (state == INSTALLSTATE_DEFAULT) {
            InstallLog::Line(L"Existing product detected — uninstall before upgrade");
            UninstallMsiProduct(productCode.c_str());
        }
    }
    std::wstring props = BuildMsiPropertyString(installDir, L"");
    InstallLog::LineFormat(L"MsiInstallProduct %s", msiPath.c_str());
    InstallLog::LineFormat(L"  %s", props.c_str());
    InstallLog::LineFormat(L"MSI verbose log: %s", msiLog.c_str());

    MsiProgress progress{this, label, progressBase, progressSpan};
    UINT exitCode = RunMsiInstall(msiPath, props, &progress);
    if (exitCode == ERROR_PRODUCT_VERSION) {
        InstallLog::Line(L"MSI 1638 — forced uninstall and retry");
        if (!productCode.empty()) UninstallMsiProduct(productCode.c_str());
        exitCode = RunMsiInstall(msiPath, props, &progress);
    }
    InstallLog::LineFormat(L"MsiInstallProduct exit code %u (%s)", exitCode, label.c_str());
    if (!MsiSuccess(exitCode)) {
        error_ = L"MSI exit ";
        error_ += std::to_wstring(exitCode);
        error_ += L": ";
        error_ += label;
        InstallLog::Line(error_.c_str());
        InstallLog::LineFormat(L"See MSI log: %s", msiLog.c_str());
        return false;
    }
    SetStatus(label, progressBase + progressSpan);
    return true;
}

bool InstallEngine::SetOpenGlassHostOwner() noexcept {
    if (ogHostPath_.empty() || !PathFileExistsW(ogHostPath_.c_str())) return true;
    PSID systemSid = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-5-18", &systemSid)) return true;
    const BOOL ownerOk = SetNamedSecurityInfoW(const_cast<LPWSTR>(ogHostPath_.c_str()), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION, systemSid, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    LocalFree(systemSid);
    return ownerOk != FALSE;
}

bool InstallEngine::RunMsiSequence(const std::wstring& x64Dir, const std::wstring& x86Dir,
    const std::wstring& openGlassDir, bool installOpenGlass) noexcept {
    wchar_t x64Msi[MAX_PATH]{}, x86Msi[MAX_PATH]{}, ogMsi[MAX_PATH]{};
    if (!PathCombineW(x64Msi, tempDir_.c_str(), L"Glass-x64.msi")) return false;
    if (!PathCombineW(x86Msi, tempDir_.c_str(), L"Glass-x86.msi")) return false;
    if (!PathCombineW(ogMsi, tempDir_.c_str(), L"OpenGlass-x64.msi")) return false;

    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    const bool is64 = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
        si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64;

    const int packages = (is64 ? 1 : 0) + 1 + (installOpenGlass ? 1 : 0);
    constexpr int kProgressEnd = 8800;
    const int span = packages > 0 ? (kProgressEnd - 400) / packages : (kProgressEnd - 400);
    int base = 400;
    auto bump = [&](const std::wstring& label, const std::wstring& dir, const wchar_t* msi) {
        const bool ok = RunOneMsi(msi, dir, label, base, span);
        base += span;
        return ok;
    };

    InstallLog::LineFormat(L"Native OS 64-bit=%d", is64 ? 1 : 0);
    if (is64) {
        RemoveMsiProductIfPresent(L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5061}");
        if (!bump(labels_.platform64, x64Dir, x64Msi)) return false;
    }
    RemoveMsiProductIfPresent(L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5062}");
    if (!bump(labels_.platform32, x86Dir, x86Msi)) return false;
    if (installOpenGlass) {
        // Старый WiX OpenGlass (5063) и официальные следы до смены ProductCode.
        RemoveMsiProductIfPresent(L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5063}");
        ogHostPath_ = openGlassDir;
        if (!ogHostPath_.empty() && ogHostPath_.back() != L'\\') ogHostPath_ += L'\\';
        ogHostPath_ += L"OpenGlassHost.exe";
        if (!bump(labels_.openGlass, openGlassDir, ogMsi)) return false;
        SetOpenGlassHostOwner();
    }
    if (!RestorePlatformAutostart(installOpenGlass)) return false;
    InstallLog::Line(L"InstallEngine: all MSI steps succeeded");
    return true;
}

bool InstallEngine::RestorePlatformAutostart(bool installOpenGlass) noexcept {
    auto startTask = [this](GP::PlatformArch arch, const std::wstring& label) {
        if (!GP::GlassAutorunTaskExists(arch)) {
            InstallLog::LineFormat(L"Autostart: no task for arch %d", static_cast<int>(arch));
            return true;
        }
        SetStatus(label, -1);
        if (GP::IsGlassPlatformRunning(arch)) {
            InstallLog::LineFormat(L"Autostart: platform already running arch %d", static_cast<int>(arch));
            return true;
        }
        // Контекстное меню: только IRegisteredTask::Run. События хоста не трогать.
        InstallLog::LineFormat(L"Autostart: StartPlatform arch %d", static_cast<int>(arch));
        GP::StartPlatform(arch);
        const DWORD start = GetTickCount();
        while (!GP::IsGlassPlatformRunning(arch)) {
            if (GetTickCount() - start >= GP::kDefaultPlatformShutdownWaitMs)
                return false;
            Sleep(200);
        }
        InstallLog::LineFormat(L"Autostart: platform running arch %d", static_cast<int>(arch));
        return true;
    };

    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    const bool is64 = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
        si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64;

    SetStatus(labels_.finalize, 8800);
    if (is64 && !startTask(GP::PlatformArch::X64, labels_.start64))
        return false;
    SetStatus(labels_.start86, 9000);
    if (!startTask(GP::PlatformArch::X86, labels_.start86))
        return false;

    // OpenGlass остановили перед заменой файлов. Это не старт платформы.
    if (installOpenGlass) {
        SetStatus(labels_.startOg, -1);
        InstallLog::Line(L"Autostart: OpenGlass TryStartup");
        (void)Shared::OpenGlassHostLaunch::TryStartup();
        const DWORD start = GetTickCount();
        while (!ImageRunning(L"OpenGlassHost.exe")) {
            if (GetTickCount() - start >= GP::kDefaultPlatformShutdownWaitMs)
                break;
            Sleep(250);
        }
        InstallLog::LineFormat(L"Autostart: OpenGlass running=%d",
            ImageRunning(L"OpenGlassHost.exe") ? 1 : 0);
    }

    SetStatus(labels_.finalize, 10000);
    return true;
}
