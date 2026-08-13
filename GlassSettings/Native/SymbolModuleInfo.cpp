#include "SymbolModuleInfo.h"

#include <DbgHelp.h>
#include <string>
#include <wil/resource.h>

#pragma comment(lib, "DbgHelp.lib")

namespace
{
    constexpr wchar_t SymbolServerBase[] = L"https://msdl.microsoft.com/download/symbols/";
    constexpr DWORD MinPdbFileNameChars = 260;
    constexpr DWORD MinSymbolIdentifierChars = 64;
    constexpr DWORD MinDownloadUrlChars = 1024;

    struct PdbInfo
    {
        DWORD Signature;
        GUID Guid;
        DWORD Age;
        char PdbFileName[1];
    };

    struct ModuleDownloadInfo
    {
        std::wstring pdbFileName;
        std::wstring identifier;
        std::wstring url;
    };

    [[nodiscard]] HRESULT MultiByteToWideString(const char* source, std::wstring& destination) noexcept
    {
        if (!source)
            return E_INVALIDARG;

        const int required = MultiByteToWideChar(CP_ACP, 0, source, -1, nullptr, 0);
        if (required == 0)
            return HRESULT_FROM_WIN32(GetLastError());

        std::wstring buffer(static_cast<size_t>(required), L'\0');
        if (MultiByteToWideChar(CP_ACP, 0, source, -1, buffer.data(), required) == 0)
            return HRESULT_FROM_WIN32(GetLastError());

        buffer.resize(static_cast<size_t>(required - 1));
        destination = std::move(buffer);
        return S_OK;
    }

    [[nodiscard]] HRESULT CopyToBuffer(wchar_t* buffer, DWORD cchBuffer, const std::wstring& value) noexcept
    {
        if (!buffer || cchBuffer == 0)
            return E_INVALIDARG;

        if (value.length() + 1 > cchBuffer)
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);

        if (wcscpy_s(buffer, cchBuffer, value.c_str()) != 0)
            return E_FAIL;

        return S_OK;
    }

    [[nodiscard]] HRESULT BuildModuleDownloadInfo(
        const std::wstring& modulePath,
        ModuleDownloadInfo& info) noexcept
    {
        info = {};
        info.url = SymbolServerBase;

        wil::unique_hfile file{
            CreateFileW(
                modulePath.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr)
        };
        if (!file)
            return HRESULT_FROM_WIN32(GetLastError());

        wil::unique_handle fileMapping{
            CreateFileMappingW(file.get(), nullptr, PAGE_READONLY, 0, 0, nullptr)
        };
        if (!fileMapping)
            return HRESULT_FROM_WIN32(GetLastError());

        wil::unique_mapview_ptr<void> imageBase{
            MapViewOfFile(fileMapping.get(), FILE_MAP_READ, 0, 0, 0)
        };
        if (!imageBase)
            return HRESULT_FROM_WIN32(GetLastError());

        ULONG size = 0;
        const auto debugDirectory = reinterpret_cast<PIMAGE_DEBUG_DIRECTORY>(
            ImageDirectoryEntryToDataEx(
                imageBase.get(),
                FALSE,
                IMAGE_DIRECTORY_ENTRY_DEBUG,
                &size,
                nullptr));
        if (!debugDirectory || size < sizeof(IMAGE_DEBUG_DIRECTORY))
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

        for (ULONG index = 0; index < size / sizeof(IMAGE_DEBUG_DIRECTORY); ++index)
        {
            if (debugDirectory[index].Type != IMAGE_DEBUG_TYPE_CODEVIEW)
                continue;

            const auto pdbInfo = reinterpret_cast<PdbInfo*>(
                reinterpret_cast<ULONG_PTR>(imageBase.get()) + debugDirectory[index].PointerToRawData);
            if (constexpr DWORD rsds = 'SDSR'; *reinterpret_cast<DWORD const*>(&pdbInfo->Signature) != rsds)
                continue;

            const HRESULT nameHr = MultiByteToWideString(pdbInfo->PdbFileName, info.pdbFileName);
            if (FAILED(nameHr))
                return nameHr;

            WCHAR identifier[40]{};
            swprintf_s(
                identifier,
                L"%08lX%04hX%04hX%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX%x",
                pdbInfo->Guid.Data1,
                pdbInfo->Guid.Data2,
                pdbInfo->Guid.Data3,
                pdbInfo->Guid.Data4[0],
                pdbInfo->Guid.Data4[1],
                pdbInfo->Guid.Data4[2],
                pdbInfo->Guid.Data4[3],
                pdbInfo->Guid.Data4[4],
                pdbInfo->Guid.Data4[5],
                pdbInfo->Guid.Data4[6],
                pdbInfo->Guid.Data4[7],
                pdbInfo->Age);
            info.identifier = identifier;

            info.url += info.pdbFileName;
            info.url += L"/";
            info.url += info.identifier;
            info.url += L"/";
            info.url += info.pdbFileName;
            return S_OK;
        }

        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
}

extern "C" HRESULT WINAPI GlassSettings_TryBuildSymbolModuleDownloadInfo(
    const wchar_t* modulePath,
    wchar_t* pdbFileName,
    DWORD cchPdbFileName,
    wchar_t* symbolIdentifier,
    DWORD cchSymbolIdentifier,
    wchar_t* downloadUrl,
    DWORD cchDownloadUrl) noexcept
{
    if (!modulePath || !*modulePath
        || !pdbFileName || cchPdbFileName < MinPdbFileNameChars
        || !symbolIdentifier || cchSymbolIdentifier < MinSymbolIdentifierChars
        || !downloadUrl || cchDownloadUrl < MinDownloadUrlChars)
    {
        return E_INVALIDARG;
    }

    ModuleDownloadInfo info{};
    const HRESULT buildHr = BuildModuleDownloadInfo(modulePath, info);
    if (FAILED(buildHr))
        return buildHr;

    const HRESULT pdbHr = CopyToBuffer(pdbFileName, cchPdbFileName, info.pdbFileName);
    if (FAILED(pdbHr))
        return pdbHr;

    const HRESULT idHr = CopyToBuffer(symbolIdentifier, cchSymbolIdentifier, info.identifier);
    if (FAILED(idHr))
        return idHr;

    return CopyToBuffer(downloadUrl, cchDownloadUrl, info.url);
}
