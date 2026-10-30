#include "GlassInstallMsi.h"
#include "GlassInstallLog.h"
#include "GlassInstallUtil.h"
#include <msi.h>
#include <msiquery.h>
#include <shlwapi.h>
#include <climits>
#include <cstring>
#include <set>
#include <vector>

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

bool UninstallProduct(const wchar_t* productCode, ProgressSink* progress, UINT* resultCode) noexcept {
    if (resultCode) *resultCode = ERROR_SUCCESS;
    if (!productCode || !productCode[0]) return true;
    const INSTALLSTATE state = MsiQueryProductStateW(productCode);
    if (state == INSTALLSTATE_ABSENT || state == INSTALLSTATE_UNKNOWN) return true;
    GlassInstallLog::LineFormat(L"MsiConfigureProductEx uninstall %s", productCode);
    MsiSetInternalUI(INSTALLUILEVEL_NONE, nullptr);
    if (progress) {
        progress->total=0; progress->sofar=0; progress->script=false;
        progress->forward=true; progress->cancellationSent=false;
        MsiSetExternalUIRecord(OnMsiUiRecord,
            INSTALLLOGMODE_PROGRESS | INSTALLLOGMODE_ACTIONSTART | INSTALLLOGMODE_ERROR
            | INSTALLLOGMODE_FATALEXIT | INSTALLLOGMODE_WARNING, progress, nullptr);
    }
    const UINT exitCode = MsiConfigureProductExW(
        productCode, 0, INSTALLSTATE_ABSENT, L"REBOOT=ReallySuppress");
    if (progress) MsiSetExternalUIRecord(nullptr, 0, nullptr, nullptr);
    if (resultCode) *resultCode = exitCode;
    GlassInstallLog::LineFormat(L"Uninstall exit code %u", exitCode);
    return exitCode == ERROR_SUCCESS || exitCode == ERROR_SUCCESS_REBOOT_REQUIRED ||
           exitCode == ERROR_UNKNOWN_PRODUCT;
}

