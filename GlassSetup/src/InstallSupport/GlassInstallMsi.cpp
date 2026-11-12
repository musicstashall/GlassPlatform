#include "GlassAppsNotification.h"
#include "GlassInstallMsi.h"
#include "GlassInstallLog.h"
#include "GlassInstallUtil.h"
#include "GlassMaintenanceCache.h"
#include "GlassOpenGlassFiles.h"
#include "GlassOpenGlassInstall.h"
#include <SecureUxThemeInventory.h>
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

void RemoveOrphanUninstallEntry(const wchar_t* productCode) noexcept {
    if (!productCode || !productCode[0]) return;
    if (IsProductInstalled(productCode)) return;
    const std::wstring sub =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + std::wstring(productCode);
    HKEY probe = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &probe) != ERROR_SUCCESS)
        return;
    RegCloseKey(probe);
    const LONG st = RegDeleteTreeW(HKEY_LOCAL_MACHINE, sub.c_str());
    GlassInstallLog::LineFormat(L"Removed orphan ARP uninstall key %s (0x%08lX)", productCode, st);
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
    if(exitCode==ERROR_SUCCESS||exitCode==ERROR_SUCCESS_REBOOT_REQUIRED||exitCode==ERROR_UNKNOWN_PRODUCT)
        GlassAppsNotification::Changed();
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
    if (_wcsicmp(name, L"OpenGlassResources-x64.msi") == 0) return kProductOpenGlassResourcesX64;
    if (_wcsicmp(name, L"OpenGlass-x64.msi") == 0) return kProductOpenGlass;
    if (_wcsicmp(name, L"GlassSettings-x64.msi") == 0) return kProductSettingsX64;
    if (_wcsicmp(name, L"GlassSettings-x86.msi") == 0) return kProductSettingsX86;
    if (_wcsicmp(name, L"SecureUxTheme-x64.msi") == 0) return kProductSecureUxThemeX64;
    if (_wcsicmp(name, L"SecureUxTheme-x86.msi") == 0) return kProductSecureUxThemeX86;
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

// Graphics MSI keeps Windows Installer ownership, but its ARP Change action
// opens the trusted product-aware controller, not a UI-less msiexec /i run.
static bool PatchResourcesMaintenance() {
    std::wstring detail;
    if(!IsProductInstalled(kProductOpenGlassResourcesX64)||
       CheckProductContexts(kProductOpenGlassResourcesX64,detail)!=ProductPathCheck::Safe)return false;
    const auto setup=GlassMaintenanceCache::CurrentFile(L"GlassSetup.exe");
    if(setup.empty())return false; // Never redirect to an untrusted/absent controller.
    wchar_t system[MAX_PATH]{};
    const UINT length=GetSystemDirectoryW(system,MAX_PATH);
    if(!length||length>=MAX_PATH)return false;
    const auto uninstall=L"\""+std::wstring(system)+L"\\msiexec.exe\" /x \""+kProductOpenGlassResourcesX64+L"\"";
    const auto modify=L"\""+setup+L"\" --modify --product \""+kProductOpenGlassResourcesX64+L"\"";
    const auto path=L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"+std::wstring(kProductOpenGlassResourcesX64);
    HKEY key=nullptr;
    if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,path.c_str(),0,KEY_SET_VALUE|KEY_WOW64_64KEY,&key)!=ERROR_SUCCESS)return false;
    const auto put=[&](const wchar_t* name,const std::wstring& value) {
        return RegSetValueExW(key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS;
    };
    bool ok=IsProductInstalled(kProductOpenGlassResourcesX64)&&
        CheckProductContexts(kProductOpenGlassResourcesX64,detail)==ProductPathCheck::Safe;
    if(ok) {
        ok=put(L"DisplayIcon",setup+L",0")&&put(L"ModifyPath",modify)&&put(L"UninstallString",uninstall)&&put(L"QuietUninstallString",uninstall+L" /qn /norestart");
        const DWORD zero=0,one=1;
        ok=RegSetValueExW(key,L"NoModify",0,REG_DWORD,reinterpret_cast<const BYTE*>(&zero),sizeof(zero))==ERROR_SUCCESS&&ok;
        ok=RegSetValueExW(key,L"NoRepair",0,REG_DWORD,reinterpret_cast<const BYTE*>(&one),sizeof(one))==ERROR_SUCCESS&&ok;
        if(ok) {
            const auto removed=RegDeleteValueW(key,L"WindowsInstaller");
            ok=removed==ERROR_SUCCESS||removed==ERROR_FILE_NOT_FOUND;
        }
    }
    RegCloseKey(key);
    GlassInstallLog::LineFormat(L"Resources ARP maintenance redirect: %s",ok?L"ok":L"incomplete");
    return ok;
}

