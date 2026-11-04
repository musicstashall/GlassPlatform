#ifndef GLASS_SETTINGS_SHELL_SPARSE_PACKAGE_CPP
#define GLASS_SETTINGS_SHELL_SPARSE_PACKAGE_CPP

#include <windows.h>
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.Storage.h>

#include <GlassModulePath.h>
#include <GlassShellMenuConstants.h>
#include <DebugLog.h>

#include "ShellSparsePackage.h"

#include <Shlwapi.h>
#include <ShlObj.h>
#include <roapi.h>
#include <wil/resource.h>

#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>

#include <string>
#include <vector>
#include <atomic>

#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

namespace winmd = winrt::Windows::Management::Deployment;
namespace winrtf = winrt::Windows::Foundation;

namespace GlassSettings::ShellSparsePackage
{
    namespace
    {
        std::atomic<int> g_lastSparsePackageError{ static_cast<int>(SparsePackageError::None) };
        std::atomic<HRESULT> g_lastDeploymentHr{ S_OK };

        constexpr HRESULT kHrPackageExternalLocationConflict = static_cast<HRESULT>(0x80073D0B);
        constexpr HRESULT kHrCertUntrustedRoot = static_cast<HRESULT>(0x800B0109);      // CERT_E_UNTRUSTEDROOT
        constexpr HRESULT kHrCertChainNotTrusted = static_cast<HRESULT>(0x800B010A);    // CERT_E_CHAINING
        constexpr HRESULT kHrSubjectNotTrusted = static_cast<HRESULT>(0x800B0004);      // TRUST_E_SUBJECT_NOT_TRUSTED
        constexpr HRESULT kHrNoSignature = static_cast<HRESULT>(0x800B0100);            // TRUST_E_NOSIGNATURE

        [[nodiscard]] bool IsSignerTrustHr(HRESULT hr) noexcept
        {
            return hr == kHrCertUntrustedRoot || hr == kHrCertChainNotTrusted || hr == kHrSubjectNotTrusted;
        }

        [[nodiscard]] SparsePackageError ClassifyDeploymentFailure(HRESULT hr) noexcept
        {
            if (hr == kHrPackageExternalLocationConflict)
                return SparsePackageError::ExternalLocationConflict;
            if (IsSignerTrustHr(hr))
                return SparsePackageError::SignerNotTrusted;
            return SparsePackageError::DeploymentFailed;
        }

        void SetLastSparsePackageError(SparsePackageError error) noexcept
        {
            g_lastSparsePackageError.store(static_cast<int>(error), std::memory_order_release);
        }

        [[nodiscard]] bool CanonicalizeOutputDirectory(wchar_t* dir, size_t cch) noexcept
        {
            if (!dir || cch == 0 || dir[0] == L'\0')
                return false;

            wchar_t full[MAX_PATH]{};
            const DWORD copied = ::GetFullPathNameW(dir, _countof(full), full, nullptr);
            if (copied == 0 || copied >= _countof(full))
                return false;

            size_t len = wcslen(full);
            if (len > 0 && full[len - 1] != L'\\' && len + 1 < _countof(full))
            {
                full[len] = L'\\';
                full[len + 1] = L'\0';
            }

            return wcscpy_s(dir, cch, full) == 0;
        }
        struct WinRtApartmentScope
        {
            WinRtApartmentScope() noexcept
                : m_hr(::RoInitialize(RO_INIT_MULTITHREADED))
            {
                if (SUCCEEDED(m_hr))
                    m_owned = true;
            }

            ~WinRtApartmentScope() noexcept
            {
                // Each successful RoInitialize, including S_FALSE, must be balanced.
                // RPC_E_CHANGED_MODE is not a successful initialization.
                if (m_owned)
                    ::RoUninitialize();
            }

            [[nodiscard]] bool CanUsePackageManager() const noexcept
            {
                return m_hr == S_OK || m_hr == S_FALSE;
            }

        private:
            HRESULT m_hr = E_FAIL;
            bool m_owned = false;
        };

