#pragma once

#include <Windows.h>

extern "C"
{
    __declspec(dllexport) HRESULT WINAPI GlassSettings_TryBuildSymbolModuleDownloadInfo(
        const wchar_t* modulePath,
        wchar_t* pdbFileName,
        DWORD cchPdbFileName,
        wchar_t* symbolIdentifier,
        DWORD cchSymbolIdentifier,
        wchar_t* downloadUrl,
        DWORD cchDownloadUrl) noexcept;
}
