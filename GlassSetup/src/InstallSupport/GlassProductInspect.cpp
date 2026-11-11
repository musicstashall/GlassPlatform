#include "GlassOpenGlassFiles.h"
#include "GlassSetupSourceLayout.h"
#include "GlassProductInspect.h"
#include "GlassPackageAcquisition.h"
#include "GlassPackageCatalog.h"
#include "GlassOpenGlassRelease.h"
#include "GlassReleaseClient.h"
#include "GlassMaintenanceCache.h"
#include <SecureUxThemeInventory.h>

#include "GlassInstallHost.h"
#include "GlassInstallRemoval.h"
#include "GlassInstallMsi.h"
#include "GlassInstallLog.h"
#include "GlassInstallUtil.h"

#include <GlassPlatformTask.h>
#include <RegistrySanity.h>

#include <msi.h>
#include <msiquery.h>
#include <softpub.h>
#include <wintrust.h>

#include <array>
#include <cstring>
#include <cstdlib>
#include <cwctype>
#include <type_traits>
#include <string>
#include <vector>

#pragma comment(lib, "msi.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace GlassProductInspect {
namespace {

void AddReason(ProductReport& rep, Reason reason) noexcept
{
    for (Reason existing : rep.reasons)
        if (existing == reason)
            return;
    rep.reasons.push_back(reason);
    rep.damaged = true;
}

[[nodiscard]] std::wstring LongFileName(const wchar_t* raw) noexcept
{
    std::wstring name = raw ? raw : L"";
    const auto bar = name.find(L'|');
    return bar == std::wstring::npos ? name : name.substr(bar + 1);
}

// Версия файла из колонки Version таблицы File построенного MSI.
[[nodiscard]] std::wstring QueryFileVersion(MSIHANDLE database, const wchar_t* longName) noexcept
{
    MSIHANDLE view = 0;
    if (MsiDatabaseOpenViewW(database, L"SELECT `FileName`, `Version` FROM `File`", &view) != ERROR_SUCCESS)
        return {};
    std::wstring version;
    if (MsiViewExecute(view, 0) == ERROR_SUCCESS)
    {
        MSIHANDLE record = 0;
        while (MsiViewFetch(view, &record) == ERROR_SUCCESS)
        {
            wchar_t name[256]{};
            DWORD cch = _countof(name);
            if (MsiRecordGetStringW(record, 1, name, &cch) == ERROR_SUCCESS &&
                _wcsicmp(LongFileName(name).c_str(), longName) == 0)
            {
                wchar_t value[64]{};
                DWORD vcch = _countof(value);
                if (MsiRecordGetStringW(record, 2, value, &vcch) == ERROR_SUCCESS)
                    version = value;
                MsiCloseHandle(record);
                break;
            }
            MsiCloseHandle(record);
        }
    }
    MsiViewClose(view);
    MsiCloseHandle(view);
    return version;
}

// SHA-1 отпечаток подписи файла (тот же приём, что в QuerySelfSignerInfo Setup).
DWORD GetFileSignerSha1(const std::wstring& path, std::wstring& hexOut) noexcept
{
    hexOut.clear();
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA trust{};
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &action, &trust);
    // Цепочка извлекается независимо от доверия — как в QuerySelfSignerInfo Setup:
    // самоподписанный сертификат автора на машине без установленного доверия
    // тоже распознаётся.
    {
        CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(trust.hWVTStateData);
        CRYPT_PROVIDER_SGNR* signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        PCCERT_CONTEXT leaf = (signer && signer->csCertChain && signer->pasCertChain)
            ? signer->pasCertChain[0].pCert : nullptr;
        if (leaf)
        {
            BYTE hash[20]{};
            DWORD cb = sizeof(hash);
            if (CertGetCertificateContextProperty(leaf, CERT_SHA1_HASH_PROP_ID, hash, &cb))
            {
                static const wchar_t* kHex = L"0123456789ABCDEF";
                hexOut.resize(40);
                for (unsigned i = 0; i < 20; ++i)
                {
                    hexOut[i * 2] = kHex[hash[i] >> 4];
                    hexOut[i * 2 + 1] = kHex[hash[i] & 0xFu];
                }
            }
        }
    }
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &trust);
    return static_cast<DWORD>(status);
}

std::wstring SelfSignerSha1() noexcept
{
    wchar_t self[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH))
        return {};
    std::wstring hex;
    GetFileSignerSha1(self, hex);
    return hex;
}

