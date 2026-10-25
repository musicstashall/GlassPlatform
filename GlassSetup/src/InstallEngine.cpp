#include "InstallEngine.h"
#include "InstallLog.h"
#include "../res/resource.h"
#include <msi.h>
#include <msiquery.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <aclapi.h>
#include <sddl.h>
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

bool UninstallMsiProduct(const wchar_t* productCode) noexcept {
    if (!productCode || !productCode[0]) return true;
    const INSTALLSTATE state = MsiQueryProductStateW(productCode);
    if (state == INSTALLSTATE_ABSENT || state == INSTALLSTATE_UNKNOWN) return true;
    InstallLog::LineFormat(L"msiexec /x %s", productCode);
    const std::wstring args = std::wstring(L"/x ") + productCode + L" /qn /norestart";
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"open";
    info.lpFile = L"msiexec.exe";
    info.lpParameters = args.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        InstallLog::LineFormat(L"Uninstall ShellExecuteEx failed (GetLastError=%lu)", GetLastError());
        return false;
    }
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(info.hProcess, &exitCode);
    CloseHandle(info.hProcess);
    InstallLog::LineFormat(L"Uninstall exit code %lu", exitCode);
    return exitCode == 0 || exitCode == 3010 || exitCode == 1605;
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
    if (thread_) {
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (!tempDir_.empty()) {
        SHFILEOPSTRUCTW op{};
        std::wstring from = tempDir_;
        from.push_back(L'\0');
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        SHFileOperationW(&op);
    }
}

bool InstallEngine::Start(HINSTANCE instance, const std::wstring& x64Dir, const std::wstring& x86Dir,
    const std::wstring& openGlassDir, bool installOpenGlass, PhaseLabels labels, StatusFn onStatus) noexcept {
    if (started_) return false;
    started_ = true;
    labels_ = std::move(labels);
    status_ = std::move(onStatus);
    installOpenGlass_ = installOpenGlass;
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
            bool ok = c->engine->ExtractPayload(c->instance);
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

void InstallEngine::SetStatus(const std::wstring& line, int percent) noexcept {
    AcquireSRWLockExclusive(&statusLock_);
    if (!line.empty()) statusLine_ = line;
    if (percent >= 0) progressUnits_ = percent;
    ReleaseSRWLockExclusive(&statusLock_);
    if (status_) status_(line, percent);
}

bool InstallEngine::ExtractPayload(HINSTANCE instance) noexcept {
    wchar_t tempRoot[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tempRoot)) return false;
    wchar_t folder[MAX_PATH]{};
    swprintf_s(folder, L"%sGlassSetup-%u", tempRoot, GetTickCount());
    if (!CreateDirectoryW(folder, nullptr)) return false;
    tempDir_ = folder;

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
        if (!PathCombineW(dest, folder, blob.fileName)) return false;
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
#else
    error_ = L"Release build required (embedded MSI missing).";
    return false;
#endif
    SetStatus(labels_.extract, 400);
    return true;
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
    constexpr int kProgressEnd = 9600;
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
    SetStatus(labels_.finalize, kProgressEnd);
    InstallLog::Line(L"InstallEngine: all MSI steps succeeded");
    return true;
}
