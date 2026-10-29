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
    const UINT kind = messageType & 0xFF000000;
    if (!progress || !record) return IDOK;
    if (kind == INSTALLMESSAGE_ERROR || kind == INSTALLMESSAGE_FATALEXIT) {
        wchar_t text[1024]{};
        DWORD size = static_cast<DWORD>(std::size(text));
        if (MsiFormatRecordW(NULL, record, text, &size) == ERROR_SUCCESS && text[0])
            GlassInstallLog::LineFormat(L"MSI: %s", text);
        return IDOK;
    }
    if (kind != INSTALLMESSAGE_PROGRESS || !progress->onProgress) return IDOK;
    const int field1 = MsiRecordGetInteger(record, 1);
    static int total = 0;
    static int sofar = 0;
    static bool script = false;
    static bool forward = true;
    if (field1 == 0) {
        script = MsiRecordGetInteger(record, 4) == 1;
        if (!script) return IDOK;
        total = MsiRecordGetInteger(record, 2);
        if (total < 1) total = 1;
        forward = MsiRecordGetInteger(record, 3) == 0;
        sofar = forward ? 0 : total;
    } else if (field1 == 2 && script) {
        const int delta = MsiRecordGetInteger(record, 2);
        if (delta > 0) {
            sofar += forward ? delta : -delta;
            if (sofar < 0) sofar = 0;
            if (sofar > total) sofar = total;
            const int units = progress->base + static_cast<int>(
                (static_cast<long long>(sofar) * progress->span) / total);
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
    if (_wcsicmp(name, L"Glass-x64.msi") == 0) return L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5071}";
    if (_wcsicmp(name, L"Glass-x86.msi") == 0) return L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5072}";
    if (_wcsicmp(name, L"OpenGlass-x64.msi") == 0) return L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5073}";
    if (_wcsicmp(name, L"GlassSettings-x64.msi") == 0) return L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5074}";
    if (_wcsicmp(name, L"GlassSettings-x86.msi") == 0) return L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5075}";
    return L"";
}

std::wstring ResolveProductCode(const std::wstring& msiPath) {
    std::wstring code = ProductCodeFromMsiPath(msiPath);
    if (code.empty()) code = KnownProductCodeFromFileName(msiPath);
    return code;
}

std::wstring SettingsProductCodeForPlatform(const wchar_t* platformProductCode) noexcept {
    if (!platformProductCode) return L"";
    if (_wcsicmp(platformProductCode, L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5071}") == 0)
        return L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5074}";
    if (_wcsicmp(platformProductCode, L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5072}") == 0)
        return L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5075}";
    return L"";
}

std::wstring BuildInstallPropertyString(const std::wstring& installDir, bool restoreNoCab) {
    std::wstring props = L"INSTALLDIR=" + MsiQuotedInstallDir(installDir) + L" REBOOT=ReallySuppress";
    if (restoreNoCab)
        props += L" GLASS_RESTORE_NO_CAB=1";
    return props;
}

bool CopyLocalPackage(const wchar_t* productCode, const std::wstring& destPath) noexcept {
    if (!productCode || !productCode[0]) return false;
    wchar_t local[MAX_PATH * 2]{};
    DWORD cch = static_cast<DWORD>(std::size(local));
    if (MsiGetProductInfoW(productCode, INSTALLPROPERTY_LOCALPACKAGE, local, &cch) != ERROR_SUCCESS)
        return false;
    if (!local[0] || !PathFileExistsW(local)) return false;
    return CopyFileW(local, destPath.c_str(), FALSE) != FALSE;
}

bool PrepareRestoreMsiCopy(const std::wstring& sourceMsi, const std::wstring& destMsi) noexcept {
    if (!CopyFileW(sourceMsi.c_str(), destMsi.c_str(), FALSE))
        return false;
    return PatchInstallFilesOff(destMsi);
}

UINT InstallProduct(const std::wstring& msiPath, const std::wstring& props, ProgressSink* progress) noexcept {
    MsiSetInternalUI(INSTALLUILEVEL_NONE, nullptr);
    if (progress && progress->onProgress) {
        MsiSetExternalUIRecord(OnMsiUiRecord,
            INSTALLLOGMODE_PROGRESS | INSTALLLOGMODE_ERROR | INSTALLLOGMODE_FATALEXIT | INSTALLLOGMODE_WARNING,
            progress, nullptr);
    }
    const UINT exitCode = MsiInstallProductW(msiPath.c_str(), props.c_str());
    MsiSetExternalUIRecord(nullptr, 0, nullptr, nullptr);
    return exitCode;
}

bool ReinstallProduct(const wchar_t* productCode, const wchar_t* installDir) noexcept {
    if (!productCode || !installDir) return false;
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp)) return false;
    wchar_t dest[MAX_PATH]{};
    if (!PathCombineW(dest, temp, L"GlassRestore.msi")) return false;
    if (!CopyLocalPackage(productCode, dest)) return false;
    wchar_t patched[MAX_PATH]{};
    if (!GetTempFileNameW(temp, L"grs", 0, patched)) return false;
    DeleteFileW(patched);
    wcscat_s(patched, L".msi");
    if (!PrepareRestoreMsiCopy(dest, patched)) return false;
    const std::wstring props = BuildInstallPropertyString(installDir, true);
    GlassInstallLog::LineFormat(L"Restore MSI reinstall %s", productCode);
    const UINT code = InstallProduct(patched, props, nullptr);
    DeleteFileW(patched);
    return MsiSuccess(code);
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