std::wstring QueryMsiProperty(MSIHANDLE database, const wchar_t* name) noexcept
{
    std::wstring sql = std::wstring(L"SELECT `Value` FROM `Property` WHERE `Property`='") + name + L"'";
    MSIHANDLE view = 0;
    if (MsiDatabaseOpenViewW(database, sql.c_str(), &view) != ERROR_SUCCESS)
        return {};
    std::wstring value;
    if (MsiViewExecute(view, 0) == ERROR_SUCCESS)
    {
        MSIHANDLE record = 0;
        if (MsiViewFetch(view, &record) == ERROR_SUCCESS)
        {
            wchar_t buf[256]{};
            DWORD cch = _countof(buf);
            if (MsiRecordGetStringW(record, 1, buf, &cch) == ERROR_SUCCESS)
                value = buf;
            MsiCloseHandle(record);
        }
    }
    MsiViewClose(view);
    MsiCloseHandle(view);
    return value;
}

struct FileCheck
{
    std::wstring componentId;
    std::wstring fileName; // длинное имя из колонки FileName (short|long)
    unsigned long long size = 0;
    DWORD hash[4]{};
    bool hasHash = false;
};

// Пользовательские каталоги OpenGlass (материалы/атласы/отражения) — их пользователь
// вправе удалять, целостность этих файлов не проверяется.
[[nodiscard]] bool IsOpenGlassUserResource(const std::wstring& path) noexcept
{
    std::wstring lower = path;
    for (auto& c : lower)
        c = static_cast<wchar_t>(towlower(c));
    return lower.find(L"\\resources\\") != std::wstring::npos;
}

bool EnumProductFiles(MSIHANDLE database, std::vector<FileCheck>& out) noexcept
{
    out.clear();
    const wchar_t* kSqlWithHash =
        L"SELECT `Component`.`ComponentId`, `File`.`FileSize`, `File`.`FileName`, "
        L"`MsiFileHash`.`Hash1`, `MsiFileHash`.`Hash2`, `MsiFileHash`.`Hash3`, `MsiFileHash`.`Hash4` "
        L"FROM `File`, `Component`, `MsiFileHash` "
        L"WHERE `Component`.`Component` = `File`.`Component_` AND `MsiFileHash`.`File_` = `File`.`File`";
    const wchar_t* kSqlPlain =
        L"SELECT `Component`.`ComponentId`, `File`.`FileSize`, `File`.`FileName` FROM `File`, `Component` "
        L"WHERE `Component`.`Component` = `File`.`Component_`";
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const wchar_t* sql = attempt == 0 ? kSqlWithHash : kSqlPlain;
        MSIHANDLE view = 0;
        if (MsiDatabaseOpenViewW(database, sql, &view) != ERROR_SUCCESS)
            continue;
        const bool withHash = attempt == 0;
        bool any = false;
        if (MsiViewExecute(view, 0) == ERROR_SUCCESS)
        {
            MSIHANDLE record = 0;
            while (MsiViewFetch(view, &record) == ERROR_SUCCESS)
            {
                FileCheck entry;
                wchar_t buf[96]{};
                DWORD cch = _countof(buf);
                if (MsiRecordGetStringW(record, 1, buf, &cch) == ERROR_SUCCESS)
                    entry.componentId = buf;
                entry.size = static_cast<unsigned long long>(
                    static_cast<LONG>(MsiRecordGetInteger(record, 2)));
                wchar_t name[256]{};
                DWORD nameCch = _countof(name);
                if (MsiRecordGetStringW(record, 3, name, &nameCch) == ERROR_SUCCESS)
                    entry.fileName = LongFileName(name);
                if (withHash)
                {
                    entry.hasHash = true;
                    for (int i = 0; i < 4; ++i)
                        entry.hash[i] = static_cast<DWORD>(MsiRecordGetInteger(record, 4 + i));
                }
                if (!entry.componentId.empty())
                {
                    out.push_back(std::move(entry));
                    any = true;
                }
                MsiCloseHandle(record);
            }
        }
        MsiViewClose(view);
        MsiCloseHandle(view);
        if (any || !withHash)
            return any;
    }
    return false;
}

[[nodiscard]] bool HasPeExtension(const std::wstring& path) noexcept
{
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos)
        return false;
    std::wstring ext = path.substr(dot);
    for (auto& c : ext)
        c = static_cast<wchar_t>(towlower(c));
    return ext == L".exe" || ext == L".dll" || ext == L".sys";
}

// MSIINSTALLHASH и имена его полей в части SDK не объявлены рядом с MsiGetFileHashW.
// Тип структуры берём из сигнатуры самой функции, а значения хеша — последние 16 байт
// (dwHashOptions, dwLength, четыре DWORD — ровно колонки Hash1..Hash4 таблицы MsiFileHash).
template <class R, class A, class B, class C>
C ThirdParameter(R (WINAPI*)(A, B, C));
using MsiFileHashBuffer = std::remove_pointer_t<decltype(ThirdParameter(&MsiGetFileHashW))>;

