#include "GlassInstallMsi.h"
#include "GlassInstallLog.h"
#include <msi.h>
#include <msiquery.h>
#include <shlwapi.h>
#include <cstring>

namespace {

// Путь в кавычках для MsiInstallProduct. Один '\' перед '"' съедается как экранирование,
// MSI получает путь без завершающего слэша и дописывает имя из Directory (GlassPlatform\GlassPlatform).
std::wstring MsiQuotedInstallDir(std::wstring path) {
    if (!path.empty() && path.back() != L'\\') path.push_back(L'\\');
    path.push_back(L'\\');
    return L"\"" + path + L"\"";
}

int CALLBACK OnMsiUiRecord(LPVOID context, UINT messageType, MSIHANDLE record) {
    auto* progress = static_cast<GlassInstallMsi::ProgressSink*>(context);
    // Request cancellation only once. Subsequent UI records must not interrupt MSI's own rollback.
    if (progress && !progress->cancellationSent && progress->cancelRequested &&
        progress->cancelRequested->load(std::memory_order_acquire)) {
        progress->cancellationSent=true;
        return IDCANCEL;
    }
    const UINT kind = messageType & 0xFF000000;
    if (!progress || !record) return IDOK;
    if (kind == INSTALLMESSAGE_ERROR || kind == INSTALLMESSAGE_FATALEXIT) {
        wchar_t text[1024]{};
        DWORD size = static_cast<DWORD>(std::size(text));
        if (MsiFormatRecordW(NULL, record, text, &size) == ERROR_SUCCESS && text[0]) {
            GlassInstallLog::LineFormat(L"MSI: %s", text);
            if (progress->onError) progress->onError(text);
        }
        return IDOK;
    }
    if (kind != INSTALLMESSAGE_PROGRESS || !progress->onProgress) return IDOK;
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
            progress->onProgress(progress->label, units);
        }
    }
    return IDOK;
}

bool PatchInstallFilesOff(const std::wstring& msiPath) noexcept {
    MSIHANDLE db = 0;
    const UINT open = MsiOpenDatabaseW(msiPath.c_str(), MSIDBOPEN_TRANSACT, &db);
    if (open != ERROR_SUCCESS) {
        GlassInstallLog::LineFormat(L"PrepareRestoreMsi: open failed %u", open);
        return false;
    }
    const wchar_t* sql =
        L"UPDATE `InstallExecuteSequence` SET `Condition` = '0' WHERE `Action` = 'InstallFiles'";
    MSIHANDLE view = 0;
    UINT st = MsiDatabaseOpenViewW(db, sql, &view);
    if (st == ERROR_SUCCESS) {
        st = MsiViewExecute(view, 0);
        MsiViewClose(view);
        MsiCloseHandle(view);
    }
    if (st != ERROR_SUCCESS) {
        const wchar_t* insert =
            L"INSERT INTO `InstallExecuteSequence` (`Action`, `Condition`, `Sequence`) "
            L"VALUES ('InstallFiles', '0', 4000)";
        st = MsiDatabaseOpenViewW(db, insert, &view);
        if (st == ERROR_SUCCESS) {
            st = MsiViewExecute(view, 0);
            MsiViewClose(view);
            MsiCloseHandle(view);
        }
    }
    if (st != ERROR_SUCCESS) {
        GlassInstallLog::LineFormat(L"PrepareRestoreMsi: failed to disable InstallFiles (%u)",st);
        MsiCloseHandle(db);
        return false;
    }
    st = MsiDatabaseCommit(db);
    MsiCloseHandle(db);
    if (st != ERROR_SUCCESS)
        GlassInstallLog::LineFormat(L"PrepareRestoreMsi: commit failed %u", st);
    return st == ERROR_SUCCESS;
}

} // namespace

