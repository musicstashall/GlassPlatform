#include "GlassProductInspect.h"

#include "GlassInstallHost.h"
#include "GlassInstallMsi.h"
#include "GlassInstallUtil.h"

#include <GlassPlatformTask.h>
#include <RegistrySanity.h>

#include <msi.h>
#include <msiquery.h>
#include <softpub.h>
#include <wintrust.h>

#include <array>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <vector>

#pragma comment(lib, "msi.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace GlassProductInspect {
namespace {

struct QuadVersion
{
    unsigned v[4]{};

    static QuadVersion Parse(const std::wstring& text) noexcept
    {
        QuadVersion out;
        size_t start = 0;
        for (int i = 0; i < 4 && start < text.size(); ++i)
        {
            const size_t end = text.find(L'.', start);
            const std::wstring part = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            out.v[i] = static_cast<unsigned>(_wtoi(part.c_str()));
            if (end == std::wstring::npos)
                break;
            start = end + 1;
        }
        return out;
    }

    [[nodiscard]] int Compare(const QuadVersion& other) const noexcept
    {
        for (int i = 0; i < 4; ++i)
        {
            if (v[i] != other.v[i])
                return v[i] < other.v[i] ? -1 : 1;
        }
        return 0;
    }

    [[nodiscard]] bool IsZero() const noexcept
    {
        return v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 0;
    }
};

void AddReason(ProductReport& rep, Reason reason) noexcept
{
    for (Reason existing : rep.reasons)
        if (existing == reason)
            return;
    rep.reasons.push_back(reason);
    if (reason != Reason::VersionOutdated)
        rep.damaged = true;
}

// SHA-1 отпечаток подписи файла (тот же приём, что в QuerySelfSignerInfo Setup).
bool GetFileSignerSha1(const std::wstring& path, std::wstring& hexOut) noexcept
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
    if (status == ERROR_SUCCESS)
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
    return !hexOut.empty();
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
    unsigned long long size = 0;
    DWORD hash[4]{};
    bool hasHash = false;
};

