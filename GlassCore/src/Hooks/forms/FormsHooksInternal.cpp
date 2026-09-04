// Shared FormsHooks helpers (caller module, forms.ni).

#include "CorePch.h"

#include <Hooks/forms/FormsHooksInternal.h>
#include <WinApiUtils.h>

namespace GlassCore::FormsHooks::Internal
{
    HMODULE GetCallerModule(void* returnAddress) noexcept
    {
        HMODULE hMod = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(returnAddress),
            &hMod);
        return hMod;
    }

    bool IsSystemWindowsFormsNiModule(HMODULE mod) noexcept
    {
        if (!mod)
            return false;
        const std::wstring base = Shared::WinApiUtils::GetModuleFileBaseName(mod);
        return _wcsicmp(base.c_str(), L"System.Windows.Forms.ni.dll") == 0;
    }

    bool IsMFC42uModule(HMODULE mod) noexcept
    {
        if (!mod)
            return false;
        const std::wstring base = Shared::WinApiUtils::GetModuleFileBaseName(mod);
        return _wcsicmp(base.c_str(), L"MFC42u.dll") == 0;
    }

    bool IsMmcProcess() noexcept
    {
        static const bool isMmc = [] {
            wchar_t exeName[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exeName, MAX_PATH);
            const wchar_t* backslash = wcsrchr(exeName, L'\\');
            const wchar_t* name = backslash ? backslash + 1 : exeName;
            return _wcsicmp(name, L"mmc.exe") == 0;
        }();
        return isMmc;
    }
}