namespace GlassInstallMsi {

bool MsiSuccess(UINT code) noexcept {
    return code == ERROR_SUCCESS || code == ERROR_SUCCESS_REBOOT_REQUIRED ||
           code == ERROR_SUCCESS_REBOOT_INITIATED;
}

bool IsProductInstalled(const wchar_t* productCode) noexcept {
    if (!productCode || !productCode[0]) return false;
    return MsiQueryProductStateW(productCode) == INSTALLSTATE_DEFAULT;
}

bool UninstallProduct(const wchar_t* productCode) noexcept {
    if (!productCode || !productCode[0]) return true;
    const INSTALLSTATE state = MsiQueryProductStateW(productCode);
    if (state == INSTALLSTATE_ABSENT || state == INSTALLSTATE_UNKNOWN) return true;
    GlassInstallLog::LineFormat(L"MsiConfigureProductEx uninstall %s", productCode);
    MsiSetInternalUI(INSTALLUILEVEL_NONE, nullptr);
    const UINT exitCode = MsiConfigureProductExW(
        productCode, 0, INSTALLSTATE_ABSENT, L"REBOOT=ReallySuppress");
    GlassInstallLog::LineFormat(L"Uninstall exit code %u", exitCode);
    return exitCode == ERROR_SUCCESS || exitCode == ERROR_SUCCESS_REBOOT_REQUIRED ||
           exitCode == ERROR_UNKNOWN_PRODUCT;
}

std::wstring ProductCodeFromMsiPath(const std::wstring& msiPath) {
    MSIHANDLE pkg = 0;
    const UINT open = MsiOpenPackageW(msiPath.c_str(), &pkg);
    if (open != ERROR_SUCCESS) return L"";
    wchar_t value[64]{};
    DWORD cch = static_cast<DWORD>(std::size(value));
    const UINT prop = MsiGetPropertyW(pkg, L"ProductCode", value, &cch);
    MsiCloseHandle(pkg);
    if (prop != ERROR_SUCCESS || !value[0]) return L"";
    return value;
}

std::wstring KnownProductCodeFromFileName(const std::wstring& msiPath) {
    const wchar_t* name = PathFindFileNameW(msiPath.c_str());
    if (!name) return L"";
    if (_wcsicmp(name, L"Glass-x64.msi") == 0) return kProductPlatformX64;
    if (_wcsicmp(name, L"Glass-x86.msi") == 0) return kProductPlatformX86;
    if (_wcsicmp(name, L"OpenGlass-x64.msi") == 0) return kProductOpenGlass;
    if (_wcsicmp(name, L"GlassSettings-x64.msi") == 0) return kProductSettingsX64;
    if (_wcsicmp(name, L"GlassSettings-x86.msi") == 0) return kProductSettingsX86;
    return L"";
}

std::wstring ResolveProductCode(const std::wstring& msiPath) {
    std::wstring code = ProductCodeFromMsiPath(msiPath);
    if (code.empty()) code = KnownProductCodeFromFileName(msiPath);
    return code;
}

std::wstring BuildInstallPropertyString(const std::wstring& installDir, bool restoreNoCab) {
    std::wstring props = L"INSTALLDIR=" + MsiQuotedInstallDir(installDir) + L" REBOOT=ReallySuppress";
    if (restoreNoCab)
        props += L" GLASS_RESTORE_NO_CAB=1";
    return props;
}

std::wstring UninsExePathForProduct(const std::wstring& productCode, const std::wstring& installDir) {
    if (installDir.empty()) return {};
    std::wstring root = installDir;
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
        root.pop_back();
    if (_wcsicmp(productCode.c_str(), kProductSettingsX64) == 0 ||
        _wcsicmp(productCode.c_str(), kProductSettingsX86) == 0) {
        const std::wstring sub = root + L"\\GlassSettings\\unins000.exe";
        if (GetFileAttributesW(sub.c_str()) != INVALID_FILE_ATTRIBUTES)
            return sub;
    }
    const std::wstring direct = root + L"\\unins000.exe";
    if (GetFileAttributesW(direct.c_str()) != INVALID_FILE_ATTRIBUTES)
        return direct;
    return {};
}

// PublishProduct пишет msiexec + WindowsInstaller=1. Переключаем ARP на unins000.exe --product "{GUID}"
// и снимаем WindowsInstaller, чтобы Параметры Windows вызывали наш WebView2-деинсталлятор.
void PatchArpUninstallString(const std::wstring& productCode, const std::wstring& installDir) noexcept {
    if (productCode.empty() || installDir.empty()) return;
    const std::wstring exe = UninsExePathForProduct(productCode, installDir);
    if (exe.empty()) return;
    const std::wstring command = L"\"" + exe + L"\" --product \"" + productCode + L"\"";
    const std::wstring quietCommand = command + L" --silent";
    const std::wstring displayIcon = exe + L",0";
    const std::wstring keyPath =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + productCode;
    auto writeRoot = [&](HKEY root, REGSAM sam) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, keyPath.c_str(), 0, KEY_SET_VALUE | sam, &key) != ERROR_SUCCESS)
            return false;
        const LONG st1 = RegSetValueExW(key, L"UninstallString", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        const LONG st2 = RegSetValueExW(key, L"QuietUninstallString", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(quietCommand.c_str()),
            static_cast<DWORD>((quietCommand.size() + 1) * sizeof(wchar_t)));
        (void)RegSetValueExW(key, L"DisplayIcon", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(displayIcon.c_str()),
            static_cast<DWORD>((displayIcon.size() + 1) * sizeof(wchar_t)));
        const DWORD one = 1;
        (void)RegSetValueExW(key, L"NoModify", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&one), sizeof(one));
        (void)RegSetValueExW(key, L"NoRepair", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&one), sizeof(one));
        (void)RegDeleteValueW(key, L"WindowsInstaller");
        RegCloseKey(key);
        return st1 == ERROR_SUCCESS && st2 == ERROR_SUCCESS;
    };
    const bool ok = writeRoot(HKEY_CURRENT_USER, 0)
        || writeRoot(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY)
        || writeRoot(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY);
    GlassInstallLog::LineFormat(L"ARP UninstallString -> unins000 (%s) %s",
        productCode.c_str(), ok ? L"ok" : L"skip");
}

