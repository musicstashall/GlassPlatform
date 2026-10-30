#ifndef GLASS_SETTINGS_SHELL_SPARSE_PACKAGE_CPP
#define GLASS_SETTINGS_SHELL_SPARSE_PACKAGE_CPP

#include <windows.h>
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Management.Deployment.h>

#include <GlassModulePath.h>
#include <GlassShellMenuConstants.h>
#include <DebugLog.h>

#include "ShellDevCertificate.h"
#include "ShellSparsePackage.h"

#include <Shlwapi.h>
#include <ShlObj.h>
#include <roapi.h>
#include <wil/resource.h>

#include <string>
#include <vector>
#include <atomic>

#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

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
                if (m_hr == S_OK)
                    m_owned = true;
            }

            ~WinRtApartmentScope() noexcept
            {
                // Только если мы подняли MTA на этом потоке.
                // S_FALSE — WinRT уже MTA; RPC_E_CHANGED_MODE — STA (UI), RoUninitialize нельзя.
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

        /// Каталог ExternalLocation sparse-пакета: рядом с GlassShellExt.dll (как PackageRootFolder в AppX).
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

            std::wstring shellExt =
                GlassShared::GlassModulePath::ResolveGlassModulePathNextToModule(nullptr, L"GlassShellExt.dll");
            if (shellExt.empty())
            {
                shellExt = GlassShared::GlassModulePath::ResolveGlassModulePathNextToModule(
                    reinterpret_cast<HMODULE>(&__ImageBase), L"GlassShellExt.dll");
            }
            if (!shellExt.empty())
            {
                const std::wstring root = GlassShared::GlassModulePath::GetInstallRootFromPath(shellExt.c_str());
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

        [[nodiscard]] bool ResolveSparseMsixPath(wchar_t* out, size_t cch, const wchar_t* exactPlatformRoot) noexcept
        {
            wchar_t dir[MAX_PATH]{};
            if (!ResolveGlassPlatformOutputDirectory(dir, _countof(dir), exactPlatformRoot))
                return false;

            // PostBuild BuildSparsePackage.ps1 кладёт msix в корень OutDir, не в GlassShellSparse\.
            if (wcscpy_s(out, cch, dir) == 0 && wcscat_s(out, cch, L"GlassShellSparse.msix") == 0 &&
                GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
                return true;

            if (!AppendSparseSubdir(dir, _countof(dir)))
                return false;
            if (wcscpy_s(out, cch, dir) != 0 || wcscat_s(out, cch, L"GlassShellSparse.msix") != 0)
                return false;
            return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
        }

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
                for (auto const& pkg : pm.FindPackagesForUser(L"", GlassShared::GlassShellMenu::kSparsePackageName))
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
                for (auto const& pkg : pm.FindPackagesForUser(L"", GlassShared::GlassShellMenu::kSparsePackageName))
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
            if (hr == static_cast<HRESULT>(0x800B0109))
            {
                DebugLog(DBG_ERROR,
                    L"[GlassSettings][ShellSparse] 0x800B0109: cert подписи msix должен быть в TrustedPeople (MS Learn)");
            }
            if (hr == kHrPackageExternalLocationConflict)
            {
                DebugLog(DBG_ERROR,
                    L"[GlassSettings][ShellSparse] 0x80073D0B: пакет уже зарегистрирован с другим ExternalLocation");
            }
            return false;
        }

        struct SparseSdkTools
        {
            wchar_t makeAppx[MAX_PATH]{};
            wchar_t signTool[MAX_PATH]{};

            [[nodiscard]] bool Valid() const noexcept
            {
                return makeAppx[0] != L'\0' && signTool[0] != L'\0';
            }
        };

        [[nodiscard]] bool AppendMaxBinVersionToolPath(
            const wchar_t* kitsRoot,
            const wchar_t* toolName,
            wchar_t* out,
            size_t cch) noexcept
        {
            if (!kitsRoot || !toolName || !out || cch == 0)
                return false;
            out[0] = L'\0';

            wchar_t binDir[MAX_PATH]{};
            if (swprintf_s(binDir, _countof(binDir), L"%s\\bin", kitsRoot) <= 0)
                return false;

            WIN32_FIND_DATAW fd{};
            wchar_t search[MAX_PATH]{};
            if (swprintf_s(search, _countof(search), L"%s\\*", binDir) <= 0)
                return false;

            wil::unique_hfind hFind{ ::FindFirstFileW(search, &fd) };
            if (!hFind || hFind.get() == INVALID_HANDLE_VALUE)
                return false;

            std::wstring bestVersion;
            do
            {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                    continue;
                if (fd.cFileName[0] == L'.')
                    continue;
                if (bestVersion.empty() || wcscmp(fd.cFileName, bestVersion.c_str()) > 0)
                    bestVersion = fd.cFileName;
            } while (::FindNextFileW(hFind.get(), &fd));

            if (bestVersion.empty())
                return false;

            const bool ok = swprintf_s(out, cch, L"%s\\bin\\%s\\x64\\%s", kitsRoot, bestVersion.c_str(), toolName) > 0;
            return ok && GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
        }

        [[nodiscard]] bool ResolveSparseSdkTools(SparseSdkTools& tools) noexcept
        {
            tools.makeAppx[0] = L'\0';
            tools.signTool[0] = L'\0';

            wchar_t kitsRoot[MAX_PATH]{};
            HKEY hKey = nullptr;
            if (::RegOpenKeyExW(
                    HKEY_LOCAL_MACHINE,
                    L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows Kits\\Installed Roots",
                    0,
                    KEY_READ,
                    &hKey) == ERROR_SUCCESS)
            {
                DWORD cb = sizeof(kitsRoot);
                DWORD type = REG_SZ;
                (void)::RegQueryValueExW(hKey, L"KitsRoot10", nullptr, &type, reinterpret_cast<LPBYTE>(kitsRoot), &cb);
                ::RegCloseKey(hKey);
            }

            if (kitsRoot[0] == L'\0')
            {
                if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILESX86, nullptr, SHGFP_TYPE_CURRENT, kitsRoot)))
                    return false;
                if (wcscat_s(kitsRoot, _countof(kitsRoot), L"\\Windows Kits\\10\\") != 0)
                    return false;
            }

            const size_t len = wcslen(kitsRoot);
            if (len > 0 && kitsRoot[len - 1] != L'\\')
                (void)wcscat_s(kitsRoot, _countof(kitsRoot), L"\\");

            if (!AppendMaxBinVersionToolPath(kitsRoot, L"makeappx.exe", tools.makeAppx, _countof(tools.makeAppx)))
                return false;
            if (!AppendMaxBinVersionToolPath(kitsRoot, L"signtool.exe", tools.signTool, _countof(tools.signTool)))
                return false;
            return true;
        }

        [[nodiscard]] bool RunSdkToolCommand(const std::wstring& cmdLine) noexcept
        {
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
            cmdBuf.push_back(L'\0');

            if (!::CreateProcessW(
                    nullptr,
                    cmdBuf.data(),
                    nullptr,
                    nullptr,
                    FALSE,
                    CREATE_NO_WINDOW,
                    nullptr,
                    nullptr,
                    &si,
                    &pi))
            {
                DebugLog(DBG_WARNING, L"[GlassSettings][ShellSparse] SDK tool CreateProcess failed: %lu", GetLastError());
                return false;
            }

            wil::unique_handle process{ pi.hProcess };
            wil::unique_handle thread{ pi.hThread };
            (void)thread;

            const DWORD waitResult = ::WaitForSingleObject(process.get(), 120000);
            if (waitResult != WAIT_OBJECT_0)
            {
                DebugLog(DBG_WARNING, L"[GlassSettings][ShellSparse] SDK tool timeout");
                ::TerminateProcess(process.get(), 1);
                return false;
            }

            DWORD exitCode = 1;
            (void)::GetExitCodeProcess(process.get(), &exitCode);
            if (exitCode != 0)
            {
                DebugLog(DBG_WARNING, L"[GlassSettings][ShellSparse] SDK tool exit=%lu", exitCode);
                return false;
            }
            return true;
        }

        [[nodiscard]] bool RefreshSignedSparseMsix(
            const wchar_t* sparseDir,
            const wchar_t* msixPath,
            const wchar_t* thumbprint,
            const SparseSdkTools& tools) noexcept
        {
            if (!sparseDir || !msixPath || !thumbprint || !*sparseDir || !*msixPath || !*thumbprint || !tools.Valid())
                return false;

            std::wstring packCmd = L"\"";
            packCmd += tools.makeAppx;
            packCmd += L"\" pack /o /d \"";
            packCmd += sparseDir;
            packCmd += L"\" /p \"";
            packCmd += msixPath;
            packCmd += L"\" /nv";

            DebugLog(DBG_NORMAL, L"[GlassSettings][ShellSparse] makeappx pack: %ls", msixPath);
            if (!RunSdkToolCommand(packCmd))
                return false;

            std::wstring signCmd = L"\"";
            signCmd += tools.signTool;
            signCmd += L"\" sign /fd SHA256 /a /sha1 ";
            signCmd += thumbprint;
            signCmd += L" \"";
            signCmd += msixPath;
            signCmd += L"\"";

            DebugLog(DBG_IMPORTANT, L"[GlassSettings][ShellSparse] signtool sign thumbprint=%ls", thumbprint);
            if (!RunSdkToolCommand(signCmd))
                return false;

            return true;
        }
    }

    SparsePackageError GetLastSparsePackageError() noexcept
    {
        return static_cast<SparsePackageError>(g_lastSparsePackageError.load(std::memory_order_acquire));
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

    bool RegisterSparsePackageQuiet(bool provisionDevCertificate, const wchar_t* exactPlatformRoot) noexcept
    {
        SetLastSparsePackageError(SparsePackageError::None);

        wchar_t outDir[MAX_PATH]{};
        wchar_t sparseDir[MAX_PATH]{};
        wchar_t manifest[MAX_PATH]{};
        if (!ResolveGlassPlatformOutputDirectory(outDir, _countof(outDir), exactPlatformRoot) ||
            !ResolveSparsePackageDirectory(sparseDir, _countof(sparseDir), exactPlatformRoot) ||
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

        if (!CanonicalizeOutputDirectory(outDir, _countof(outDir)))
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
            if (!EnsureSparsePackagesRemoved(pm, 10000))
            {
                DebugLog(DBG_ERROR, L"[GlassSettings][ShellSparse] register: не удалось снять прежний sparse-пакет");
                SetLastSparsePackageError(SparsePackageError::ExternalLocationConflict);
                return false;
            }

            if (provisionDevCertificate && !ShellDevCertificate::EnsureGlassShellDevCertificateTrusted())
            {
                DebugLog(DBG_ERROR, L"[GlassSettings][ShellSparse] register: dev certificate failed");
                SetLastSparsePackageError(SparsePackageError::DevCertFailed);
                return false;
            }

            const winrtf::Uri outDirUri = UriFromPath(outDir);
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
                options.ForceUpdateFromAnyVersion(true);
                DebugLog(DBG_IMPORTANT, L"[GlassSettings][ShellSparse] AddPackageByUriAsync msix: %ls", msixPath);
                if (DeploymentSucceeded(pm.AddPackageByUriAsync(msixUri, options).get()))
                    return true;

                if (IsSparsePackagePresentAnywhere(pm))
                    return true;

                if (g_lastDeploymentHr.load(std::memory_order_acquire) != kHrPackageExternalLocationConflict)
                    return false;

                DebugLog(DBG_WARNING, L"[GlassSettings][ShellSparse] 0x80073D0B: повтор после снятия пакета");
                if (!EnsureSparsePackagesRemoved(pm, 10000))
                    return false;

                return DeploymentSucceeded(pm.AddPackageByUriAsync(msixUri, options).get());
            };

            wchar_t thumbprint[64]{};
            if (provisionDevCertificate)
                (void)ShellDevCertificate::GetGlassShellDevCertThumbprint(thumbprint, _countof(thumbprint));

            SparseSdkTools sdk{};
            const bool sdkOk = provisionDevCertificate && ResolveSparseSdkTools(sdk);
            if (!sdkOk)
            {
                DebugLog(DBG_NORMAL, L"[GlassSettings][ShellSparse] Windows SDK tools не найдены, пропуск repack/sign");
            }

            bool registered = false;
            wchar_t msixPath[MAX_PATH]{};
            const bool hasMsix = ResolveSparseMsixPath(msixPath, _countof(msixPath), exactPlatformRoot);

            if (hasMsix)
                registered = tryDeployMsix(msixPath);

            if (!registered && sdkOk && thumbprint[0] != L'\0' && hasMsix)
            {
                if (RefreshSignedSparseMsix(sparseDir, msixPath, thumbprint, sdk))
                    registered = tryDeployMsix(msixPath);
                else
                    DebugLog(DBG_WARNING, L"[GlassSettings][ShellSparse] pack/sign msix не удался");
            }

            if (!registered)
            {
                winmd::RegisterPackageOptions regOptions;
                regOptions.ExternalLocationUri(outDirUri);
                regOptions.ForceUpdateFromAnyVersion(true);
                DebugLog(DBG_IMPORTANT, L"[GlassSettings][ShellSparse] RegisterPackageByUriAsync manifest: %ls", manifest);
                registered = DeploymentSucceeded(pm.RegisterPackageByUriAsync(manifestUri, regOptions).get());
                if (!registered &&
                    g_lastDeploymentHr.load(std::memory_order_acquire) == kHrPackageExternalLocationConflict &&
                    EnsureSparsePackagesRemoved(pm, 10000))
                {
                    registered = DeploymentSucceeded(pm.RegisterPackageByUriAsync(manifestUri, regOptions).get());
                }
            }

            if (!registered)
            {
                const HRESULT hr = g_lastDeploymentHr.load(std::memory_order_acquire);
                SetLastSparsePackageError(
                    hr == kHrPackageExternalLocationConflict ? SparsePackageError::ExternalLocationConflict
                                                             : SparsePackageError::DeploymentFailed);
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
            SetLastSparsePackageError(SparsePackageError::DeploymentFailed);
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
        if (!ResolveGlassPlatformOutputDirectory(outDir, _countof(outDir), exactPlatformRoot) ||
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
