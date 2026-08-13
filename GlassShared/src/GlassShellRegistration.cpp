#include "SharedPch.h"

#include <GlassShellMenuConstants.h>
#include <GlassShellRegistration.h>

#include <strsafe.h>

namespace Shared::GlassShellRegistration
{
    namespace
    {
        [[nodiscard]] LONG DeleteRegistryTreeRelative(HKEY root, const wchar_t* subkey) noexcept
        {
            return ::RegDeleteTreeW(root, subkey);
        }

        [[nodiscard]] bool ReadHandlerDefaultClsidForRoot(
            const wchar_t* shellClassRoot,
            wchar_t* buf,
            size_t cchChars) noexcept
        {
            if (!shellClassRoot || !buf || cchChars == 0)
                return false;
            buf[0] = L'\0';

            wchar_t key[384]{};
            if (FAILED(::StringCchPrintfW(
                    key,
                    _countof(key),
                    L"Software\\Classes\\%s\\ShellEx\\ContextMenuHandlers\\%s",
                    shellClassRoot,
                    GlassShared::GlassShellMenu::kHandlerSubkeyName)))
                return false;

            DWORD cb = static_cast<DWORD>(cchChars * sizeof(wchar_t));
            const LSTATUS st = ::RegGetValueW(HKEY_CURRENT_USER, key, nullptr, RRF_RT_REG_SZ, nullptr, buf, &cb);
            return st == ERROR_SUCCESS && buf[0] != L'\0';
        }

        [[nodiscard]] bool IsOurLegacyHandlerOnRoot(const wchar_t* shellClassRoot) noexcept
        {
            wchar_t value[160]{};
            if (!ReadHandlerDefaultClsidForRoot(shellClassRoot, value, _countof(value)))
                return false;
            return _wcsicmp(value, GlassShared::GlassShellMenu::kClsidStringBraced) == 0;
        }
    }

    void RemoveLegacyContextMenuHandler() noexcept
    {
        wchar_t key[384]{};
        const wchar_t* const roots[] = {L"DesktopBackground", L"Directory\\Background"};
        for (const wchar_t* root : roots)
        {
            if (SUCCEEDED(::StringCchPrintfW(
                    key,
                    _countof(key),
                    L"Software\\Classes\\%s\\ShellEx\\ContextMenuHandlers\\%s",
                    root,
                    GlassShared::GlassShellMenu::kHandlerSubkeyName)))
            {
                (void)DeleteRegistryTreeRelative(HKEY_CURRENT_USER, key);
            }
        }

        if (SUCCEEDED(::StringCchPrintfW(key, _countof(key), L"Software\\Classes\\CLSID\\%s",
                GlassShared::GlassShellMenu::kClsidStringBraced)))
        {
            (void)DeleteRegistryTreeRelative(HKEY_CURRENT_USER, key);
        }
    }

    bool IsLegacyContextMenuHandlerPresent() noexcept
    {
        return IsOurLegacyHandlerOnRoot(L"DesktopBackground") ||
               IsOurLegacyHandlerOnRoot(L"Directory\\Background");
    }
}
