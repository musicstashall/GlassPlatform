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

    bool RegisterLegacyContextMenuHandler(const wchar_t* modulePath) noexcept
    {
        if (!modulePath || modulePath[0] == L'\0')
            return false;

        wchar_t clsidSubkey[384]{};
        if (FAILED(::StringCchPrintfW(clsidSubkey, _countof(clsidSubkey),
                L"Software\\Classes\\CLSID\\%s\\InprocServer32",
                GlassShared::GlassShellMenu::kClsidStringBraced)))
            return false;

        const DWORD moduleCb = static_cast<DWORD>((wcslen(modulePath) + 1U) * sizeof(wchar_t));
        LONG e = ::RegSetKeyValueW(HKEY_CURRENT_USER, clsidSubkey, nullptr, REG_SZ, modulePath, moduleCb);
        if (e != ERROR_SUCCESS)
            return false;

        static constexpr wchar_t kApartment[] = L"Apartment";
        e = ::RegSetKeyValueW(HKEY_CURRENT_USER, clsidSubkey, L"ThreadingModel", REG_SZ, kApartment,
            static_cast<DWORD>(sizeof(kApartment)));
        if (e != ERROR_SUCCESS)
            return false;

        wchar_t handlerSubkey[384]{};
        if (FAILED(::StringCchPrintfW(handlerSubkey, _countof(handlerSubkey),
                L"Software\\Classes\\DesktopBackground\\ShellEx\\ContextMenuHandlers\\%s",
                GlassShared::GlassShellMenu::kHandlerSubkeyName)))
            return false;

        const wchar_t* clsid = GlassShared::GlassShellMenu::kClsidStringBraced;
        const DWORD clsidCb = static_cast<DWORD>((wcslen(clsid) + 1U) * sizeof(wchar_t));
        e = ::RegSetKeyValueW(HKEY_CURRENT_USER, handlerSubkey, nullptr, REG_SZ, clsid, clsidCb);
        return e == ERROR_SUCCESS;
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

    void RemoveStaleExplorerCommandRegistration() noexcept
    {
        wchar_t key[384]{};
        const wchar_t* const shellRoots[] = {L"DesktopBackground", L"Directory\\Background"};
        for (const wchar_t* root : shellRoots)
        {
            for (const wchar_t* verb :
                {GlassShared::GlassShellMenu::kDesktopToggleVerbSubkey,
                    GlassShared::GlassShellMenu::kDesktopOpenConfigVerbSubkey})
            {
                if (SUCCEEDED(::StringCchPrintfW(key, _countof(key), L"Software\\Classes\\%s\\Shell\\%s", root, verb)))
                    (void)DeleteRegistryTreeRelative(HKEY_CURRENT_USER, key);
            }
        }

        for (const wchar_t* clsid :
            {GlassShared::GlassShellMenu::kToggleExplorerClsidBraced,
                GlassShared::GlassShellMenu::kOpenConfigExplorerClsidBraced})
        {
            if (SUCCEEDED(::StringCchPrintfW(key, _countof(key), L"Software\\Classes\\CLSID\\%s", clsid)))
                (void)DeleteRegistryTreeRelative(HKEY_CURRENT_USER, key);
        }
    }

    bool IsLegacyContextMenuHandlerPresent() noexcept
    {
        return IsOurLegacyHandlerOnRoot(L"DesktopBackground") ||
               IsOurLegacyHandlerOnRoot(L"Directory\\Background");
    }

    bool TryGetLegacyContextMenuHandlerModule(wchar_t* buf, size_t cchChars) noexcept
    {
        if (!buf || cchChars == 0)
            return false;
        buf[0] = L'\0';

        wchar_t key[384]{};
        if (FAILED(::StringCchPrintfW(key, _countof(key), L"Software\\Classes\\CLSID\\%s\\InprocServer32",
                GlassShared::GlassShellMenu::kClsidStringBraced)))
            return false;

        DWORD cb = static_cast<DWORD>(cchChars * sizeof(wchar_t));
        const LSTATUS st = ::RegGetValueW(HKEY_CURRENT_USER, key, nullptr, RRF_RT_REG_SZ, nullptr, buf, &cb);
        if (st != ERROR_SUCCESS)
        {
            buf[0] = L'\0';
            return false;
        }
        return buf[0] != L'\0';
    }
}