// Кэш MSI, из которого продукт установлен (MsiGetProductInfo LocalPackage). Сверка
// файлов идёт именно с ним: новый пакет содержит новые сборки и не является
// эталоном целостности уже установленных файлов.
[[nodiscard]] std::wstring CachedMsiPath(const wchar_t* productCode) noexcept
{
    wchar_t path[MAX_PATH * 2]{};
    DWORD cch = _countof(path);
    if (MsiGetProductInfoW(productCode, L"LocalPackage", path, &cch) != ERROR_SUCCESS || !path[0])
        return {};
    return path;
}

[[nodiscard]] bool FileHashMatches(const std::wstring& path, const DWORD (&expected)[4]) noexcept
{
    MsiFileHashBuffer hash{};
    if (MsiGetFileHashW(path.c_str(), 0, &hash) != ERROR_SUCCESS)
        return true; // файл недоступен — не считаем повреждением (есть проверки размера/подписи)
    static_assert(sizeof(MsiFileHashBuffer) >= 16, "MSI hash buffer too small");
    const auto* bytes = reinterpret_cast<const BYTE*>(&hash);
    return memcmp(bytes + sizeof(MsiFileHashBuffer) - 16, expected, sizeof(expected)) == 0;
}

// Compare only Native's bytes against a verified OFFER, independently of the
// installed-baseline integrity check. Equal MSI ProductVersion is not equal DLL.
void CompareNativeWithOffer(const std::wstring& offer,const std::wstring& installed,
    bool& compared,bool& differs) {
    compared=false;differs=false;
    MSIHANDLE database=0;
    if(MsiOpenDatabaseW(offer.c_str(),MSIDBOPEN_READONLY,&database)!=ERROR_SUCCESS)return;
    DWORD expected[4]{};
    const auto authored=QueryMsiProperty(database,L"GLASSNATIVEMSIHASH");
    if(!authored.empty()) {
        bool valid=authored.size()==32;
        if(valid)for(size_t i=0;i<32;++i) {
            const wchar_t c=authored[i];
            const int digit=c>=L'0'&&c<=L'9'?c-L'0':c>=L'A'&&c<=L'F'?c-L'A'+10:c>=L'a'&&c<=L'f'?c-L'a'+10:-1;
            if(digit<0){valid=false;break;}
            expected[i/8]=(expected[i/8]<<4)|static_cast<DWORD>(digit);
        }
        MsiCloseHandle(database);
        if(!valid){GlassInstallLog::LineFormat(L"Native offer comparison: malformed authored hash: %s",offer.c_str());return;}
    } else {
        std::vector<FileCheck> files;
        const bool enumerated=EnumProductFiles(database,files);MsiCloseHandle(database);
        if(!enumerated){GlassInstallLog::LineFormat(L"Native offer comparison: no file metadata: %s",offer.c_str());return;}
        const FileCheck* native=nullptr;
        for(const auto& file:files)if(!_wcsicmp(file.fileName.c_str(),L"GlassSettings.Native.dll")) {
            if(native){GlassInstallLog::LineFormat(L"Native offer comparison: ambiguous Native metadata: %s",offer.c_str());return;}
            native=&file;
        }
        if(!native||!native->hasHash) {
            GlassInstallLog::LineFormat(L"Native offer comparison: no Native hash (versioned files may lack MsiFileHash); rebuild offered Settings MSI: %s",offer.c_str());return;
        }
        memcpy(expected,native->hash,sizeof(expected));
    }
    MsiFileHashBuffer hash{};hash.dwFileHashInfoSize=sizeof(hash);
    const UINT status=MsiGetFileHashW(installed.c_str(),0,&hash);
    if(status!=ERROR_SUCCESS) {
        GlassInstallLog::LineFormat(L"Native offer comparison: unreadable (%u): %s",status,installed.c_str());return;
    }
    const auto* actual=reinterpret_cast<const BYTE*>(&hash)+sizeof(hash)-16;
    compared=true;differs=memcmp(actual,expected,16)!=0;
    DWORD words[4]{};memcpy(words,actual,sizeof(words));
    GlassInstallLog::LineFormat(L"Native offer comparison: differs=%d installed=%s MSI=%s MsiHash128=%08lX-%08lX-%08lX-%08lX offered=%08lX-%08lX-%08lX-%08lX",
        differs?1:0,installed.c_str(),offer.c_str(),words[0],words[1],words[2],words[3],expected[0],expected[1],expected[2],expected[3]);
}