        [[nodiscard]] bool FindSparsePackageByPrefixInRepoKey(const wchar_t* subkey) noexcept
        {
            if (!subkey || !*subkey)
                return false;

            HKEY hKey = nullptr;
            if (::RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
                return false;

            const std::wstring prefix = std::wstring(GlassShared::GlassShellMenu::kSparsePackageName) + L"_";
            bool found = false;
            for (DWORD index = 0;; ++index)
            {
                wchar_t name[256]{};
                DWORD cch = static_cast<DWORD>(_countof(name));
                const LONG e = ::RegEnumKeyExW(hKey, index, name, &cch, nullptr, nullptr, nullptr, nullptr);
                if (e == ERROR_NO_MORE_ITEMS)
                    break;
                if (e != ERROR_SUCCESS)
                    continue;
                if (_wcsnicmp(name, prefix.c_str(), prefix.size()) == 0)
                {
                    found = true;
                    break;
                }
            }

            ::RegCloseKey(hKey);
            return found;
        }

        [[nodiscard]] bool IsSparsePackagePresentInRepositoryRegistry() noexcept
        {
            static constexpr wchar_t kPackagesKey[] =
                L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages";
            static constexpr wchar_t kFamiliesKey[] =
                L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Families";
            return FindSparsePackageByPrefixInRepoKey(kPackagesKey) ||
                   FindSparsePackageByPrefixInRepoKey(kFamiliesKey);
        }

        [[nodiscard]] std::vector<std::wstring> FindSparsePackageFullNamesInRepositoryRegistry() noexcept
        {
            static constexpr wchar_t kPackagesKey[] =
                L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages";

            std::vector<std::wstring> names;
            HKEY hKey = nullptr;
            if (::RegOpenKeyExW(HKEY_CURRENT_USER, kPackagesKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
                return names;

            const std::wstring prefix = std::wstring(GlassShared::GlassShellMenu::kSparsePackageName) + L"_";
            for (DWORD index = 0;; ++index)
            {
                wchar_t name[256]{};
                DWORD cch = static_cast<DWORD>(_countof(name));
                const LONG e = ::RegEnumKeyExW(hKey, index, name, &cch, nullptr, nullptr, nullptr, nullptr);
                if (e == ERROR_NO_MORE_ITEMS)
                    break;
                if (e != ERROR_SUCCESS)
                    continue;
                if (_wcsnicmp(name, prefix.c_str(), prefix.size()) == 0)
                    names.emplace_back(name);
            }

            ::RegCloseKey(hKey);
            return names;
        }

        void AppendUniquePackageName(std::vector<std::wstring>& names, const std::wstring& candidate)
        {
            if (candidate.empty())
                return;
            for (const auto& existing : names)
            {
                if (_wcsicmp(existing.c_str(), candidate.c_str()) == 0)
                    return;
            }
            names.push_back(candidate);
        }

        void AppendUniquePackageName(std::vector<std::wstring>& names, const winrt::hstring& candidate)
        {
            if (candidate.empty())
                return;
            AppendUniquePackageName(names, std::wstring(candidate.c_str()));
        }

        [[nodiscard]] bool ResolveExeOutputDirectory(wchar_t* out, size_t cch) noexcept
        {
            if (!out || cch == 0)
                return false;
            out[0] = L'\0';
            wchar_t exe[MAX_PATH]{};
            if (::GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0)
                return false;
            wchar_t* sl = wcsrchr(exe, L'\\');
            if (!sl)
                return false;
            *(sl + 1) = L'\0';
            return CanonicalizeOutputDirectory(exe, cch) && wcscpy_s(out, cch, exe) == 0;
        }

        [[nodiscard]] bool DirectoryHasSparseArtifacts(const wchar_t* platformDir) noexcept
        {
            if (!platformDir || !*platformDir)
                return false;

            wchar_t probe[MAX_PATH]{};
            if (wcscpy_s(probe, platformDir) != 0)
                return false;
            if (wcscat_s(probe, _countof(probe), GlassShared::GlassShellMenu::kSparsePackageSubdir) != 0)
                return false;
            if (wcscat_s(probe, _countof(probe), L"AppxManifest.xml") != 0)
                return false;
            return ::GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES;
        }

        [[nodiscard]] bool TryCanonicalizePlatformDir(const wchar_t* dir, wchar_t* out, size_t cch) noexcept
        {
            if (!dir || !*dir || !out || cch == 0)
                return false;

            wchar_t buf[MAX_PATH]{};
            if (wcscpy_s(buf, dir) != 0)
                return false;
            if (!CanonicalizeOutputDirectory(buf, _countof(buf)))
                return false;
            if (!DirectoryHasSparseArtifacts(buf))
                return false;
            return wcscpy_s(out, cch, buf) == 0;
        }


        [[nodiscard]] std::wstring TryReadRegistryInstallDir() noexcept
        {
            auto readVal = [](HKEY hive, const wchar_t* valName, REGSAM sam) noexcept -> std::wstring {
                HKEY key = nullptr;
                if (::RegOpenKeyExW(hive, L"Software\\GlassPlatform", 0, KEY_READ | sam, &key) != ERROR_SUCCESS)
                    return {};
                wchar_t buf[MAX_PATH]{};
                DWORD cb = sizeof(buf);
                DWORD type = 0;
                const LSTATUS st = ::RegQueryValueExW(key, valName, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &cb);
                ::RegCloseKey(key);
                if (st != ERROR_SUCCESS || type != REG_SZ || !buf[0])
                    return {};
                return buf;
            };
#if defined(_WIN64)
            if (auto v = readVal(HKEY_LOCAL_MACHINE, L"InstallPathX64", KEY_WOW64_64KEY); !v.empty())
                return v;
            if (auto v = readVal(HKEY_LOCAL_MACHINE, L"InstallPath", KEY_WOW64_64KEY); !v.empty())
                return v;
            if (auto v = readVal(HKEY_CURRENT_USER, L"InstallPathX64", 0); !v.empty())
                return v;
#else
            if (auto v = readVal(HKEY_LOCAL_MACHINE, L"InstallPathX86", KEY_WOW64_32KEY); !v.empty())
                return v;
            if (auto v = readVal(HKEY_LOCAL_MACHINE, L"InstallPath", KEY_WOW64_32KEY); !v.empty())
                return v;
            if (auto v = readVal(HKEY_CURRENT_USER, L"InstallPathX86", 0); !v.empty())
                return v;
#endif
            return readVal(HKEY_CURRENT_USER, L"InstallPath", 0);
        }

        /// Корень платформы (каталог с GlassCompat.dll и подкаталогом GlassShellSparse\). ExternalLocation
        /// sparse-пакета — его подкаталог GlassShellSparse\ (ResolveSparsePackageDirectory), где лежат
        /// GlassShellExt.dll, GlassShellHost.exe, манифесты и msix.
        [[nodiscard]] bool ResolveGlassPlatformOutputDirectory(
            wchar_t* out, size_t cch, const wchar_t* exactPlatformRoot) noexcept
        {
            if (!out || cch == 0)
                return false;
            out[0] = L'\0';

            // Setup/Unins already resolved InstallPath. Never search the
            // rundll32 directory, temp parent or another registry copy when
            // an exact root was supplied, even if that root is unavailable.
            if (exactPlatformRoot)
                return exactPlatformRoot[0] && TryCanonicalizePlatformDir(exactPlatformRoot, out, cch);

            // GlassShellExt.dll лежит в <root>\GlassShellSparse\ (kShellExtRelativePath); корень платформы —
            // родитель этого подкаталога.
            std::wstring shellExt = GlassShared::GlassModulePath::ResolveGlassModulePathNextToModule(
                nullptr, GlassShared::GlassShellMenu::kShellExtRelativePath);
            if (shellExt.empty())
            {
                shellExt = GlassShared::GlassModulePath::ResolveGlassModulePathNextToModule(
                    reinterpret_cast<HMODULE>(&__ImageBase), GlassShared::GlassShellMenu::kShellExtRelativePath);
            }
            if (!shellExt.empty())
            {
                std::wstring root = GlassShared::GlassModulePath::GetInstallRootFromPath(shellExt.c_str());
                if (const size_t slash = root.find_last_of(L'\\'); slash != std::wstring::npos && slash > 0)
                    root.resize(slash);
                else
                    root.clear();
                if (!root.empty())
                {
                    wchar_t buf[MAX_PATH]{};
                    if (wcscpy_s(buf, root.c_str()) == 0 && TryCanonicalizePlatformDir(buf, out, cch))
                        return true;
                }
            }

            if (const std::wstring regRoot = TryReadRegistryInstallDir(); !regRoot.empty())
            {
                wchar_t regBuf[MAX_PATH]{};
                if (wcscpy_s(regBuf, regRoot.c_str()) == 0 && TryCanonicalizePlatformDir(regBuf, out, cch))
                    return true;
            }

            wchar_t exeDir[MAX_PATH]{};
            if (ResolveExeOutputDirectory(exeDir, _countof(exeDir)) && TryCanonicalizePlatformDir(exeDir, out, cch))
                return true;

            size_t len = wcslen(exeDir);
            if (len > 0 && exeDir[len - 1] == L'\\')
                exeDir[len - 1] = L'\0';
            wchar_t* slash = wcsrchr(exeDir, L'\\');
            if (slash)
            {
                *slash = L'\0';
                if (TryCanonicalizePlatformDir(exeDir, out, cch))
                    return true;
            }

            return false;
        }

        [[nodiscard]] bool AppendSparseSubdir(wchar_t* dir, size_t cch) noexcept
        {
            return wcscat_s(dir, cch, GlassShared::GlassShellMenu::kSparsePackageSubdir) == 0;
        }

        [[nodiscard]] bool ResolveSparseManifestPath(wchar_t* out, size_t cch, const wchar_t* exactPlatformRoot) noexcept
        {
            wchar_t dir[MAX_PATH]{};
            if (!ResolveGlassPlatformOutputDirectory(dir, _countof(dir), exactPlatformRoot) || !AppendSparseSubdir(dir, _countof(dir)))
                return false;
            if (wcscat_s(dir, _countof(dir), L"AppxManifest.xml") != 0)
                return false;
            return wcscpy_s(out, cch, dir) == 0;
        }

        [[nodiscard]] bool ResolveSparseUnregisterStubManifestPath(wchar_t* out, size_t cch, const wchar_t* exactPlatformRoot) noexcept
        {
            wchar_t dir[MAX_PATH]{};
            if (!ResolveGlassPlatformOutputDirectory(dir, _countof(dir), exactPlatformRoot) || !AppendSparseSubdir(dir, _countof(dir)))
                return false;
            if (wcscat_s(dir, _countof(dir), GlassShared::GlassShellMenu::kSparseUnregisterStubFileName) != 0)
                return false;
            return wcscpy_s(out, cch, dir) == 0;
        }

        [[nodiscard]] bool ResolveSparsePackageDirectory(wchar_t* out, size_t cch, const wchar_t* exactPlatformRoot) noexcept
        {
            wchar_t dir[MAX_PATH]{};
            if (!ResolveGlassPlatformOutputDirectory(dir, _countof(dir), exactPlatformRoot) || !AppendSparseSubdir(dir, _countof(dir)))
                return false;
            return wcscpy_s(out, cch, dir) == 0;
        }

        /// <root>\GlassShellSparse\GlassShellSparse.msix (PostBuild BuildSparsePackage.ps1 + GlassSign.targets).
        [[nodiscard]] bool ResolveSparseMsixPath(wchar_t* out, size_t cch, const wchar_t* exactPlatformRoot) noexcept
        {
            wchar_t dir[MAX_PATH]{};
            if (!ResolveSparsePackageDirectory(dir, _countof(dir), exactPlatformRoot))
                return false;
            if (wcscpy_s(out, cch, dir) != 0 || wcscat_s(out, cch, GlassShared::GlassShellMenu::kSparseMsixFileName) != 0)
                return false;
            return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
        }

        [[nodiscard]] bool IsThumbprintInTrustedPeopleAt(DWORD location, const BYTE* hash, DWORD cbHash) noexcept
        {
            if (!hash || cbHash == 0)
                return false;
            CRYPT_HASH_BLOB blob{ cbHash, const_cast<BYTE*>(hash) };
            HCERTSTORE store = ::CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                location | CERT_STORE_OPEN_EXISTING_FLAG | CERT_STORE_READONLY_FLAG, L"TrustedPeople");
            if (!store)
                return false;
            PCCERT_CONTEXT found = ::CertFindCertificateInStore(
                store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SHA1_HASH, &blob, nullptr);
            const bool hit = found != nullptr;
            if (found)
                ::CertFreeCertificateContext(found);
            ::CertCloseStore(store, 0);
            return hit;
        }

        /// Отпечаток SHA-1 сертификата есть в хранилище TrustedPeople (CU или LM): так доверие
        /// самоподписанному издателю видит и AppX-развёртывание.
        [[nodiscard]] bool IsThumbprintInTrustedPeople(const BYTE* hash, DWORD cbHash) noexcept
        {
            return IsThumbprintInTrustedPeopleAt(CERT_SYSTEM_STORE_CURRENT_USER, hash, cbHash) ||
                   IsThumbprintInTrustedPeopleAt(CERT_SYSTEM_STORE_LOCAL_MACHINE, hash, cbHash);
        }

        /// Authenticode-состояние msix (AppX SIP) без UI и без сети с доступом к листовому сертификату
        /// подписанта; состояние WinVerifyTrust закрывается в деструкторе. Самоподписанный издатель из
        /// TrustedPeople для WINTRUST_ACTION_GENERIC_VERIFY_V2 остаётся «недоверенным корнем», поэтому
        /// вызывающие дополнительно смотрят отпечаток в TrustedPeople — так доверие видит развёртывание.
        struct MsixSignerScope
        {
            WINTRUST_FILE_INFO fileInfo{};
            WINTRUST_DATA data{};
            GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
            LONG verify = static_cast<LONG>(E_FAIL);
            PCCERT_CONTEXT signer = nullptr; // owned by the WinVerifyTrust state, valid until destruction

            explicit MsixSignerScope(const wchar_t* msixPath) noexcept
            {
                fileInfo.cbStruct = sizeof(fileInfo);
                fileInfo.pcwszFilePath = msixPath;
                data.cbStruct = sizeof(data);
                data.dwUIChoice = WTD_UI_NONE;
                data.fdwRevocationChecks = WTD_REVOKE_NONE;
                data.dwUnionChoice = WTD_CHOICE_FILE;
                data.pFile = &fileInfo;
                data.dwStateAction = WTD_STATEACTION_VERIFY;
                data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
                verify = ::WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
                if (CRYPT_PROVIDER_DATA* prov = ::WTHelperProvDataFromStateData(data.hWVTStateData))
                {
                    if (CRYPT_PROVIDER_SGNR* sgnr = ::WTHelperGetProvSignerFromChain(prov, 0, FALSE, 0))
                    {
                        if (sgnr->csCertChain > 0)
                        {
                            if (CRYPT_PROVIDER_CERT* cert = ::WTHelperGetProvCertFromChain(sgnr, 0))
                                signer = cert->pCert;
                        }
                    }
                }
            }

            MsixSignerScope(const MsixSignerScope&) = delete;
            MsixSignerScope& operator=(const MsixSignerScope&) = delete;

            ~MsixSignerScope()
            {
                data.dwStateAction = WTD_STATEACTION_CLOSE;
                (void)::WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
            }

            [[nodiscard]] bool Unsigned() const noexcept { return verify == static_cast<LONG>(kHrNoSignature); }

            void CopySubject(wchar_t* subjectOut, size_t cchSubject) const noexcept
            {
                if (!subjectOut || !cchSubject)
                    return;
                subjectOut[0] = L'\0';
                if (!signer)
                    return;
                DWORD strType = CERT_X500_NAME_STR;
                (void)::CertGetNameStringW(signer, CERT_NAME_RDN_TYPE, 0, &strType, subjectOut,
                    static_cast<DWORD>(cchSubject));
            }

            [[nodiscard]] bool Sha1(BYTE (&hash)[20], DWORD& cbHash) const noexcept
            {
                cbHash = sizeof(hash);
                return signer && ::CertGetCertificateContextProperty(signer, CERT_SHA1_HASH_PROP_ID, hash, &cbHash);
            }
        };

        [[nodiscard]] winrtf::Uri UriFromPath(const wchar_t* path) noexcept
        {
            if (!path || !*path)
                return nullptr;

            wchar_t url[2048]{};
            DWORD cchUrl = static_cast<DWORD>(_countof(url));
            if (FAILED(::UrlCreateFromPathW(path, url, &cchUrl, 0)))
                return nullptr;
            return winrtf::Uri{ url };
        }

        [[nodiscard]] std::vector<std::wstring> CollectSparsePackageFullNames(winmd::PackageManager const& pm) noexcept
        {
            std::vector<std::wstring> names;

            try
            {
                for (auto const& pkg : pm.FindPackagesForUser(L"", GlassShared::GlassShellMenu::kSparsePackageFamilyName))
                {
                    AppendUniquePackageName(names, pkg.Id().FullName());
                }
            }
            catch (winrt::hresult_error const& ex)
            {
                DebugLog(DBG_WARNING, L"[GlassSettings][ShellSparse] FindPackagesForUser: 0x%08X %ls",
                    static_cast<unsigned>(ex.code()), ex.message().c_str());
            }
            catch (...)
            {
                DebugLog(DBG_WARNING, L"[GlassSettings][ShellSparse] FindPackagesForUser: unknown error");
            }

            if (names.empty())
            {
                try
                {
                    for (auto const& pkg : pm.FindPackages())
                    {
                        if (pkg.Id().Name() == GlassShared::GlassShellMenu::kSparsePackageName)
                            AppendUniquePackageName(names, pkg.Id().FullName());
                    }
                }
                catch (...)
                {
                }
            }

            for (const auto& regName : FindSparsePackageFullNamesInRepositoryRegistry())
                AppendUniquePackageName(names, regName);

            return names;
        }

        [[nodiscard]] std::vector<winrt::hstring> FindPackageFullNames(winmd::PackageManager const& pm) noexcept
        {
            std::vector<winrt::hstring> names;
            for (const auto& name : CollectSparsePackageFullNames(pm))
                names.emplace_back(name);
            return names;
        }

        [[nodiscard]] bool RemovePackageByFullName(winmd::PackageManager& pm, const std::wstring& fullName) noexcept
        {
            if (fullName.empty())
                return true;

            try
            {
                DebugLog(DBG_NORMAL, L"[GlassSettings][ShellSparse] RemovePackageAsync: %ls", fullName.c_str());
                auto op = pm.RemovePackageAsync(winrt::hstring{ fullName }, winmd::RemovalOptions::None);
                const auto result = op.get();
                const HRESULT hr = result.ExtendedErrorCode();
                if (FAILED(hr))
                {
                    DebugLog(DBG_WARNING,
                        L"[GlassSettings][ShellSparse] RemovePackageAsync result: 0x%08X %ls",
                        static_cast<unsigned>(hr),
                        result.ErrorText().c_str());
                    return false;
                }
            }
            catch (winrt::hresult_error const& ex)
            {
                DebugLog(DBG_WARNING, L"[GlassSettings][ShellSparse] RemovePackageAsync failed: 0x%08X %ls",
                    static_cast<unsigned>(ex.code()), ex.message().c_str());
                return false;
            }
            catch (...)
            {
                DebugLog(DBG_WARNING, L"[GlassSettings][ShellSparse] RemovePackageAsync failed");
                return false;
            }

            return true;
        }

        [[nodiscard]] bool IsSparsePackagePresentInPackageManager(winmd::PackageManager const& pm) noexcept
        {
            try
            {
                for (auto const& pkg : pm.FindPackagesForUser(L"", GlassShared::GlassShellMenu::kSparsePackageFamilyName))
                {
                    (void)pkg;
                    return true;
                }
            }
            catch (...)
            {
            }

            try
            {
                for (auto const& pkg : pm.FindPackages())
                {
                    if (pkg.Id().Name() == GlassShared::GlassShellMenu::kSparsePackageName)
                        return true;
                }
            }
            catch (...)
            {
            }

            return false;
        }

        [[nodiscard]] bool IsSparsePackagePresentAnywhere(winmd::PackageManager const& pm) noexcept
        {
            return IsSparsePackagePresentInPackageManager(pm) || IsSparsePackagePresentInRepositoryRegistry();
        }

        [[nodiscard]] bool RemoveAllSparsePackages(winmd::PackageManager& pm) noexcept
        {
            bool ok = true;
            for (const auto& fullName : CollectSparsePackageFullNames(pm))
            {
                if (!RemovePackageByFullName(pm, fullName))
                    ok = false;
            }
            return ok;
        }

        [[nodiscard]] bool EnsureSparsePackagesRemoved(winmd::PackageManager& pm, DWORD timeoutMs) noexcept
        {
            const DWORD step = 250;
            const DWORD started = ::GetTickCount();
            while (true)
            {
                if (!IsSparsePackagePresentInPackageManager(pm))
                    return true;

                (void)RemoveAllSparsePackages(pm);

                const DWORD elapsed = ::GetTickCount() - started;
                if (elapsed >= timeoutMs)
                    break;
                ::Sleep(step);
            }

            return !IsSparsePackagePresentInPackageManager(pm);
        }

        void EnsureBlockedKeyExists() noexcept
        {
            HKEY hKey = nullptr;
            if (::RegCreateKeyExW(
                    HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Blocked",
                    0,
                    nullptr,
                    REG_OPTION_NON_VOLATILE,
                    KEY_SET_VALUE,
                    nullptr,
                    &hKey,
                    nullptr) == ERROR_SUCCESS)
            {
                ::RegCloseKey(hKey);
            }
        }

        void SetExplorerCommandsBlocked(bool blocked) noexcept
        {
            static constexpr wchar_t kBlockedKey[] =
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Blocked";

            if (blocked)
                EnsureBlockedKeyExists();

            const wchar_t* const clsids[] = {
                GlassShared::GlassShellMenu::kToggleExplorerClsidBraced,
                GlassShared::GlassShellMenu::kOpenConfigExplorerClsidBraced,
            };

            for (const wchar_t* clsid : clsids)
            {
                if (blocked)
                {
                    (void)::RegSetKeyValueW(
                        HKEY_CURRENT_USER,
                        kBlockedKey,
                        clsid,
                        REG_SZ,
                        L"",
                        sizeof(wchar_t));
                }
                else
                {
                    (void)::RegDeleteKeyValueW(HKEY_CURRENT_USER, kBlockedKey, clsid);
                }
            }
        }

        [[nodiscard]] bool DeploymentSucceeded(winmd::DeploymentResult const& result) noexcept
        {
            if (result.IsRegistered())
            {
                g_lastDeploymentHr.store(S_OK, std::memory_order_release);
                return true;
            }

            const HRESULT hr = result.ExtendedErrorCode();
            g_lastDeploymentHr.store(hr, std::memory_order_release);
            DebugLog(DBG_ERROR, L"[GlassSettings][ShellSparse] deployment failed: 0x%08X %ls",
                static_cast<unsigned>(hr),
                result.ErrorText().c_str());
            if (IsSignerTrustHr(hr))
            {
                DebugLog(DBG_ERROR,
                    L"[GlassSettings][ShellSparse] 0x%08X: сертификат подписи msix (%ls) не в TrustedPeople пользователя — "
                    L"меню будет зарегистрировано классически", static_cast<unsigned>(hr),
                    GlassShared::GlassShellMenu::kSparsePublisher);
            }
            if (hr == kHrPackageExternalLocationConflict)
            {
                DebugLog(DBG_ERROR,
                    L"[GlassSettings][ShellSparse] 0x80073D0B: пакет уже зарегистрирован с другим ExternalLocation");
            }
            return false;
        }
    }

    SparsePackageError GetLastSparsePackageError() noexcept
    {
        return static_cast<SparsePackageError>(g_lastSparsePackageError.load(std::memory_order_acquire));
    }

    HRESULT GetLastSparseDeploymentHr() noexcept
    {
        return g_lastDeploymentHr.load(std::memory_order_acquire);
    }

    bool IsSparsePackageInstalledForCurrentUser() noexcept
    {
        WinRtApartmentScope apartment;
        if (!apartment.CanUsePackageManager())
            return IsSparsePackagePresentInRepositoryRegistry();

        try
        {
            winmd::PackageManager pm;
            return IsSparsePackagePresentInPackageManager(pm);
        }
        catch (...)
        {
            return IsSparsePackagePresentInRepositoryRegistry();
        }
    }

    SparseSignerTrust QuerySparseSignerTrust(
        const wchar_t* exactPlatformRoot, wchar_t* subjectOut, size_t cchSubject) noexcept
    {
        if (subjectOut && cchSubject)
            subjectOut[0] = L'\0';

        wchar_t msixPath[MAX_PATH]{};
        if (!ResolveSparseMsixPath(msixPath, _countof(msixPath), exactPlatformRoot))
            return SparseSignerTrust::MsixMissing;

        const MsixSignerScope scope(msixPath);
        SparseSignerTrust trust = SparseSignerTrust::Unknown;
        if (scope.Unsigned())
        {
            trust = SparseSignerTrust::Unsigned;
        }
        else if (scope.signer)
        {
            scope.CopySubject(subjectOut, cchSubject);
            bool thumbprintTrusted = false;
            BYTE hash[20]{};
            DWORD cbHash = 0;
            if (scope.Sha1(hash, cbHash))
                thumbprintTrusted = IsThumbprintInTrustedPeople(hash, cbHash);
            trust = (scope.verify == ERROR_SUCCESS || thumbprintTrusted) ? SparseSignerTrust::Trusted
                                                                         : SparseSignerTrust::Untrusted;
        }
        else if (scope.verify == ERROR_SUCCESS)
        {
            trust = SparseSignerTrust::Trusted;
        }

        DebugLog(DBG_NORMAL, L"[GlassSettings][ShellSparse] signer trust: WinVerifyTrust=0x%08X state=%d subject=%ls",
            static_cast<unsigned>(scope.verify), static_cast<int>(trust),
            (subjectOut && subjectOut[0]) ? subjectOut : L"-");
        return trust;
    }

    SparseSignerTrustResult TrustSparseSigner(
        const wchar_t* exactPlatformRoot, wchar_t* subjectOut, size_t cchSubject, DWORD* lastErrorOut) noexcept
    {
        if (lastErrorOut)
            *lastErrorOut = 0;
        if (subjectOut && cchSubject)
            subjectOut[0] = L'\0';

        wchar_t msixPath[MAX_PATH]{};
        if (!ResolveSparseMsixPath(msixPath, _countof(msixPath), exactPlatformRoot))
            return SparseSignerTrustResult::MsixMissing;

        const MsixSignerScope scope(msixPath);
        if (scope.Unsigned())
            return SparseSignerTrustResult::Unsigned;
        if (!scope.signer)
            return SparseSignerTrustResult::Unknown;

        // Only the author certificate the package manifest names as Publisher is ever trusted:
        // a re-signed msix from somebody else must not make this helper vouch for them.
        wchar_t subject[512]{};
        scope.CopySubject(subject, _countof(subject));
        if (subjectOut && cchSubject)
            (void)wcscpy_s(subjectOut, cchSubject, subject);
        if (_wcsicmp(subject, GlassShared::GlassShellMenu::kSparsePublisher) != 0)
        {
            DebugLog(DBG_ERROR, L"[GlassSettings][ShellSparse] trust signer: subject %ls is not %ls — nothing trusted",
                subject, GlassShared::GlassShellMenu::kSparsePublisher);
            return SparseSignerTrustResult::SubjectMismatch;
        }

        BYTE hash[20]{};
        DWORD cbHash = 0;
        if (scope.Sha1(hash, cbHash) && IsThumbprintInTrustedPeopleAt(CERT_SYSTEM_STORE_LOCAL_MACHINE, hash, cbHash))
        {
            DebugLog(DBG_NORMAL, L"[GlassSettings][ShellSparse] trust signer: %ls already in LocalMachine\\TrustedPeople", subject);
            return SparseSignerTrustResult::AlreadyTrusted;
        }

        HCERTSTORE store = ::CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, L"TrustedPeople");
        if (!store)
        {
            const DWORD error = ::GetLastError();
            if (lastErrorOut)
                *lastErrorOut = error;
            DebugLog(DBG_ERROR, L"[GlassSettings][ShellSparse] trust signer: CertOpenStore(LM\\TrustedPeople) failed %lu", error);
            return SparseSignerTrustResult::StoreFailed;
        }
        const BOOL added = ::CertAddCertificateContextToStore(store, scope.signer, CERT_STORE_ADD_REPLACE_EXISTING, nullptr);
        const DWORD error = added ? 0 : ::GetLastError();
        ::CertCloseStore(store, 0);
        if (!added)
        {
            if (lastErrorOut)
                *lastErrorOut = error;
            DebugLog(DBG_ERROR, L"[GlassSettings][ShellSparse] trust signer: CertAddCertificateContextToStore failed %lu", error);
            return SparseSignerTrustResult::StoreFailed;
        }

        DebugLog(DBG_IMPORTANT, L"[GlassSettings][ShellSparse] trust signer: %ls added to LocalMachine\\TrustedPeople (%ls)",
            subject, msixPath);
        return SparseSignerTrustResult::Added;
    }