bool EnumProductFiles(MSIHANDLE database, std::vector<FileCheck>& out) noexcept
{
    out.clear();
    const wchar_t* kSqlWithHash =
        L"SELECT `Component`.`ComponentId`, `File`.`FileSize`, `MsiFileHash`.`Hash1`, `MsiFileHash`.`Hash2`, "
        L"`MsiFileHash`.`Hash3`, `MsiFileHash`.`Hash4` FROM `File`, `Component`, `MsiFileHash` "
        L"WHERE `Component`.`Component` = `File`.`Component_` AND `MsiFileHash`.`File_` = `File`.`File`";
    const wchar_t* kSqlPlain =
        L"SELECT `Component`.`ComponentId`, `File`.`FileSize` FROM `File`, `Component` "
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
                if (withHash)
                {
                    entry.hasHash = true;
                    for (int i = 0; i < 4; ++i)
                        entry.hash[i] = static_cast<DWORD>(MsiRecordGetInteger(record, 3 + i));
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

// Сверка файлов установленного продукта с таблицами File/MsiFileHash встроенного
// MSI и с подписью издателя установщика (повреждение, подмена).
void CheckProductFiles(
    const std::wstring& msiPath,
    const wchar_t* productCode,
    const std::wstring& signerSha1,
    ProductReport& rep) noexcept
{
    MSIHANDLE database = 0;
    if (MsiOpenDatabaseW(msiPath.c_str(), MSIDBOPEN_READONLY, &database) != ERROR_SUCCESS)
        return;
    std::vector<FileCheck> files;
    EnumProductFiles(database, files);
    const std::wstring packageVersion = QueryMsiProperty(database, L"ProductVersion");
    if (!packageVersion.empty() && !rep.installedVersion.empty())
    {
        if (QuadVersion::Parse(rep.installedVersion).Compare(QuadVersion::Parse(packageVersion)) < 0)
        {
            rep.updateAvailable = true;
            AddReason(rep, Reason::VersionOutdated);
        }
    }
    MsiCloseHandle(database);

    for (const auto& file : files)
    {
        wchar_t path[MAX_PATH * 2]{};
        DWORD cch = _countof(path);
        const UINT state = MsiGetComponentPathW(productCode, file.componentId.c_str(), path, &cch);
        if (state != INSTALLSTATE_LOCAL || !path[0])
        {
            // Компонент должен быть установлен локально, но файл недоступен.
            if (state == INSTALLSTATE_ABSENT || state == INSTALLSTATE_UNKNOWN)
                AddReason(rep, Reason::FileMissing);
            continue;
        }
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
        if (file.hasHash)
        {
            MSIINSTALLHASH hash{};
            if (MsiGetFileHashW(path, 0, &hash) != ERROR_SUCCESS ||
                hash.dw1 != file.hash[0] || hash.dw2 != file.hash[1] ||
                hash.dw3 != file.hash[2] || hash.dw4 != file.hash[3])
                AddReason(rep, Reason::FileHashMismatch);
        }
        if (!signerSha1.empty() && HasPeExtension(path))
        {
            std::wstring signer;
            if (!GetFileSignerSha1(path, signer) || _wcsicmp(signer.c_str(), signerSha1.c_str()) != 0)
                AddReason(rep, Reason::SignatureMismatch);
        }
    }
}

} // namespace

Report Run(const Inputs& in)
{
    Report report;
    const std::wstring signerSha1 = in.signerSha1.empty() ? SelfSignerSha1() : in.signerSha1;

    struct ProductInputs
    {
        const wchar_t* modernCode;
        const wchar_t* legacyCode;
        UINT msiResource;
        ProductReport* report;
    };
    const ProductInputs products[] = {
        { GlassInstallMsi::kProductPlatformX64, GlassInstallMsi::kLegacyPlatformX64, in.msiResX64, &report.x64 },
        { GlassInstallMsi::kProductPlatformX86, GlassInstallMsi::kLegacyPlatformX86, in.msiResX86, &report.x86 },
        { GlassInstallMsi::kProductSettingsX64, nullptr, in.msiResSettings, &report.settings },
        { GlassInstallMsi::kProductSettingsX86, nullptr, in.msiResSettings, &report.settings },
        { GlassInstallMsi::kProductOpenGlass, GlassInstallMsi::kLegacyOpenGlass, in.msiResOg, &report.og },
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
    }

    if (!report.x64.installed && !report.x86.installed && !report.settings.installed && !report.og.installed)
    {
        report.ran = true;
        return report;
    }

    std::wstring tempDir;
    if (GlassInstallUtil::CreateSecureTempDirectory(L"GlassInspect", tempDir))
    {
        const struct { UINT res; ProductReport* rep; const wchar_t* code; } fileJobs[] = {
            { in.msiResX64, &report.x64, GlassInstallMsi::kProductPlatformX64 },
            { in.msiResX86, &report.x86, GlassInstallMsi::kProductPlatformX86 },
            { in.msiResSettings, &report.settings,
                in.os64 ? GlassInstallMsi::kProductSettingsX64 : GlassInstallMsi::kProductSettingsX86 },
            { in.msiResOg, &report.og, GlassInstallMsi::kProductOpenGlass },
        };
        int index = 0;
        for (const auto& job : fileJobs)
        {
            if (!job.rep->installed || job.res == 0)
                continue;
            // Файловые проверки только для современных кодов продуктов: состав
            // легаси-MSI не соответствует текущему пакету.
            if (!GlassInstallMsi::IsProductInstalled(job.code))
                continue;
            std::wstring msiPath = tempDir + L"\\inspect-" + std::to_wstring(index++) + L".msi";
            if (!GlassInstallUtil::ExtractResourceToFile(in.instance, job.res, RT_RCDATA, msiPath, nullptr))
                continue;
            CheckProductFiles(msiPath, job.code, signerSha1, *job.rep);
            DeleteFileW(msiPath.c_str());
        }
        RemoveDirectoryW(tempDir.c_str());
    }

    // Параметры реестра установки: корень каждого продукта должен разрешаться.
    GlassInstallUtil::InstallRoots roots{};
    const bool rootsOk = GlassInstallUtil::ResolveInstallRoots(roots);
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
        AddReason(report.settings, Reason::RegistryInvalid);
    if (sanity.openGlassIssues)
        AddReason(report.og, Reason::RegistryInvalid);

    report.ran = true;
    return report;
}

} // namespace GlassProductInspect