// Windows Installer strips cached signatures/cabinets. Trust its protected
// cache and exact registered identity, not the version of a replacement MSI.
bool InstalledBaselineMatches(const std::wstring& path,const wchar_t* code) noexcept {
    wchar_t windows[32768]{};const UINT length=GetWindowsDirectoryW(windows,_countof(windows));
    if(!length || length>=_countof(windows))return false;
    const auto parent=std::wstring(windows)+L"\\Installer";
    const auto slash=path.find_last_of(L"\\/");
    if(slash==std::wstring::npos || _wcsicmp(path.substr(0,slash).c_str(),parent.c_str()) ||
       !GlassMaintenanceCache::TrustedDirectory(parent) || !GlassMaintenanceCache::TrustedPath(path,false))return false;
    MSIHANDLE db=0,summary=0;
    if(MsiOpenDatabaseW(path.c_str(),MSIDBOPEN_READONLY,&db)!=ERROR_SUCCESS)return false;
    bool ok=_wcsicmp(QueryMsiProperty(db,L"ProductCode").c_str(),code)==0;
    wchar_t registered[256]{},package[256]{};DWORD n=_countof(registered),size=_countof(package);
    UINT type=0;INT number=0;FILETIME time{};
    ok=ok && MsiGetProductInfoW(code,INSTALLPROPERTY_PACKAGECODE,registered,&n)==ERROR_SUCCESS &&
        MsiGetSummaryInformationW(db,nullptr,0,&summary)==ERROR_SUCCESS;
    if(ok)ok=MsiSummaryInfoGetPropertyW(summary,9,&type,&number,&time,package,&size)==ERROR_SUCCESS &&
        registered[0] && _wcsicmp(registered,package)==0;
    if(summary)MsiCloseHandle(summary);MsiCloseHandle(db);return ok;
}

// Сверка файлов установленного продукта с таблицами File/MsiFileHash установленного
// MSI и с подписью издателя установщика (повреждение, подмена).
bool CheckProductFiles(
    const std::wstring& msiPath,
    const wchar_t* productCode,
    const std::wstring& signerSha1,
    ProductReport& rep,
    bool openGlassProduct) noexcept
{
    MSIHANDLE database = 0;
    if (MsiOpenDatabaseW(msiPath.c_str(), MSIDBOPEN_READONLY, &database) != ERROR_SUCCESS)
        return false;
    std::vector<FileCheck> files;
    if(openGlassProduct)rep.installedPayloadVersion=QueryFileVersion(database,L"OpenGlass.dll");
    const bool enumerated=EnumProductFiles(database, files);
    MsiCloseHandle(database);
    if(!enumerated)return false;

    for (const auto& file : files)
    {
        wchar_t path[MAX_PATH * 2]{};
        DWORD cch = _countof(path);
        const UINT state = MsiGetComponentPathW(productCode, file.componentId.c_str(), path, &cch);
        if (state != INSTALLSTATE_LOCAL || !path[0])
        {
            // Компонент должен быть установлен локально, но файл недоступен.
            // Хост службы OpenGlass вправе отсутствовать: старые версии запускаются
            // задачей без OpenGlassHost.exe.
            if (openGlassProduct && _wcsicmp(file.fileName.c_str(), L"OpenGlassHost.exe") == 0)
                continue;
            if (state == INSTALLSTATE_ABSENT || state == INSTALLSTATE_UNKNOWN)
                AddReason(rep, Reason::FileMissing);
            continue;
        }
        if (openGlassProduct && IsOpenGlassUserResource(path))
            continue; // материалы/атласы/отражения пользователь вправе удалять
        WIN32_FILE_ATTRIBUTE_DATA attrs{};
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attrs))
        {
            AddReason(rep, Reason::FileMissing);
            continue;
        }
        const unsigned long long actualSize =
            (static_cast<unsigned long long>(attrs.nFileSizeHigh) << 32) | attrs.nFileSizeLow;
        if (file.size != 0 && actualSize != file.size)
            AddReason(rep, Reason::FileSizeMismatch);
        if (file.hasHash && !FileHashMatches(path, file.hash))
            AddReason(rep, Reason::FileHashMismatch);
        if (HasPeExtension(path))
        {
            std::wstring signer;
            const DWORD trust = GetFileSignerSha1(path, signer);
            // Подпись принимается, если файл подписан издателем установщика (ours)
            // или цепочка доверена Windows (trusted) — перераспространяемые
            // библиотеки Microsoft и т. п. Подмена ловится контрольной суммой.
            const bool ours = !signerSha1.empty() && _wcsicmp(signer.c_str(), signerSha1.c_str()) == 0;
            if (!ours && trust != ERROR_SUCCESS)
                AddReason(rep, Reason::SignatureMismatch);
        }
    }
    return true;
}

} // namespace

std::wstring QuerySelfSignerSha1() noexcept
{
    return SelfSignerSha1();
}