    InstallerSparseState QueryInstallerSparseState(const wchar_t* exactPlatformRoot) noexcept
    {
        if (!exactPlatformRoot || !exactPlatformRoot[0])
            return InstallerSparseState::Unknown;
        // ExternalLocation пакета — подкаталог GlassShellSparse\ платформы (там же GlassShellExt.dll).
        wchar_t expected[MAX_PATH]{};
        if (wcscpy_s(expected, exactPlatformRoot) != 0 ||
            !CanonicalizeOutputDirectory(expected, _countof(expected)) ||
            !AppendSparseSubdir(expected, _countof(expected)))
            return InstallerSparseState::Unknown;

        WinRtApartmentScope apartment;
        if (!apartment.CanUsePackageManager())
            return InstallerSparseState::Unknown;
        try
        {
            winmd::PackageManager pm;
            std::wstring currentFullName;
            // The two-argument overload takes a family name, not Identity@Name.
            for (auto const& pkg : pm.FindPackagesForUser(
                     L"", GlassShared::GlassShellMenu::kSparsePackageFamilyName))
            {
                if (!currentFullName.empty() || !pkg.Status().VerifyIsOK())
                    return InstallerSparseState::Conflict;
                const auto location = pkg.EffectiveExternalPath();
                wchar_t actual[MAX_PATH]{};
                if (location.empty() || wcscpy_s(actual, location.c_str()) != 0 ||
                    !CanonicalizeOutputDirectory(actual, _countof(actual)) ||
                    _wcsicmp(actual, expected) != 0)
                    return InstallerSparseState::Conflict;
                currentFullName = pkg.Id().FullName().c_str();
                if (currentFullName.empty())
                    return InstallerSparseState::Unknown;
            }
            // Never remove a same-name package registered only for another user
            // or an additional version as a side effect of this installer.
            for (auto const& pkg : pm.FindPackages())
            {
                if (pkg.Id().Name() != GlassShared::GlassShellMenu::kSparsePackageName)
                    continue;
                const auto fullName = pkg.Id().FullName();
                if (currentFullName.empty() || _wcsicmp(fullName.c_str(), currentFullName.c_str()) != 0)
                    return InstallerSparseState::Conflict;
            }
            return currentFullName.empty() ? InstallerSparseState::Absent : InstallerSparseState::Matching;
        }
        catch (...)
        {
            return InstallerSparseState::Unknown;
        }
    }