DWORD CALLBACK SnapshotCopyProgress(LARGE_INTEGER,LARGE_INTEGER,LARGE_INTEGER,LARGE_INTEGER,
    DWORD,DWORD,HANDLE,HANDLE,LPVOID context) {
    const auto* cancelled=static_cast<const std::atomic_bool*>(context);
    return cancelled && cancelled->load(std::memory_order_acquire) ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

bool CopyLocalPackage(const wchar_t* productCode, const std::wstring& destPath,
    const std::atomic_bool* cancelled) noexcept {
    if (!productCode || !productCode[0]) return false;
    wchar_t local[MAX_PATH * 2]{};
    DWORD cch = static_cast<DWORD>(std::size(local));
    if (MsiGetProductInfoW(productCode, INSTALLPROPERTY_LOCALPACKAGE, local, &cch) != ERROR_SUCCESS)
        return false;
    if (!local[0] || !PathFileExistsW(local)) return false;
    if (cancelled && cancelled->load(std::memory_order_acquire)) { SetLastError(ERROR_CANCELLED); return false; }
    return CopyFileExW(local,destPath.c_str(),SnapshotCopyProgress,const_cast<std::atomic_bool*>(cancelled),nullptr,0)!=FALSE;
}

bool PrepareRestoreMsiCopy(const std::wstring& sourceMsi, const std::wstring& destMsi) noexcept {
    if (!CopyFileW(sourceMsi.c_str(), destMsi.c_str(), FALSE))
        return false;
    return PatchInstallFilesOff(destMsi);
}

UINT InstallProduct(const std::wstring& msiPath, const std::wstring& props, ProgressSink* progress) noexcept {
    MsiSetInternalUI(INSTALLUILEVEL_NONE, nullptr);
    if (progress) {
        progress->total = 0;
        progress->sofar = 0;
        progress->script = false;
        progress->forward = true;
        progress->cancellationSent = false;
    }
    if (progress && (progress->onProgress || progress->cancelRequested)) {
        MsiSetExternalUIRecord(OnMsiUiRecord,
            INSTALLLOGMODE_PROGRESS | INSTALLLOGMODE_ACTIONSTART | INSTALLLOGMODE_ERROR
            | INSTALLLOGMODE_FATALEXIT | INSTALLLOGMODE_WARNING,
            progress, nullptr);
    }
    const UINT exitCode = MsiInstallProductW(msiPath.c_str(), props.c_str());
    MsiSetExternalUIRecord(nullptr, 0, nullptr, nullptr);
    MsiEnableLogW(0,nullptr,0);
    return exitCode;
}

std::wstring QueryDisplayVersion(const wchar_t* productCode) noexcept {
    if (!productCode) return L"";
    wchar_t value[64]{};
    DWORD cch = static_cast<DWORD>(std::size(value));
    if (MsiGetProductInfoW(productCode, INSTALLPROPERTY_VERSIONSTRING, value, &cch) != ERROR_SUCCESS)
        return L"";
    return value;
}

} // namespace GlassInstallMsi