bool FileHasSameSignerAsSetup(const std::wstring& path) noexcept
{
    const std::wstring self = SelfSignerSha1();
    if (self.empty() || path.empty())
        return false;
    std::wstring hex;
    GetFileSignerSha1(path, hex);
    return !hex.empty() && _wcsicmp(hex.c_str(), self.c_str()) == 0;
}

// Read an installed payload's fixed FileVersion without loading/executing it.
static std::wstring PayloadFileVersion(const std::wstring& path) {
    DWORD ignored=0;const DWORD bytes=GetFileVersionInfoSizeW(path.c_str(),&ignored);
    if(!bytes||bytes>1024*1024)return {};
    std::vector<BYTE> data(bytes);
    if(!GetFileVersionInfoW(path.c_str(),0,bytes,data.data()))return {};
    VS_FIXEDFILEINFO* info=nullptr;UINT size=0;
    if(!VerQueryValueW(data.data(),L"\\",reinterpret_cast<void**>(&info),&size)||
       size<sizeof(*info)||!info||info->dwSignature!=0xfeef04bd)return {};
    return std::to_wstring(HIWORD(info->dwFileVersionMS))+L"."+std::to_wstring(LOWORD(info->dwFileVersionMS))+L"."+
        std::to_wstring(HIWORD(info->dwFileVersionLS))+L"."+std::to_wstring(LOWORD(info->dwFileVersionLS));
}

static void ReadSettingsPayloadVersion(ProductReport& report,const std::wstring& root) {
    if(report.installed&&!root.empty())
        report.installedPayloadVersion=PayloadFileVersion(root+L"\\GlassSettings\\GlassSettings.exe");
}