std::wstring ProductCodeFromMsiPath(const std::wstring& msiPath) {
    // Reading a property must not start an MSI installation session: opening
    // a package can briefly show the system "Preparing to install" window,
    // even before our silent install callback is registered.
    MSIHANDLE db = 0;
    if (MsiOpenDatabaseW(msiPath.c_str(), MSIDBOPEN_READONLY, &db) != ERROR_SUCCESS) return L"";
    MSIHANDLE view = 0, record = 0;
    const wchar_t* query = L"SELECT `Value` FROM `Property` WHERE `Property` = 'ProductCode'";
    UINT result = MsiDatabaseOpenViewW(db, query, &view);
    if (result == ERROR_SUCCESS) result = MsiViewExecute(view, 0);
    if (result == ERROR_SUCCESS) result = MsiViewFetch(view, &record);
    wchar_t value[64]{};
    DWORD cch = static_cast<DWORD>(std::size(value));
    if (result == ERROR_SUCCESS) result = MsiRecordGetStringW(record, 1, value, &cch);
    if (record) MsiCloseHandle(record);
    if (view) { MsiViewClose(view); MsiCloseHandle(view); }
    MsiCloseHandle(db);
    return result == ERROR_SUCCESS && value[0] ? std::wstring(value) : std::wstring{};
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

ProductPathCheck CheckProductContexts(const wchar_t* productCode, std::wstring& detail,
    const std::atomic_bool* cancelled) noexcept {
    detail.clear();
    if (!productCode || !*productCode) return ProductPathCheck::Unverifiable;
    try {
        const auto isCancelled=[&]() { return cancelled && cancelled->load(std::memory_order_acquire); };
        const auto countContexts=[&](const wchar_t* sid,unsigned& count) {
            count=0;
            for (DWORD i=0;;++i) {
                if (isCancelled()) return false;
                const UINT rc=MsiEnumProductsExW(productCode,sid,MSIINSTALLCONTEXT_ALL,i,nullptr,nullptr,nullptr,nullptr);
                if (rc==ERROR_NO_MORE_ITEMS || (rc==ERROR_UNKNOWN_PRODUCT && i==0)) return true;
                if (rc!=ERROR_SUCCESS) { detail=L"MSI context enumeration error "+std::to_wstring(rc);return false; }
                ++count;
            }
        };
        unsigned all=0,current=0;
        if (!countContexts(L"S-1-1-0",all) || !countContexts(nullptr,current))
            return isCancelled()?ProductPathCheck::Cancelled:ProductPathCheck::Unverifiable;
        const INSTALLSTATE installed=MsiQueryProductStateW(productCode);
        if (all!=current || all>1 || (all==1 && installed!=INSTALLSTATE_DEFAULT) ||
            (all==0 && installed!=INSTALLSTATE_UNKNOWN && installed!=INSTALLSTATE_ABSENT)) {
            detail=L"Ambiguous MSI context/state: all="+std::to_wstring(all)+L" current="+
                std::to_wstring(current)+L" state="+std::to_wstring(static_cast<int>(installed));
            return ProductPathCheck::Unverifiable;
        }
        return isCancelled()?ProductPathCheck::Cancelled:ProductPathCheck::Safe;
    } catch (...) { return ProductPathCheck::Unverifiable; }
}

ProductPathCheck CheckProductFileRoots(const wchar_t* productCode, const std::wstring& cachedMsi,
    const std::wstring& fixedRoot, std::wstring& detail, const std::atomic_bool* cancelled) noexcept {
    detail.clear();
    const auto isCancelled=[&]() { return cancelled && cancelled->load(std::memory_order_acquire); };
    if (isCancelled()) return ProductPathCheck::Cancelled;
    if (!productCode || !*productCode || cachedMsi.empty() || fixedRoot.empty())
        return ProductPathCheck::Unverifiable;
    try {
        if (!GlassInstallUtil::IsSafeInstallPath(fixedRoot)) {
            detail=fixedRoot;
            return ProductPathCheck::Unverifiable;
        }
        const std::wstring cachedCode=ProductCodeFromMsiPath(cachedMsi);
        if (cachedCode.empty() || _wcsicmp(cachedCode.c_str(),productCode)!=0) {
            detail=L"Cached MSI product code does not match its registration";
            return ProductPathCheck::Unverifiable;
        }
        if (isCancelled()) return ProductPathCheck::Cancelled;
        struct MsiHandle {
            MSIHANDLE value=0;
            ~MsiHandle() { if (value) MsiCloseHandle(value); }
        } db;
        if (MsiOpenDatabaseW(cachedMsi.c_str(), MSIDBOPEN_READONLY, &db.value)!=ERROR_SUCCESS)
            return ProductPathCheck::Unverifiable;
        // Our packages do not author these tables. Their operations/custom
        // code can affect paths not represented by File component key paths.
        // Unknown legacy packages must be inspected rather than guessed safe.
        for (const wchar_t* table:{L"CustomAction",L"RemoveFile",L"MoveFile",L"DuplicateFile",
                                  L"ServiceInstall",L"ServiceControl",L"Shortcut",L"RemoveRegistry"}) {
            if (isCancelled()) return ProductPathCheck::Cancelled;
            if (MsiDatabaseIsTablePersistentW(db.value,table)!=MSICONDITION_NONE) {
                detail=std::wstring(L"Unsupported MSI table: ")+table;
                return ProductPathCheck::Unverifiable;
            }
        }
        std::set<std::wstring> seen;
        std::vector<wchar_t> path(32768);
        // File components and explicitly created empty-folder components can
        // both be removed by MSI. Query their registered key paths without
        // starting an install session. Registry key paths fail containment.
        const auto countRows=[&](const wchar_t* sql,unsigned& count) -> ProductPathCheck {
            count=0;MsiHandle view;
            if (MsiDatabaseOpenViewW(db.value,sql,&view.value)!=ERROR_SUCCESS ||
                MsiViewExecute(view.value,0)!=ERROR_SUCCESS) return ProductPathCheck::Unverifiable;
            for (;;) {
                if (isCancelled()) return ProductPathCheck::Cancelled;
                MsiHandle row;const UINT status=MsiViewFetch(view.value,&row.value);
                if (status==ERROR_NO_MORE_ITEMS) break;
                if (status!=ERROR_SUCCESS || !row.value || count==UINT_MAX) return ProductPathCheck::Unverifiable;
                ++count;
            }
            MsiViewClose(view.value);return ProductPathCheck::Safe;
        };
        const auto checkRows=[&](const wchar_t* sql,bool requireRows,bool folderRows,unsigned& rows) -> ProductPathCheck {
            rows=0;
            MsiHandle view;
            if (MsiDatabaseOpenViewW(db.value,sql,&view.value)!=ERROR_SUCCESS ||
                MsiViewExecute(view.value,0)!=ERROR_SUCCESS) return ProductPathCheck::Unverifiable;
            for (;;) {
                if (isCancelled()) return ProductPathCheck::Cancelled;
                MsiHandle row;
                const UINT status=MsiViewFetch(view.value,&row.value);
                if (status==ERROR_NO_MORE_ITEMS) break;
                if (status!=ERROR_SUCCESS || !row.value || rows==UINT_MAX) return ProductPathCheck::Unverifiable;
                ++rows;
                wchar_t id[64]{};
                DWORD idSize=static_cast<DWORD>(std::size(id));
                if (MsiRecordGetStringW(row.value,1,id,&idSize)!=ERROR_SUCCESS || !id[0])
                    return ProductPathCheck::Unverifiable;
                if (folderRows) {
                    // CreateFolder can refer to a different directory than its
                    // component's key path. Accept only our generated shape:
                    // the same directory and a directory (null) key path.
                    wchar_t folderId[256]{},componentDir[256]{};
                    DWORD folderSize=static_cast<DWORD>(std::size(folderId));
                    DWORD dirSize=static_cast<DWORD>(std::size(componentDir));
                    if (MsiRecordGetStringW(row.value,2,folderId,&folderSize)!=ERROR_SUCCESS || !folderId[0] ||
                        MsiRecordGetStringW(row.value,3,componentDir,&dirSize)!=ERROR_SUCCESS || !componentDir[0] ||
                        _wcsicmp(folderId,componentDir)!=0 || !MsiRecordIsNull(row.value,4)) {
                        detail=std::wstring(L"Unsupported CreateFolder component: ")+id;
                        return ProductPathCheck::Unverifiable;
                    }
                }
                if (!seen.emplace(id).second) continue;
                path[0]=L'\0';
                DWORD size=static_cast<DWORD>(path.size());
                const INSTALLSTATE state=MsiGetComponentPathW(productCode,id,path.data(),&size);
                if (state!=INSTALLSTATE_LOCAL || !path[0] || size>=path.size()) {
                    detail=std::wstring(L"component ")+id+L" (state="+std::to_wstring(static_cast<int>(state))+L")";
                    return ProductPathCheck::Unverifiable;
                }
                if (!GlassInstallUtil::IsWithinInstallRoot(fixedRoot,path.data())) {
                    detail=path.data();
                    return ProductPathCheck::OutsideRoot;
                }
                if (!GlassInstallUtil::IsSafeInstallPath(path.data())) {
                    detail=path.data();
                    return ProductPathCheck::Unverifiable;
                }
            }
            MsiViewClose(view.value);
            return requireRows && !rows?ProductPathCheck::Unverifiable:ProductPathCheck::Safe;
        };
        const wchar_t* files=L"SELECT `Component`.`ComponentId` FROM `File`, `Component` "
            L"WHERE `File`.`Component_` = `Component`.`Component`";
        unsigned total=0,matched=0;
        ProductPathCheck result=countRows(L"SELECT `Component_` FROM `File`",total);
        if (result!=ProductPathCheck::Safe) return result;
        result=checkRows(files,true,false,matched);
        if (result!=ProductPathCheck::Safe) return result;
        if (total!=matched) { detail=L"Unmatched File components";return ProductPathCheck::Unverifiable; }
        const MSICONDITION folders=MsiDatabaseIsTablePersistentW(db.value,L"CreateFolder");
        if (folders==MSICONDITION_TRUE) {
            result=countRows(L"SELECT `Component_` FROM `CreateFolder`",total);
            if (result!=ProductPathCheck::Safe) return result;
            const wchar_t* sql=L"SELECT `Component`.`ComponentId`, `CreateFolder`.`Directory_`, "
                L"`Component`.`Directory_`, `Component`.`KeyPath` FROM `CreateFolder`, `Component` "
                L"WHERE `CreateFolder`.`Component_` = `Component`.`Component`";
            result=checkRows(sql,false,true,matched);
            if (result!=ProductPathCheck::Safe) return result;
            if (total!=matched) { detail=L"Unmatched CreateFolder components";return ProductPathCheck::Unverifiable; }
        } else if (folders!=MSICONDITION_NONE) return ProductPathCheck::Unverifiable;
        GlassInstallLog::LineFormat(L"MSI component paths verified under fixed root: product=%s components=%u root=%s",
            productCode, static_cast<unsigned>(seen.size()), fixedRoot.c_str());
        return ProductPathCheck::Safe;
    } catch (...) {
        return ProductPathCheck::Unverifiable;
    }
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
