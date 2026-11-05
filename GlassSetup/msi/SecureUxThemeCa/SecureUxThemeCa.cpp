#include <windows.h>
#include <msi.h>
#include <SecureUxThemeHookRegistry.h>

#ifdef _M_IX86
#pragma comment(linker, "/EXPORT:SecureUxThemeCaInstall=_SecureUxThemeCaInstall@4")
#pragma comment(linker, "/EXPORT:SecureUxThemeCaUninstall=_SecureUxThemeCaUninstall@4")
#else
#pragma comment(linker, "/EXPORT:SecureUxThemeCaInstall")
#pragma comment(linker, "/EXPORT:SecureUxThemeCaUninstall")
#endif

extern "C" UINT __stdcall SecureUxThemeCaInstall(MSIHANDLE) noexcept {
    return Shared::SecureUxThemeHook::Install() ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
}

extern "C" UINT __stdcall SecureUxThemeCaUninstall(MSIHANDLE) noexcept {
    return Shared::SecureUxThemeHook::Uninstall() ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) noexcept { return TRUE; }
