#include <windows.h>
#include <msi.h>
#include <SecureUxThemeHookRegistry.h>

extern "C" UINT __stdcall SecureUxThemeCaInstall(MSIHANDLE) noexcept {
    return Shared::SecureUxThemeHook::Install() ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
}

extern "C" UINT __stdcall SecureUxThemeCaUninstall(MSIHANDLE) noexcept {
    return Shared::SecureUxThemeHook::Uninstall() ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) noexcept { return TRUE; }