    bool InstallerSparseHasVerifiedMenu(const wchar_t* exactPlatformRoot) noexcept
    {
        WinRtApartmentScope apartment;
        if (!apartment.CanUsePackageManager() ||
            QueryInstallerSparseState(exactPlatformRoot)!=InstallerSparseState::Matching)
            return false;
        try
        {
            winmd::PackageManager pm;
            for (auto const& pkg : pm.FindPackagesForUser(
                     L"",GlassShared::GlassShellMenu::kSparsePackageFamilyName))
            {
                // A loose development registration can use a different file
                // with the same identity (including our unregister stub).
                // InstalledLocation alone cannot establish which was applied.
                if (pkg.IsDevelopmentMode())
                    return false;
                const std::wstring path=std::wstring(pkg.InstalledLocation().Path().c_str())+L"\\AppxManifest.xml";
                WIN32_FILE_ATTRIBUTE_DATA attributes{};
                if (!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&attributes) ||
                    attributes.nFileSizeHigh || attributes.nFileSizeLow>1024*1024 ||
                    (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    return false;
                auto file=winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(winrt::hstring{ path }).get();
                winrt::Windows::Data::Xml::Dom::XmlLoadSettings settings;
                settings.ProhibitDtd(true);
                settings.ResolveExternals(false);
                auto xml=winrt::Windows::Data::Xml::Dom::XmlDocument::LoadFromFileAsync(file,settings).get();
                const auto ns=winrt::box_value(winrt::hstring{
                    L"xmlns:d4='http://schemas.microsoft.com/appx/manifest/desktop/windows10/4' "
                    L"xmlns:d5='http://schemas.microsoft.com/appx/manifest/desktop/windows10/5' "
                    L"xmlns:com='http://schemas.microsoft.com/appx/manifest/com/windows10'"});
                const auto verb=xml.SelectSingleNodeNS(
                    L"//d4:Extension[@Category='windows.fileExplorerContextMenus']/d4:FileExplorerContextMenus/"
                    L"d5:ItemType[@Type='Directory\\Background']/d5:Verb[@Id='GlassPlatform00Toggle' and "
                    L"@Clsid='6D4E18F4-91B2-4C7D-A3E9-442B7C889D41']",ns);
                const auto cls=xml.SelectSingleNodeNS(
                    L"//com:Extension[@Category='windows.comServer']/com:ComServer/com:SurrogateServer/"
                    L"com:Class[@Id='6D4E18F4-91B2-4C7D-A3E9-442B7C889D41' and @Path='GlassShellExt.dll']",ns);
                return verb && cls;
            }
        }
        catch (...) {}
        return false;
    }

    bool UnregisterInstallerSparsePackageQuiet(const wchar_t* exactPlatformRoot) noexcept
    {
        WinRtApartmentScope apartment;
        if (!apartment.CanUsePackageManager())
            return false;
        try
        {
            const auto state=QueryInstallerSparseState(exactPlatformRoot);
            if (state==InstallerSparseState::Absent)
                return true;
            if (state!=InstallerSparseState::Matching)
                return false;
            winmd::PackageManager pm;
            auto packages=pm.FindPackagesForUser(L"",GlassShared::GlassShellMenu::kSparsePackageFamilyName);
            std::wstring fullName;
            for (auto const& pkg : packages)
            {
                if (!fullName.empty())
                    return false;
                fullName=pkg.Id().FullName().c_str();
            }
            if (fullName.empty() || !DeploymentSucceeded(
                    pm.RemovePackageAsync(winrt::hstring{ fullName },winmd::RemovalOptions::None).get()))
                return false;
            // Another user's installation may still appear machine-wide. It is
            // not ours to remove and must not trigger the unregister-stub path.
            for (auto const& pkg : pm.FindPackagesForUser(
                     L"",GlassShared::GlassShellMenu::kSparsePackageFamilyName))
            {
                (void)pkg;
                return false;
            }
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool RegisterSparsePackageQuiet(const wchar_t* exactPlatformRoot) noexcept
    {
        SetLastSparsePackageError(SparsePackageError::None);
        g_lastDeploymentHr.store(S_OK, std::memory_order_release);

        // ExternalLocation = <root>\GlassShellSparse\: там GlassShellExt.dll (com:Class Path),
        // GlassShellHost.exe (Application Executable), манифесты и msix.
        wchar_t sparseDir[MAX_PATH]{};
        wchar_t manifest[MAX_PATH]{};
        if (!ResolveSparsePackageDirectory(sparseDir, _countof(sparseDir), exactPlatformRoot) ||
            !ResolveSparseManifestPath(manifest, _countof(manifest), exactPlatformRoot))
        {
            DebugLog(DBG_ERROR, L"[GlassSettings][ShellSparse] register: пути не найдены");
            SetLastSparsePackageError(SparsePackageError::ArtifactsMissing);
            return false;
        }

        if (GetFileAttributesW(manifest) == INVALID_FILE_ATTRIBUTES)
        {
            DebugLog(DBG_ERROR, L"[GlassSettings][ShellSparse] register: нет AppxManifest.xml");
            SetLastSparsePackageError(SparsePackageError::ArtifactsMissing);
            return false;
        }

        wchar_t shellExt[MAX_PATH]{};
        if (wcscpy_s(shellExt, sparseDir) != 0 ||
            wcscat_s(shellExt, GlassShared::GlassShellMenu::kShellExtFileName) != 0 ||
            GetFileAttributesW(shellExt) == INVALID_FILE_ATTRIBUTES)
        {
            DebugLog(DBG_ERROR, L"[GlassSettings][ShellSparse] register: нет GlassShellExt.dll в GlassShellSparse\\");
            SetLastSparsePackageError(SparsePackageError::ArtifactsMissing);
            return false;
        }

        if (!CanonicalizeOutputDirectory(sparseDir, _countof(sparseDir)))
        {
            DebugLog(DBG_ERROR, L"[GlassSettings][ShellSparse] register: не удалось нормализовать каталог");
            SetLastSparsePackageError(SparsePackageError::ArtifactsMissing);
            return false;
        }

        WinRtApartmentScope apartment;
        if (!apartment.CanUsePackageManager())
        {
            DebugLog(DBG_ERROR, L"[GlassSettings][ShellSparse] register: init_apartment failed");
            SetLastSparsePackageError(SparsePackageError::DeploymentFailed);
            return false;
        }

        try
        {
            winmd::PackageManager pm;
            const bool installerMode = exactPlatformRoot != nullptr;
            if (installerMode)
            {
                // Setup never removes or replaces an existing package during
                // registration: there is no complete package-state rollback.
                if (QueryInstallerSparseState(exactPlatformRoot) != InstallerSparseState::Absent)
                {
                    SetLastSparsePackageError(SparsePackageError::ExternalLocationConflict);
                    return false;
                }
            }
            else if (!EnsureSparsePackagesRemoved(pm, 10000))
            {
                DebugLog(DBG_ERROR, L"[GlassSettings][ShellSparse] register: не удалось снять прежний sparse-пакет");
                SetLastSparsePackageError(SparsePackageError::ExternalLocationConflict);
                return false;
            }

            const winrtf::Uri outDirUri = UriFromPath(sparseDir);
            const winrtf::Uri manifestUri = UriFromPath(manifest);
            if (!outDirUri || !manifestUri)
            {
                DebugLog(DBG_ERROR, L"[GlassSettings][ShellSparse] register: UriFromPath failed");
                SetLastSparsePackageError(SparsePackageError::ArtifactsMissing);
                return false;
            }

            const auto tryDeployMsix = [&](const wchar_t* msixPath) -> bool
            {
                const winrtf::Uri msixUri = UriFromPath(msixPath);
                if (!msixUri)
                    return false;

                winmd::AddPackageOptions options;
                options.ExternalLocationUri(outDirUri);
                options.ForceUpdateFromAnyVersion(!installerMode);
                DebugLog(DBG_IMPORTANT, L"[GlassSettings][ShellSparse] AddPackageByUriAsync msix: %ls", msixPath);
                if (DeploymentSucceeded(pm.AddPackageByUriAsync(msixUri, options).get()))
                    return true;

                // A package that appeared concurrently is not evidence that our
                // registration succeeded, and must not be removed by Setup.
                if (installerMode)
                    return false;
                if (IsSparsePackagePresentAnywhere(pm))
                    return true;

                if (g_lastDeploymentHr.load(std::memory_order_acquire) != kHrPackageExternalLocationConflict)
                    return false;

                DebugLog(DBG_WARNING, L"[GlassSettings][ShellSparse] 0x80073D0B: повтор после снятия пакета");
                if (!EnsureSparsePackagesRemoved(pm, 10000))
                    return false;

                return DeploymentSucceeded(pm.AddPackageByUriAsync(msixUri, options).get());
            };

            bool registered = false;
            wchar_t msixPath[MAX_PATH]{};
            const bool hasMsix = ResolveSparseMsixPath(msixPath, _countof(msixPath), exactPlatformRoot);
            if (!hasMsix)
                DebugLog(DBG_WARNING, L"[GlassSettings][ShellSparse] register: нет GlassShellSparse.msix, только loose-манифест");

            if (hasMsix)
                registered = tryDeployMsix(msixPath);

            // Подписанный msix отклонён из-за недоверия издателю: loose-регистрация того же манифеста
            // на машине без режима разработчика тоже невозможна; причина должна остаться SignerNotTrusted.
            const HRESULT msixHr = g_lastDeploymentHr.load(std::memory_order_acquire);
            if (!registered && hasMsix && IsSignerTrustHr(msixHr))
            {
                SetLastSparsePackageError(SparsePackageError::SignerNotTrusted);
                return false;
            }

            if (!registered)
            {
                if (installerMode && QueryInstallerSparseState(exactPlatformRoot) != InstallerSparseState::Absent)
                {
                    SetLastSparsePackageError(SparsePackageError::ExternalLocationConflict);
                    return false;
                }
                winmd::RegisterPackageOptions regOptions;
                regOptions.ExternalLocationUri(outDirUri);
                regOptions.ForceUpdateFromAnyVersion(!installerMode);
                DebugLog(DBG_IMPORTANT, L"[GlassSettings][ShellSparse] RegisterPackageByUriAsync manifest: %ls", manifest);
                registered = DeploymentSucceeded(pm.RegisterPackageByUriAsync(manifestUri, regOptions).get());
                if (!registered && !installerMode &&
                    g_lastDeploymentHr.load(std::memory_order_acquire) == kHrPackageExternalLocationConflict &&
                    EnsureSparsePackagesRemoved(pm, 10000))
                {
                    registered = DeploymentSucceeded(pm.RegisterPackageByUriAsync(manifestUri, regOptions).get());
                }
            }

            if (!registered)
            {
                const HRESULT hr = g_lastDeploymentHr.load(std::memory_order_acquire);
                SetLastSparsePackageError(ClassifyDeploymentFailure(hr));
                return false;
            }

            if (installerMode && QueryInstallerSparseState(exactPlatformRoot) != InstallerSparseState::Matching)
            {
                SetLastSparsePackageError(SparsePackageError::DeploymentFailed);
                return false;
            }
            SetExplorerCommandsBlocked(false);

            if (FindPackageFullNames(pm).empty() && !IsSparsePackagePresentInRepositoryRegistry())
            {
                DebugLog(DBG_ERROR, L"[GlassSettings][ShellSparse] register: пакет не найден после установки");
                SetLastSparsePackageError(SparsePackageError::DeploymentFailed);
                return false;
            }

            DebugLog(DBG_IMPORTANT, L"[GlassSettings][ShellSparse] register: успех");
            return true;
        }
        catch (winrt::hresult_error const& ex)
        {
            DebugLog(DBG_ERROR, L"[GlassSettings][ShellSparse] register: 0x%08X %ls",
                static_cast<unsigned>(ex.code()), ex.message().c_str());
            g_lastDeploymentHr.store(static_cast<HRESULT>(ex.code()), std::memory_order_release);
            SetLastSparsePackageError(ClassifyDeploymentFailure(static_cast<HRESULT>(ex.code())));
            return false;
        }
        catch (...)
        {
            DebugLog(DBG_ERROR, L"[GlassSettings][ShellSparse] register: unknown error");
            SetLastSparsePackageError(SparsePackageError::DeploymentFailed);
            return false;
        }
    }

    bool UnregisterSparsePackageQuiet(const wchar_t* exactPlatformRoot) noexcept
    {
        wchar_t outDir[MAX_PATH]{};
        wchar_t stubManifest[MAX_PATH]{};
        if (!ResolveSparsePackageDirectory(outDir, _countof(outDir), exactPlatformRoot) ||
            !ResolveSparseUnregisterStubManifestPath(stubManifest, _countof(stubManifest), exactPlatformRoot))
        {
            DebugLog(DBG_ERROR, L"[GlassSettings][ShellSparse] unregister: пути не найдены");
            SetLastSparsePackageError(SparsePackageError::ArtifactsMissing);
            return false;
        }

        const bool hasStubManifest = ::GetFileAttributesW(stubManifest) != INVALID_FILE_ATTRIBUTES;
        if (!hasStubManifest)
        {
            DebugLog(DBG_WARNING, L"[GlassSettings][ShellSparse] unregister: нет stub manifest, только RemovePackage");
        }

        WinRtApartmentScope apartment;
        if (!apartment.CanUsePackageManager())
        {
            DebugLog(DBG_ERROR, L"[GlassSettings][ShellSparse] unregister: init_apartment failed");
            return false;
        }

        try
        {
            winmd::PackageManager pm;
            const auto existing = FindPackageFullNames(pm);
            if (existing.empty())
            {
                SetExplorerCommandsBlocked(false);
                DebugLog(DBG_NORMAL, L"[GlassSettings][ShellSparse] unregister: пакет уже отсутствует");
                return true;
            }

            SetExplorerCommandsBlocked(true);

            const winrtf::Uri outDirUri = UriFromPath(outDir);
            const winrtf::Uri stubUri = UriFromPath(stubManifest);
            if (hasStubManifest && outDirUri && stubUri)
            {
                winmd::RegisterPackageOptions stubOptions;
                stubOptions.ExternalLocationUri(outDirUri);
                stubOptions.ForceUpdateFromAnyVersion(true);
                try
                {
                    DebugLog(DBG_IMPORTANT, L"[GlassSettings][ShellSparse] RegisterPackageByUriAsync stub");
                    (void)pm.RegisterPackageByUriAsync(stubUri, stubOptions).get();
                }
                catch (winrt::hresult_error const& ex)
                {
                    DebugLog(DBG_WARNING, L"[GlassSettings][ShellSparse] stub register: 0x%08X %ls",
                        static_cast<unsigned>(ex.code()), ex.message().c_str());
                }
            }

            if (!EnsureSparsePackagesRemoved(pm, 15000))
            {
                DebugLog(DBG_ERROR, L"[GlassSettings][ShellSparse] unregister: пакет всё ещё установлен");
                return false;
            }
            SetExplorerCommandsBlocked(false);

            if (IsSparsePackagePresentInPackageManager(pm))
            {
                DebugLog(DBG_ERROR, L"[GlassSettings][ShellSparse] unregister: PackageManager всё ещё видит пакет");
                return false;
            }

            DebugLog(DBG_IMPORTANT, L"[GlassSettings][ShellSparse] unregister: успех");
            return true;
        }
        catch (winrt::hresult_error const& ex)
        {
            SetExplorerCommandsBlocked(false);
            DebugLog(DBG_ERROR, L"[GlassSettings][ShellSparse] unregister: 0x%08X %ls",
                static_cast<unsigned>(ex.code()), ex.message().c_str());
            return false;
        }
        catch (...)
        {
            SetExplorerCommandsBlocked(false);
            DebugLog(DBG_ERROR, L"[GlassSettings][ShellSparse] unregister: unknown error");
            return false;
        }
    }
}

#endif