Report Run(const Inputs& in)
{
    Report report;
    const std::wstring signerSha1 = in.signerSha1.empty() ? SelfSignerSha1() : in.signerSha1;

    struct ProductInputs
    {
        const wchar_t* modernCode;
        const wchar_t* legacyCode;
        ProductReport* report;
    };
    const ProductInputs products[] = {
        { GlassInstallMsi::kProductPlatformX64, GlassInstallMsi::kLegacyPlatformX64, &report.x64 },
        { GlassInstallMsi::kProductPlatformX86, GlassInstallMsi::kLegacyPlatformX86, &report.x86 },
        { GlassInstallMsi::kProductSettingsX64, nullptr, &report.settings64 },
        { GlassInstallMsi::kProductSettingsX86, nullptr, &report.settings32 },
        { GlassInstallMsi::kProductSecureUxThemeX64, nullptr, &report.secureUxTheme },
        { GlassInstallMsi::kProductSecureUxThemeX86, nullptr, &report.secureUxTheme },
        { GlassInstallMsi::kProductOpenGlass, GlassInstallMsi::kLegacyOpenGlass, &report.og },
    };

    for (const auto& product : products)
    {
        const bool modern = GlassInstallMsi::IsProductInstalled(product.modernCode);
        const bool legacy = product.legacyCode && GlassInstallMsi::IsProductInstalled(product.legacyCode);
        if (!modern && !legacy)
            continue;
        ProductReport& rep = *product.report;
        rep.installed = true;
        if (rep.installedVersion.empty())
            rep.installedVersion = GlassInstallMsi::QueryDisplayVersion(
                modern ? product.modernCode : product.legacyCode);
        rep.installedPackageVersion=rep.installedVersion;
    }

    report.settings=report.settings64.installed?report.settings64:report.settings32;
    const bool glassRepackedSut=report.secureUxTheme.installed;
    std::vector<std::wstring> officialSut;
    const UINT sutInventoryStatus=Shared::SecureUxThemeInventory::QueryInstalled(officialSut);
    if (sutInventoryStatus==ERROR_SUCCESS && !officialSut.empty()) {
        report.secureUxTheme.installed=true;
        if(officialSut.size()==1 && !glassRepackedSut)report.officialSecureUxThemeCode=officialSut.front();
        report.secureUxTheme.installedVersion=officialSut.size()==1 && !glassRepackedSut?
            GlassInstallMsi::QueryDisplayVersion(officialSut.front().c_str()):L"";
        report.secureUxTheme.installedPackageVersion=report.secureUxTheme.installedVersion;
    }

    // Evidence is available even on the all-products-absent early return.
    // Never infer installation from System32 payloads or native Inactive state.
    report.secureUxThemePresenceDetail=L"Installed MSI inventory rc="+std::to_wstring(sutInventoryStatus)+
        L"; installed="+(report.secureUxTheme.installed?std::wstring(L"true"):std::wstring(L"false"));
    for(const auto* code:{GlassInstallMsi::kProductSecureUxThemeX64,GlassInstallMsi::kProductSecureUxThemeX86})
        report.secureUxThemePresenceDetail+=L"; "+std::wstring(code)+L" state="+std::to_wstring(static_cast<int>(MsiQueryProductStateW(code)));
    std::vector<std::wstring> sutCandidates;
    const UINT candidatesStatus=Shared::SecureUxThemeInventory::Query(sutCandidates);
    report.secureUxThemePresenceDetail+=L"; family candidates rc="+std::to_wstring(candidatesStatus);
    for(const auto& code:sutCandidates)
        report.secureUxThemePresenceDetail+=L"; "+code+L" state="+std::to_wstring(static_cast<int>(MsiQueryProductStateW(code.c_str())));
    GlassInstallLog::LineFormat(L"SecureUxTheme presence: %s",report.secureUxThemePresenceDetail.c_str());

    // Probe physical backends BEFORE the all-MSI-absent return. Keep registration
    // and residual evidence separate: a residual has no installed MSI baseline.
    if (in.os64 && !report.og.installed) {
        bool serviceExists=false;DWORD startType=SERVICE_AUTO_START;
        const bool serviceKnown=GlassInstallHost::QueryOpenGlassServiceStartType(serviceExists,startType);
        const DWORD serviceError=serviceKnown?ERROR_SUCCESS:GetLastError();
        GlassInstallUtil::OpenGlassTaskState tasks;
        const bool tasksKnown=GlassInstallUtil::QueryOpenGlassTasks(tasks);
        report.og.residual=serviceExists || tasks.found;
        if (report.og.residual) {
            report.og.damaged=true;
            GlassInstallUtil::InstallRoots residualRoots{};std::wstring detail;
            report.og.residualSafe=serviceKnown && tasksKnown &&
                GlassInstallUtil::ResolveInstallRoots(residualRoots) &&
                GlassInstallRemoval::ValidateOpenGlassPaths(residualRoots.openGlass,in.cancelled,false,detail);
            GlassInstallLog::LineFormat(L"OpenGlass residual: service=%d tasks=%d serviceError=%lu safe=%d detail=%s",
                serviceExists?1:0,tasks.found?1:0,serviceError,report.og.residualSafe?1:0,detail.c_str());
        }
    }

    GlassInstallUtil::InstallRoots roots{};
    const bool rootsOk = GlassInstallUtil::ResolveInstallRoots(roots);
    std::atomic_bool neverCancel{false};
    const auto& cancel=in.cancelled?*in.cancelled:neverCancel;
    GlassPackageCatalog::Catalog catalog;
    std::wstring diagnostic;
    if(in.inspectSources)GlassPackageAcquisition::FetchCatalog(cancel,catalog,diagnostic);
    const struct { GlassPackageCatalog::Product product; ProductReport* rep; const wchar_t* code; const wchar_t* legacy; } jobs[]={
        {GlassPackageCatalog::Product::Platform64,&report.x64,GlassInstallMsi::kProductPlatformX64,GlassInstallMsi::kLegacyPlatformX64},
        {GlassPackageCatalog::Product::Platform32,&report.x86,GlassInstallMsi::kProductPlatformX86,GlassInstallMsi::kLegacyPlatformX86},
        {GlassPackageCatalog::Product::Settings64,&report.settings64,GlassInstallMsi::kProductSettingsX64,nullptr},
        {GlassPackageCatalog::Product::Settings32,&report.settings32,GlassInstallMsi::kProductSettingsX86,nullptr},
        {GlassPackageCatalog::Product::OpenGlass,&report.og,GlassInstallMsi::kProductOpenGlass,GlassInstallMsi::kLegacyOpenGlass}};
    for(const auto& job:jobs) {
        if(cancel.load())break;
        if(job.product==GlassPackageCatalog::Product::OpenGlass)continue; // Upstream file product, never adjacent/cached Glass MSI.
        std::wstring offerPath;
        if(in.inspectSources && GlassPackageAcquisition::Describe(job.product,catalog,cancel,
            job.rep->targetVersion,job.rep->sourceVerified,&offerPath,
            &job.rep->attachedPackageVersion,&job.rep->attachedPackagePath)) {
            GlassPackageRules::Version installed,target;
            const auto numeric=[](const std::wstring& text,GlassPackageRules::Version& out) {
                for(wchar_t c:text)if(static_cast<unsigned>(c)>127)return false;
                return GlassPackageRules::ParseVersion(std::string(text.begin(),text.end()),out);
            };
            job.rep->updateAvailable=job.rep->installed &&
                numeric(job.rep->installedPackageVersion,installed) && numeric(job.rep->targetVersion,target) &&
                GlassPackageRules::Compare(installed,target)<0;
        }
        job.rep->selectedPackagePath=offerPath;
        const bool payload=job.product==GlassPackageCatalog::Product::OpenGlass||job.product==GlassPackageCatalog::Product::Settings64||job.product==GlassPackageCatalog::Product::Settings32;
        const wchar_t* payloadFile=job.product==GlassPackageCatalog::Product::OpenGlass?L"OpenGlass.dll":L"GlassSettings.exe";
        if(payload&&!job.rep->attachedPackagePath.empty()) {
            MSIHANDLE db=0;
            if(MsiOpenDatabaseW(job.rep->attachedPackagePath.c_str(),MSIDBOPEN_READONLY,&db)==ERROR_SUCCESS) {
                job.rep->attachedPayloadVersion=QueryFileVersion(db,payloadFile);MsiCloseHandle(db);
            }
        }
        if(payload&&!offerPath.empty()) {
            MSIHANDLE db=0;
            if(MsiOpenDatabaseW(offerPath.c_str(),MSIDBOPEN_READONLY,&db)==ERROR_SUCCESS) {
                job.rep->targetPayloadVersion=QueryFileVersion(db,payloadFile);MsiCloseHandle(db);
            }
        }
        if(job.rep->installed&&job.rep->sourceVerified&&!offerPath.empty()&&rootsOk&&
            (job.product==GlassPackageCatalog::Product::Settings64||job.product==GlassPackageCatalog::Product::Settings32)) {
            const auto& root=job.product==GlassPackageCatalog::Product::Settings64?roots.x64:roots.x86;
            if(!root.empty())CompareNativeWithOffer(offerPath,root+L"\\GlassSettings\\GlassSettings.Native.dll",
                job.rep->nativeSourceCompared,job.rep->nativeSourceDiffers);
            GlassInstallLog::LineFormat(L"Native dependency freshness: product=%s compared=%d differs=%d installedMSI=%s offeredMSI=%s",
                job.code,job.rep->nativeSourceCompared?1:0,job.rep->nativeSourceDiffers?1:0,
                job.rep->installedPackageVersion.c_str(),job.rep->targetVersion.c_str());
        }
        if(!job.rep->installed)continue;
        // Integrity always uses the installed package, never a same-version
        // replacement guessed to have identical bytes/PackageCode.
        const auto* installedCode=GlassInstallMsi::IsProductInstalled(job.code)?job.code:
            job.legacy && GlassInstallMsi::IsProductInstalled(job.legacy)?job.legacy:job.code;
        const auto reference=CachedMsiPath(installedCode);
        std::wstring contextDetail;
        if(!reference.empty() && InstalledBaselineMatches(reference,installedCode) && GlassInstallMsi::CheckProductContexts(installedCode,contextDetail)==GlassInstallMsi::ProductPathCheck::Safe)
            job.rep->integrityKnown=CheckProductFiles(reference,installedCode,signerSha1,*job.rep,job.product==GlassPackageCatalog::Product::OpenGlass);
        if(!job.rep->integrityKnown)AddReason(*job.rep,Reason::InspectionUnavailable);
    }
    if(report.og.installed&&rootsOk&&!roots.openGlass.empty())
        report.og.installedPayloadVersion=PayloadFileVersion(roots.openGlass+L"\\OpenGlass.dll");
    // The application FileVersion is a display fact, not MSI version authority.
    if(rootsOk) {
        ReadSettingsPayloadVersion(report.settings64,roots.x64);
        ReadSettingsPayloadVersion(report.settings32,roots.x86);
    }
    if(rootsOk&&!roots.openGlass.empty()) {
        try {
            const auto files=GlassOpenGlassFiles::Inspect(roots.openGlass);
            if(files.present) {
                report.og.installed=true;report.og.residual=false;report.og.residualSafe=false;
                report.og.integrityKnown=true;report.og.damaged=!files.integrity;
                report.og.installedVersion.clear();report.og.installedPackageVersion.clear();
                report.og.installedPayloadVersion=files.version;report.og.reasons.clear();
                if(!files.integrity)AddReason(report.og,Reason::FileHashMismatch);
            }
        }catch(...) {
            if(GlassOpenGlassFiles::HasReceipt(roots.openGlass)) {
                report.og.installed=true;report.og.damaged=true;report.og.integrityKnown=false;
                AddReason(report.og,Reason::InspectionUnavailable);
            }
        }
    }
    // Package releases may update installer plumbing without changing OpenGlass.
    // Keep MSI version authority for acquisition/execution, not the product badge.
    report.og.updateAvailable=report.og.installed&&report.og.sourceVerified&&GlassOpenGlassRelease::PayloadUpdateAvailable(
        report.og.installedPayloadVersion,report.og.targetPayloadVersion);
    if(in.inspectSources&&!cancel.load()) {
        std::string text,version;std::wstring error;
        if(GlassReleaseClient::Get(L"https://api.github.com/repos/ALTaleX531/OpenGlass/releases/latest",INVALID_HANDLE_VALUE,&text,1024*1024,cancel,error)&&
           GlassOpenGlassRelease::Parse(text,version)) {
            report.og.remotePayloadVersion=GlassReleaseClient::Wide(version);
            // One metadata snapshot for inspection and subsequent Next; this
            // caches no payload and never turns metadata into install authority.
            {std::lock_guard<std::mutex> lock(GlassSetupSourceLayout::openGlassCatalogMutex);
             GlassSetupSourceLayout::openGlassCatalogText=text;}
            GlassInstallLog::LineFormat(L"OpenGlass upstream release: %s; local DLL: %s",report.og.remotePayloadVersion.c_str(),report.og.installedPayloadVersion.c_str());
        }else GlassInstallLog::LineFormat(L"OpenGlass upstream release unavailable: %s",error.c_str());
    }
    report.settings=report.settings64.installed?report.settings64:report.settings32;
    report.settings.damaged=report.settings64.damaged||report.settings32.damaged;
    report.settings.updateAvailable=report.settings64.updateAvailable||report.settings32.updateAvailable;

    if (in.cancelled && in.cancelled->load(std::memory_order_acquire))
        return report;
    // Параметры реестра установки: корень каждого продукта должен разрешаться.
    const auto rootInvalid = [](const std::wstring& root) noexcept {
        if (root.empty())
            return true;
        const DWORD attrs = GetFileAttributesW(root.c_str());
        return attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
    };
    if (report.x64.installed && (!rootsOk || rootInvalid(roots.x64)))
        AddReason(report.x64, Reason::RegistryInvalid);
    if (report.x86.installed && (!rootsOk || rootInvalid(roots.x86)))
        AddReason(report.x86, Reason::RegistryInvalid);
    if (report.settings.installed && (!rootsOk || rootInvalid(in.os64 ? roots.x64 : roots.x86)))
        AddReason(report.settings, Reason::RegistryInvalid);
    if (glassRepackedSut && (!rootsOk || rootInvalid(in.os64 ? roots.x64 : roots.x86)))
        AddReason(report.secureUxTheme, Reason::RegistryInvalid);
    if (report.og.installed && (!rootsOk || rootInvalid(roots.openGlass)))
        AddReason(report.og, Reason::RegistryInvalid);

    // Служба/задачи автозапуска.
    if (report.x64.installed &&
        !GlassShared::GlassPlatformTask::GlassAutorunTaskExists(GlassShared::GlassPlatformTask::PlatformArch::X64))
        AddReason(report.x64, Reason::AutostartMissing);
    if (report.x86.installed &&
        !GlassShared::GlassPlatformTask::GlassAutorunTaskExists(GlassShared::GlassPlatformTask::PlatformArch::X86))
        AddReason(report.x86, Reason::AutostartMissing);
    if (report.og.installed)
    {
        GlassInstallUtil::OpenGlassTaskState tasks{};
        bool serviceExists = false;
        DWORD startType = 0;
        const bool taskKnown = GlassInstallUtil::QueryOpenGlassTasks(tasks);
        const bool serviceKnown = GlassInstallHost::QueryOpenGlassServiceStartType(serviceExists, startType);
        if ((taskKnown && !tasks.found) && (serviceKnown && !serviceExists))
            AddReason(report.og, Reason::AutostartMissing);
        else if (!taskKnown && !serviceKnown)
            AddReason(report.og, Reason::AutostartMissing);
    }

    // Известные параметры реестра платформы (GlassPlatform и DWM-значения OpenGlass):
    // некорректные значения — признак повреждения; чинит удаление при установке.
    const auto sanity = Shared::RegistrySanity::Validate(false);
    if (sanity.glassIssues)
    {
        AddReason(report.settings, Reason::RegistryInvalid);
        report.settings.registryValueNames = sanity.glassValueNames;
        for(auto* settings:{&report.settings64,&report.settings32})if(settings->installed) {
            AddReason(*settings,Reason::RegistryInvalid);
            settings->registryValueNames=sanity.glassValueNames;
        }
    }
    if (sanity.openGlassIssues)
    {
        AddReason(report.og, Reason::RegistryInvalid);
        report.og.registryValueNames = sanity.openGlassValueNames;
    }

    if(in.inspectSources&&!cancel.load()) {
        std::wstring sutDiagnostic;
        GlassSecureUxPackage::Describe(cancel,report.secureUxOffer,sutDiagnostic);
        if(!sutDiagnostic.empty())GlassInstallLog::LineFormat(L"SecureUxTheme offer discovery: %s",sutDiagnostic.c_str());
    }
    report.ran = true;
    return report;
}

} // namespace GlassProductInspect