// PublishProduct пишет msiexec + WindowsInstaller=1. Переключаем ARP на unins000.exe --product "{GUID}"
// и снимаем WindowsInstaller, чтобы Параметры Windows вызывали наш WebView2-деинсталлятор.
bool PatchArpUninstallString(const std::wstring& productCode, const std::wstring& installDir) noexcept {
    try {
        bool managed=false;
        for(const auto* code:{kProductPlatformX64,kProductPlatformX86,kProductSettingsX64,kProductSettingsX86,
            kProductOpenGlass,kLegacyPlatformX64,kLegacyPlatformX86,kLegacyOpenGlass,kProductSecureUxThemeX64,kProductSecureUxThemeX86})
            managed=managed||_wcsicmp(productCode.c_str(),code)==0;
        std::wstring contextDetail;
        if(!managed || !IsProductInstalled(productCode.c_str()) ||
           CheckProductContexts(productCode.c_str(),contextDetail)!=ProductPathCheck::Safe)return false;
        wchar_t registered[39]{};MSIINSTALLCONTEXT context=MSIINSTALLCONTEXT_NONE;
        if(MsiEnumProductsExW(productCode.c_str(),nullptr,MSIINSTALLCONTEXT_ALL,0,registered,&context,nullptr,nullptr)!=ERROR_SUCCESS)return false;
        if(context!=MSIINSTALLCONTEXT_MACHINE && context!=MSIINSTALLCONTEXT_USERMANAGED && context!=MSIINSTALLCONTEXT_USERUNMANAGED)return false;
        const bool secureUxThemeProduct =
            _wcsicmp(productCode.c_str(), kProductSecureUxThemeX64) == 0 ||
            _wcsicmp(productCode.c_str(), kProductSecureUxThemeX86) == 0;
        if (installDir.empty() && !secureUxThemeProduct) return false;
        std::wstring exe = GlassMaintenanceCache::CurrentFile(L"GlassUnins.exe");
        if(exe.empty())exe=UninsExePathForProduct(productCode, installDir);
        if (exe.empty()) return false;
        const std::wstring command = L"\"" + exe + L"\" --product \"" + productCode + L"\"";
        const std::wstring quietCommand = command + L" --silent";
        const auto setup=GlassMaintenanceCache::CurrentFile(L"GlassSetup.exe");
        const auto modify=setup.empty()?L"\""+exe+L"\" --interactive --product \""+productCode+L"\"":L"\""+setup+L"\" --modify --product \""+productCode+L"\"";
        const std::wstring displayIcon = exe + L",0";
        const std::wstring keyPath =
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + productCode;
        const bool settings=_wcsicmp(productCode.c_str(),kProductSettingsX64)==0||_wcsicmp(productCode.c_str(),kProductSettingsX86)==0;
        std::vector<std::wstring> sutCandidates;
        const bool sutGuard=Shared::SecureUxThemeInventory::Query(sutCandidates)!=ERROR_SUCCESS || !sutCandidates.empty() ||
            MsiQueryProductStateW(kProductSecureUxThemeX64)!=INSTALLSTATE_UNKNOWN ||
            MsiQueryProductStateW(kProductSecureUxThemeX86)!=INSTALLSTATE_UNKNOWN;
        bool opened=false;
        auto writeRoot = [&](HKEY root, REGSAM sam) {
            HKEY key = nullptr;
            const LONG status=RegOpenKeyExW(root,keyPath.c_str(),0,KEY_SET_VALUE|sam,&key);
            if(status!=ERROR_SUCCESS) {
                // Only a missing key permits trying the other registry view.
                // An access failure must not redirect writes to a stale key.
                opened=status!=ERROR_FILE_NOT_FOUND && status!=ERROR_PATH_NOT_FOUND;
                return false;
            }
            opened=true;
            wchar_t currentCode[39]{};MSIINSTALLCONTEXT currentContext=MSIINSTALLCONTEXT_NONE;
            if(!IsProductInstalled(productCode.c_str()) ||
               CheckProductContexts(productCode.c_str(),contextDetail)!=ProductPathCheck::Safe ||
               MsiEnumProductsExW(productCode.c_str(),nullptr,MSIINSTALLCONTEXT_ALL,0,currentCode,&currentContext,nullptr,nullptr)!=ERROR_SUCCESS ||
               currentContext!=context){RegCloseKey(key);return false;}
            const LONG st1 = RegSetValueExW(key, L"UninstallString", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(quietCommand.c_str()),
                static_cast<DWORD>((quietCommand.size() + 1) * sizeof(wchar_t)));
            const LONG st2 = RegSetValueExW(key, L"QuietUninstallString", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(quietCommand.c_str()),
                static_cast<DWORD>((quietCommand.size() + 1) * sizeof(wchar_t)));
            bool valuesOk=true;
            // Upgrade old same-name registrations as well as new MSI metadata.
            // Write only after the existing context/ownership recheck above.
            if(settings) {
                const std::wstring title=_wcsicmp(productCode.c_str(),kProductSettingsX64)==0?
                    L"GlassSettings (64-bit)":L"GlassSettings (32-bit)";
                valuesOk=RegSetValueExW(key,L"DisplayName",0,REG_SZ,
                    reinterpret_cast<const BYTE*>(title.c_str()),static_cast<DWORD>((title.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS;
            }
            valuesOk=RegSetValueExW(key, L"DisplayIcon", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(displayIcon.c_str()),
                static_cast<DWORD>((displayIcon.size() + 1) * sizeof(wchar_t)))==ERROR_SUCCESS && valuesOk;
            const DWORD one = 1,zero=0;
            valuesOk=RegSetValueExW(key,L"ModifyPath",0,REG_SZ,reinterpret_cast<const BYTE*>(modify.c_str()),static_cast<DWORD>((modify.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS && valuesOk;
            valuesOk=RegSetValueExW(key,L"NoModify",0,REG_DWORD,reinterpret_cast<const BYTE*>(&zero),sizeof(zero))==ERROR_SUCCESS && valuesOk;
            const DWORD noRemove=settings&&sutGuard?1:0;
            valuesOk=RegSetValueExW(key,L"NoRemove",0,REG_DWORD,reinterpret_cast<const BYTE*>(&noRemove),sizeof(noRemove))==ERROR_SUCCESS && valuesOk;
            valuesOk=RegSetValueExW(key, L"NoRepair", 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&one), sizeof(one))==ERROR_SUCCESS && valuesOk;
            if(st1==ERROR_SUCCESS && st2==ERROR_SUCCESS && valuesOk) {
                const LONG deleted=RegDeleteValueW(key,L"WindowsInstaller");
                valuesOk=deleted==ERROR_SUCCESS || deleted==ERROR_FILE_NOT_FOUND;
            }
            if(st1!=ERROR_SUCCESS || st2!=ERROR_SUCCESS || !valuesOk)
                GlassInstallLog::LineFormat(L"ARP write incomplete: product=%s view=%u uninstall=%ld quiet=%ld policyOk=%d",
                    productCode.c_str(),static_cast<unsigned>(sam),st1,st2,valuesOk?1:0);
            RegCloseKey(key);
            return st1 == ERROR_SUCCESS && st2 == ERROR_SUCCESS && valuesOk;
        };
        // Do not fall through from a machine instance to an unrelated HKCU key.
        const HKEY root=context==MSIINSTALLCONTEXT_MACHINE?HKEY_LOCAL_MACHINE:HKEY_CURRENT_USER;
        const bool product32=_wcsicmp(productCode.c_str(),kProductSettingsX86)==0 ||
            _wcsicmp(productCode.c_str(),kProductPlatformX86)==0 ||
            _wcsicmp(productCode.c_str(),kLegacyPlatformX86)==0 ||
            _wcsicmp(productCode.c_str(),kProductSecureUxThemeX86)==0;
        const REGSAM primaryView=product32?KEY_WOW64_32KEY:KEY_WOW64_64KEY;
        const REGSAM legacyView=product32?KEY_WOW64_64KEY:KEY_WOW64_32KEY;
        bool ok=writeRoot(root,primaryView);
        if(!opened)ok=writeRoot(root,legacyView);
        GlassInstallLog::LineFormat(L"ARP UninstallString -> unins000 (%s) %s",
            productCode.c_str(), ok ? L"ok" : L"skip");
        return ok;
    }catch(...){return false;}
}

// A separate, non-MSI registration. Never adopt/delete the upstream Inno key
// or reuse the retired Glass MSI identity as an ARP registry key.
static bool RefreshOpenGlassFileArp(const std::wstring& root) {
    if(root.empty())return true; // No file-product root: do not publish or delete any key.
    constexpr auto path=L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\GlassPlatform.OpenGlass";
    constexpr auto owner=L"GlassPlatform.OpenGlass.Files.v1";
    HKEY key=nullptr;
    auto status=RegOpenKeyExW(HKEY_LOCAL_MACHINE,path,0,KEY_READ|KEY_WRITE|KEY_WOW64_64KEY,&key);
    if(status!=ERROR_SUCCESS&&status!=ERROR_FILE_NOT_FOUND)return false;
    if(key) {
        wchar_t marker[80]{};DWORD type=0,bytes=sizeof(marker);
        const bool ours=RegQueryValueExW(key,L"GlassOwner",nullptr,&type,reinterpret_cast<BYTE*>(marker),&bytes)==ERROR_SUCCESS&&
            type==REG_SZ&&bytes==sizeof(L"GlassPlatform.OpenGlass.Files.v1")&&wcscmp(marker,owner)==0;
        if(!ours){RegCloseKey(key);return false;}
    }
    const bool present=GlassOpenGlassFiles::HasReceipt(root);
    if(!present) {
        if(!key)return true;
        RegCloseKey(key);
        return RegDeleteKeyExW(HKEY_LOCAL_MACHINE,path,KEY_WOW64_64KEY,0)==ERROR_SUCCESS;
    }
    GlassOpenGlassFiles::Record record;
    try{record=GlassOpenGlassFiles::Inspect(root);}catch(...){if(key)RegCloseKey(key);return false;}
    const auto exe=GlassMaintenanceCache::CurrentFile(L"GlassUnins.exe"),setup=GlassMaintenanceCache::CurrentFile(L"GlassSetup.exe");
    if(!record.present||exe.empty()||setup.empty()){if(key)RegCloseKey(key);return false;}
    if(!key&&RegCreateKeyExW(HKEY_LOCAL_MACHINE,path,0,nullptr,0,KEY_READ|KEY_WRITE|KEY_WOW64_64KEY,nullptr,&key,nullptr)!=ERROR_SUCCESS)return false;
    const auto put=[&](const wchar_t* name,const std::wstring& value){return RegSetValueExW(key,name,0,REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS;};
    const std::wstring command=L"\""+exe+L"\" --product \""+kProductOpenGlass+L"\"";
    bool ok=put(L"GlassOwner",owner);
    ok=put(L"DisplayName",L"OpenGlass (GlassPlatform)")&&ok;
    ok=put(L"DisplayVersion",record.version)&&ok;
    ok=put(L"InstallLocation",root)&&ok;
    ok=put(L"DisplayIcon",exe+L",0")&&ok;
    ok=put(L"UninstallString",command+L" --interactive")&&ok;
    ok=put(L"QuietUninstallString",command+L" --silent")&&ok;
    ok=put(L"ModifyPath",L"\""+setup+L"\" --modify --product \""+kProductOpenGlass+L"\"")&&ok;
    const DWORD one=1;
    ok=(RegSetValueExW(key,L"NoRepair",0,REG_DWORD,reinterpret_cast<const BYTE*>(&one),sizeof(one))==ERROR_SUCCESS)&&ok;
    RegCloseKey(key);return ok;
}

// Augment only the unambiguous, validated official Inno registration.
// Its uninstall commands, identity and service ownership remain upstream-owned.
static bool PatchOfficialOpenGlassMaintenance() {
    GlassOpenGlassInstall::Record before;std::wstring error;
    if(!GlassOpenGlassInstall::Query(before,error))return false;
    if(!before.installed)return true;
    const auto setup=GlassMaintenanceCache::CurrentFile(L"GlassSetup.exe");
    if(setup.empty())return false;
    HKEY key=nullptr;
    if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,before.keyPath.c_str(),0,KEY_SET_VALUE|KEY_WOW64_64KEY,&key)!=ERROR_SUCCESS)return false;
    GlassOpenGlassInstall::Record after;
    bool ok=GlassOpenGlassInstall::Query(after,error)&&GlassOpenGlassInstall::Same(before,after);
    if(ok) {
        const auto modify=L"\""+setup+L"\" --modify --product \""+kProductOpenGlass+L"\"";
        const DWORD zero=0;
        ok=RegSetValueExW(key,L"ModifyPath",0,REG_SZ,reinterpret_cast<const BYTE*>(modify.c_str()),
            static_cast<DWORD>((modify.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS;
        if(ok)ok=RegSetValueExW(key,L"NoModify",0,REG_DWORD,reinterpret_cast<const BYTE*>(&zero),sizeof(zero))==ERROR_SUCCESS;
    }
    RegCloseKey(key);
    GlassInstallLog::LineFormat(L"Official OpenGlass Change redirect: %s",ok?L"ok":L"incomplete");
    return ok;
}

bool RefreshManagedArp(const std::wstring& x64Dir,const std::wstring& x86Dir,const std::wstring& openGlassDir) noexcept {
    try {
        bool ok=true;
        for(const auto* code:{kProductPlatformX64,kProductPlatformX86,kProductSettingsX64,kProductSettingsX86,
            kProductOpenGlass,kLegacyPlatformX64,kLegacyPlatformX86,kLegacyOpenGlass,kProductSecureUxThemeX64,kProductSecureUxThemeX86}) {
            const auto state=MsiQueryProductStateW(code);
            if(state==INSTALLSTATE_UNKNOWN)continue;
            if(state!=INSTALLSTATE_DEFAULT){
                GlassInstallLog::LineFormat(L"Remaining ARP skipped: product=%s MSI state=%d",code,static_cast<int>(state));
                ok=false;continue;
            }
            const bool product32=_wcsicmp(code,kProductPlatformX86)==0||_wcsicmp(code,kLegacyPlatformX86)==0||
                _wcsicmp(code,kProductSettingsX86)==0||_wcsicmp(code,kProductSecureUxThemeX86)==0;
            const bool og=_wcsicmp(code,kProductOpenGlass)==0||_wcsicmp(code,kLegacyOpenGlass)==0;
            if(!PatchArpUninstallString(code,og?openGlassDir:product32?x86Dir:x64Dir)) {
                GlassInstallLog::LineFormat(L"Remaining ARP refresh refused/failed: product=%s",code);ok=false;
            }
        }
        if(IsProductInstalled(kProductOpenGlassResourcesX64))ok=PatchResourcesMaintenance()&&ok;
        ok=RefreshOpenGlassFileArp(openGlassDir)&&ok;
        ok=PatchOfficialOpenGlassMaintenance()&&ok;
        GlassAppsNotification::Changed();
        GlassInstallLog::LineFormat(L"Remaining managed ARP refresh: %s",ok?L"ok":L"incomplete");
        return ok;
    }catch(...){return false;}
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
                wchar_t installedCode[39]{};
                const UINT rc=MsiEnumProductsExW(productCode,sid,MSIINSTALLCONTEXT_ALL,i,installedCode,nullptr,nullptr,nullptr);
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
                if (state==INSTALLSTATE_ABSENT) continue; // Not installed: no component resource to remove.
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
        GlassInstallLog::LineFormat(L"MSI component paths verified under installation root: product=%s components=%u root=%s",
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
