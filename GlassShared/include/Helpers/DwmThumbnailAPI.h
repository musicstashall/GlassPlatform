#pragma once
namespace Shared::DwmThumbnailAPI
{
    enum class THUMBNAIL_TYPE
    {
        TT_DEFAULT,
        TT_SNAPSHOT,
        TT_ICONIC,
        TT_BITMAPPENDING,
        TT_BITMAP
    };

    constexpr UINT DWM_TNP_FREEZE = 0x100000;
    constexpr UINT DWM_TNP_ENABLE3D = 0x4000000;
    constexpr UINT DWM_TNP_DISABLE3D = 0x8000000;
    constexpr UINT DWM_TNP_FORCECVI = 0x40000000;
    constexpr UINT DWM_TNP_DISABLEFORCECVI = 0x80000000;

    struct DWM_COLORIZATION_PARAMETERS
    {
        DWORD Color;
        DWORD AfterGlow;
        UINT ColorBalance;
        UINT AfterGlowBalance;
        UINT BlurBalance;
        UINT GlassReflectionIntensity;
        UINT OpaqueBlend;
    };

    inline HRESULT(WINAPI* DwmpQueryThumbnailType)(HTHUMBNAIL, THUMBNAIL_TYPE*) = nullptr;
    inline HRESULT(WINAPI* DwmGetColorizationParameters)(DWM_COLORIZATION_PARAMETERS*) = nullptr;
    inline HRESULT(WINAPI* DwmSetColorizationParameters)(DWM_COLORIZATION_PARAMETERS*) = nullptr;
    inline HRESULT(WINAPI* DwmpCreateSharedThumbnailVisual)(
        HWND hwndDestination,
        HWND hwndSource,
        DWORD dwThumbnailFlags,
        DWM_THUMBNAIL_PROPERTIES* pThumbnailProperties,
        void* pDCompDevice,
        void** ppVisual,
        HTHUMBNAIL* phThumbnailId
        ) = nullptr;
    inline HRESULT(WINAPI* DwmpQueryWindowThumbnailSourceSize)(
        HWND hwndSource,
        BOOL fSourceClientAreaOnly,
        SIZE* pSize
        ) = nullptr;

    // For older builds (pre-iron)
    inline HRESULT(WINAPI* DwmpCreateSharedVirtualDesktopVisual)(
        HWND hwndDestination,
        void* pDCompDevice,
        void** ppVisual,
        HTHUMBNAIL* phThumbnailId
        ) = nullptr;
    inline HRESULT(WINAPI* DwmpUpdateSharedVirtualDesktopVisual)(
        HTHUMBNAIL hThumbnailId,
        HWND* phwndsInclude,
        DWORD chwndsInclude,
        HWND* phwndsExclude,
        DWORD chwndsExclude,
        RECT* prcSource,
        SIZE* pDestinationSize
        ) = nullptr;

    // For 20xxx+ (newer builds)
    inline HRESULT(WINAPI* DwmpCreateSharedMultiWindowVisual)(
        HWND hwndDestination,
        void* pDCompDevice,
        void** ppVisual,
        HTHUMBNAIL* phThumbnailId
        ) = nullptr;
    inline HRESULT(WINAPI* DwmpUpdateSharedMultiWindowVisual)(
        HTHUMBNAIL hThumbnailId,
        HWND* phwndsInclude,
        DWORD chwndsInclude,
        HWND* phwndsExclude,
        DWORD chwndsExclude,
        RECT* prcSource,
        SIZE* pDestinationSize,
        DWORD dwFlags
        ) = nullptr;

    inline HRESULT Initialize() noexcept
    {
        try
        {
            HMODULE dwmModule = GetModuleHandleW(L"dwmapi.dll");
            THROW_LAST_ERROR_IF_NULL(dwmModule);

            DwmpQueryThumbnailType = reinterpret_cast<decltype(DwmpQueryThumbnailType)>(
                GetProcAddress(dwmModule, MAKEINTRESOURCEA(114)));
            THROW_LAST_ERROR_IF_NULL(DwmpQueryThumbnailType);

            DwmGetColorizationParameters = reinterpret_cast<decltype(DwmGetColorizationParameters)>(
                GetProcAddress(dwmModule, MAKEINTRESOURCEA(127)));
            THROW_LAST_ERROR_IF_NULL(DwmGetColorizationParameters);

            DwmSetColorizationParameters = reinterpret_cast<decltype(DwmSetColorizationParameters)>(
                GetProcAddress(dwmModule, MAKEINTRESOURCEA(131)));
            THROW_LAST_ERROR_IF_NULL(DwmSetColorizationParameters);

            DwmpCreateSharedThumbnailVisual = reinterpret_cast<decltype(DwmpCreateSharedThumbnailVisual)>(
                GetProcAddress(dwmModule, MAKEINTRESOURCEA(147)));
            THROW_LAST_ERROR_IF_NULL(DwmpCreateSharedThumbnailVisual);

            DwmpQueryWindowThumbnailSourceSize = reinterpret_cast<decltype(DwmpQueryWindowThumbnailSourceSize)>(
                GetProcAddress(dwmModule, MAKEINTRESOURCEA(162)));
            THROW_LAST_ERROR_IF_NULL(DwmpQueryWindowThumbnailSourceSize);

            // Both virtual desktop and multi-window share ordinal 163 for create
            DwmpCreateSharedVirtualDesktopVisual = reinterpret_cast<decltype(DwmpCreateSharedVirtualDesktopVisual)>(
                GetProcAddress(dwmModule, MAKEINTRESOURCEA(163)));
            THROW_LAST_ERROR_IF_NULL(DwmpCreateSharedVirtualDesktopVisual);
            DwmpCreateSharedMultiWindowVisual = DwmpCreateSharedVirtualDesktopVisual;

            // Update ordinal 164
            DwmpUpdateSharedVirtualDesktopVisual = reinterpret_cast<decltype(DwmpUpdateSharedVirtualDesktopVisual)>(
                GetProcAddress(dwmModule, MAKEINTRESOURCEA(164)));
            THROW_LAST_ERROR_IF_NULL(DwmpUpdateSharedVirtualDesktopVisual);
            DwmpUpdateSharedMultiWindowVisual = reinterpret_cast<decltype(DwmpUpdateSharedMultiWindowVisual)>(
                DwmpUpdateSharedVirtualDesktopVisual);

            return S_OK;
        }
        CATCH_RETURN()
    }
}