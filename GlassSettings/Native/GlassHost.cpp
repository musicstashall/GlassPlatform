#include "NativeHost.h"

#include <Core/TextPreviewExport.h>
#include <DebugLog.h>
#include <GlassLogClient.h>
#include <GlassInkAlphaCapsule.h>
#include <GlassPlatformTask.h>
#include <GlassModulePath.h>
#include <ReloadRenderFlags.h>

#include <commctrl.h>
#include <shobjidl.h>

#include <atomic>
#include <cstdio>
#include <string>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
    [[nodiscard]] bool CopyShellItemPath(IShellItem* item, wchar_t* pathBuffer, DWORD cchPath) noexcept
    {
        if (!item || !pathBuffer || cchPath == 0)
            return false;

        PWSTR path = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path)
            return false;

        const bool ok = wcscpy_s(pathBuffer, cchPath, path) == 0 && pathBuffer[0] != L'\0';
        ::CoTaskMemFree(path);
        return ok;
    }
}

namespace GlassSettingsNative
{
    namespace
    {
        HMODULE           g_hGlassCore = nullptr;
        std::atomic<bool> g_shutdownDone{ false };

        void EnsureHostCommonControlsV6() noexcept
        {
            static bool done = false;
            if (done)
                return;
            INITCOMMONCONTROLSEX icc{
                sizeof(icc),
                ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES,
            };
            (void)::InitCommonControlsEx(&icc);
            done = true;
        }

        [[nodiscard]] std::wstring DllPathNextToExe(const wchar_t* dllName) noexcept
        {
            const std::wstring resolved =
                GlassShared::GlassModulePath::ResolveGlassModulePathNextToModule(nullptr, dllName);
            if (!resolved.empty())
                return resolved;
            return dllName ? dllName : L"";
        }

        // Отдельная функция: __try нельзя смешивать с C++ unwind в LoadGlassCoreDll.
        [[nodiscard]] DWORD CallHostUiEnsureEngineSealed(void(WINAPI* pfn)()) noexcept
        {
            if (!pfn)
                return 0;
            DWORD sehCode = 0;
            __try
            {
                pfn();
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                sehCode = GetExceptionCode();
            }
            return sehCode;
        }

        // WINAPI + GlassCore.def (неукрашенное имя на x86).
        using PFN_PaintTextPreview = HRESULT(WINAPI*)(
            BYTE*, int, int, int, const GlassCoreTextPreviewRequest*, BOOL);

        [[nodiscard]] DWORD CallPaintTextPreviewSealed(
            PFN_PaintTextPreview pfn,
            BYTE* bgraPremul,
            int width,
            int height,
            int stride,
            const GlassCoreTextPreviewRequest* request,
            BOOL interactiveDuringGesture,
            HRESULT* outHr) noexcept
        {
            if (!pfn || !outHr)
                return 0;
            *outHr = E_FAIL;
            DWORD sehCode = 0;
            __try
            {
                *outHr = pfn(bgraPremul, width, height, stride, request, interactiveDuringGesture);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                sehCode = GetExceptionCode();
            }
            return sehCode;
        }

        void DrainPendingUiMessages(DWORD maxWaitMs) noexcept
        {
            const DWORD deadline = ::GetTickCount() + maxWaitMs;
            while (::GetTickCount() < deadline)
            {
                MSG msg{};
                while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                {
                    if (msg.message == WM_QUIT)
                    {
                        ::PostQuitMessage(static_cast<int>(msg.wParam));
                        return;
                    }
                    ::TranslateMessage(&msg);
                    ::DispatchMessageW(&msg);
                }
                ::Sleep(1);
            }
        }

        void DrainHostExitMessages() noexcept
        {
            MSG msg{};
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT)
                {
                    ::PostQuitMessage(static_cast<int>(msg.wParam));
                    return;
                }
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
        }

#if defined(_DEBUG)
        ULONGLONG g_shutdownTraceStartMs = 0;

        void ShutdownTrace(const wchar_t* phase) noexcept
        {
            const ULONGLONG now = ::GetTickCount64();
            if (g_shutdownTraceStartMs == 0)
                g_shutdownTraceStartMs = now;

            DebugLog(
                DBG_IMPORTANT,
                L"[GlassSettings][Shutdown] +%llu ms %s",
                now - g_shutdownTraceStartMs,
                phase);
        }
#else
        void ShutdownTrace(const wchar_t*) noexcept {}
#endif

        void StopHostLogClient() noexcept
        {
#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER)
            Shared::Log::GlassLogClient::Instance().Stop();
#endif
        }

        void RunShutdownBeforeFreeLibrary() noexcept
        {
            bool expected = false;
            if (!g_shutdownDone.compare_exchange_strong(expected, true))
                return;

#if defined(_DEBUG)
            g_shutdownTraceStartMs = 0;
#endif
            ShutdownTrace(L"RunShutdownBeforeFreeLibrary BEGIN");

            DrainHostExitMessages();
            (void)::SendNotifyMessageW(HWND_BROADCAST, WM_NULL, 0, 0);
            DrainHostExitMessages();
            ShutdownTrace(L"inter-step drain");

            if (g_hGlassCore)
            {
                using PFN = void(WINAPI*)();
                if (const auto pHostExit = reinterpret_cast<PFN>(
                        ::GetProcAddress(g_hGlassCore, "GlassCore_HostExitShutdown")))
                    pHostExit();
                else if (const auto pShutdown = reinterpret_cast<PFN>(
                             ::GetProcAddress(g_hGlassCore, "GlassCore_Shutdown")))
                    pShutdown();
            }
            ShutdownTrace(L"GlassCore host shutdown");

            StopHostLogClient();
            ShutdownTrace(L"StopHostLogClient");
        }

        void UnloadGlassCoreModule() noexcept
        {
            if (!g_hGlassCore)
                return;
            (void)::FreeLibrary(g_hGlassCore);
            g_hGlassCore = nullptr;
        }
    }

    bool LoadGlassCoreDll() noexcept
    {
        if (g_shutdownDone.load(std::memory_order_acquire))
            return false;

        if (g_hGlassCore)
            return true;
        EnsureHostCommonControlsV6();
        const std::wstring path = DllPathNextToExe(L"GlassCore.dll");
        g_hGlassCore = ::LoadLibraryW(path.c_str());
        if (!g_hGlassCore)
            g_hGlassCore = ::LoadLibraryW(L"GlassCore.dll");
        if (g_hGlassCore)
        {
            using PFN = void(WINAPI*)();
            if (const auto pEnsure = reinterpret_cast<PFN>(
                    ::GetProcAddress(g_hGlassCore, "GlassCore_HostUiEnsureEngine")))
            {
                // Как GlassCompat: SEH вокруг Ensure вне loader lock (DllMain UI-хостов не init'ит).
                const DWORD sehCode = CallHostUiEnsureEngineSealed(pEnsure);
            }
        }

        return g_hGlassCore != nullptr;
    }

    bool EnsureGlassCoreRuntime() noexcept
    {
        return LoadGlassCoreDll();
    }

    bool IsGlassRuntimeLoaded() noexcept
    {
        return g_hGlassCore != nullptr;
    }

    void ShutdownGlassRuntime() noexcept
    {
        RunShutdownBeforeFreeLibrary();
        ShutdownTrace(L"before FreeLibrary");
        UnloadGlassCoreModule();
        ShutdownTrace(L"RunShutdownBeforeFreeLibrary END");
    }

    void ReapplyOpenRootBackdropsIfEngineLoaded() noexcept
    {
        if (!LoadGlassCoreDll() || !g_hGlassCore)
            return;

        using PFN = void(WINAPI*)();
        const auto fn = reinterpret_cast<PFN>(
            ::GetProcAddress(g_hGlassCore, "GlassCore_ReapplyOpenRootBackdrops"));
        if (fn)
            fn();
    }

    void ResetReapplyFingerprintIfEngineLoaded() noexcept
    {
        if (!LoadGlassCoreDll() || !g_hGlassCore)
            return;

        using PFN = void(WINAPI*)();
        const auto fn = reinterpret_cast<PFN>(
            ::GetProcAddress(g_hGlassCore, "GlassCore_ResetReapplyFingerprint"));
        if (fn)
            fn();
    }

    void PostLiveRootBackdropReapplyIfEngineLoaded(DWORD scope, DWORD reloadFlag) noexcept
    {
        if (LoadGlassCoreDll() && g_hGlassCore)
        {
            using PFN = void(WINAPI*)(DWORD);
            const auto fn = reinterpret_cast<PFN>(
                ::GetProcAddress(g_hGlassCore, "GlassCore_PostLiveRootBackdropReapply"));
            if (fn)
                fn(scope);
        }

        if (GlassShared::GlassPlatformTask::IsGlassPlatformRunning())
            (void)GlassShared::GlassPlatformTask::SignalReloadRender(reloadFlag);
    }

    void InvalidateThemedChromeOnTrackedRootsIfEngineLoaded() noexcept
    {
        if (LoadGlassCoreDll() && g_hGlassCore)
        {
            using PFN = void(WINAPI*)();
            const auto fn = reinterpret_cast<PFN>(
                ::GetProcAddress(g_hGlassCore, "GlassCore_InvalidateThemedChromeOnTrackedRoots"));
            if (fn)
                fn();
        }

        if (GlassShared::GlassPlatformTask::IsGlassPlatformRunning())
        {
            (void)GlassShared::GlassPlatformTask::SignalReloadRender(
                kReloadRenderFlagRootLiveThemePaint);
        }
    }

    void InvalidateTextGlowClientsOnTrackedRootsIfEngineLoaded() noexcept
    {
        if (LoadGlassCoreDll() && g_hGlassCore)
        {
            using PFN = void(WINAPI*)();
            const auto fn = reinterpret_cast<PFN>(
                ::GetProcAddress(g_hGlassCore, "GlassCore_InvalidateTextGlowClientsOnTrackedRoots"));
            if (fn)
                fn();
        }
    }

    void PostLiveRootColorizationToAppliedRootsIfEngineLoaded(LPARAM liveRootScope) noexcept
    {
        if (LoadGlassCoreDll() && g_hGlassCore)
        {
            using PFN = void(WINAPI*)(LPARAM);
            const auto fn = reinterpret_cast<PFN>(
                ::GetProcAddress(g_hGlassCore, "GlassCore_PostLiveRootColorizationToAppliedRoots"));
            if (fn)
                fn(liveRootScope);
        }

        if (GlassShared::GlassPlatformTask::IsGlassPlatformRunning())
        {
            GlassShared::GlassPlatformTask::WriteReloadRootLiveScope(liveRootScope);
            (void)GlassShared::GlassPlatformTask::SignalReloadRender(
                kReloadRenderFlagRootLiveColorizationApplied);
        }
    }

    void ReloadRenderTextInkFromRegistryIfEngineLoaded() noexcept
    {
        if (!LoadGlassCoreDll() || !g_hGlassCore)
            return;

        using PFN = void(WINAPI*)();
        const auto fn = reinterpret_cast<PFN>(
            ::GetProcAddress(g_hGlassCore, "GlassCore_ReloadRenderTextInkFromRegistry"));
        if (fn)
            fn();
    }

    void SetRenderTextPreviewGlobalGlowIfEngineLoaded(const void* data, size_t byteCount) noexcept
    {
        if (!data || byteCount < sizeof(Shared::InkCapsule::GlobalTextGlow))
            return;
        if (!LoadGlassCoreDll() || !g_hGlassCore)
            return;

        using PFN = void(WINAPI*)(const void*, size_t);
        const auto fn = reinterpret_cast<PFN>(
            ::GetProcAddress(g_hGlassCore, "GlassCore_SetRenderTextPreviewGlobalGlow"));
        if (fn)
            fn(data, byteCount);
    }

    HRESULT PaintTextPreviewToBufferIfEngineLoaded(
        BYTE* bgraPremul,
        int   width,
        int   height,
        int   stride,
        const GlassCoreTextPreviewRequest* request,
        BOOL  interactiveDuringGesture) noexcept
    {
        if (!bgraPremul || !request)
            return E_INVALIDARG;
        if (!LoadGlassCoreDll() || !g_hGlassCore)
            return E_FAIL;

        // Как CanShowTextVisualTuningDialog: Ensure до paint (DllMain UI-хостов не init'ит Engine).
        using PFNEnsure = void(WINAPI*)();
        if (const auto ensure = reinterpret_cast<PFNEnsure>(
                ::GetProcAddress(g_hGlassCore, "GlassCore_HostUiEnsureEngine")))
        {
            const DWORD ensureSeh = CallHostUiEnsureEngineSealed(ensure);
            if (ensureSeh != 0)
                return E_FAIL;
        }

        using PFN = HRESULT(WINAPI*)(
            BYTE*, int, int, int, const GlassCoreTextPreviewRequest*, BOOL);
        const auto fn = reinterpret_cast<PFN>(
            ::GetProcAddress(g_hGlassCore, "GlassCore_PaintTextPreviewToBuffer"));
        if (!fn)
            return E_NOTIMPL;
        HRESULT hr = E_FAIL;
        const DWORD paintSeh = CallPaintTextPreviewSealed(
            fn, bgraPremul, width, height, stride, request, interactiveDuringGesture, &hr);
        if (paintSeh != 0)
            return E_FAIL;
        return hr;
    }

    bool CanShowTextVisualTuningDialog() noexcept
    {
        if (!LoadGlassCoreDll() || !g_hGlassCore)
            return false;

        if (!::GetProcAddress(g_hGlassCore, "GlassCore_PaintTextPreviewToBuffer"))
            return false;

        using PFNEnsure = void(WINAPI*)();
        if (const auto ensure = reinterpret_cast<PFNEnsure>(
                ::GetProcAddress(g_hGlassCore, "GlassCore_HostUiEnsureEngine")))
            ensure();

        return true;
    }
}

extern "C" BOOL WINAPI GlassSettings_LoadGlassCoreDll() noexcept
{
    return GlassSettingsNative::LoadGlassCoreDll() ? TRUE : FALSE;
}

extern "C" BOOL WINAPI GlassSettings_EnsureGlassCoreRuntime() noexcept
{
    return GlassSettingsNative::EnsureGlassCoreRuntime() ? TRUE : FALSE;
}

extern "C" BOOL WINAPI GlassSettings_IsGlassRuntimeLoaded() noexcept
{
    return GlassSettingsNative::IsGlassRuntimeLoaded() ? TRUE : FALSE;
}

extern "C" void WINAPI GlassSettings_ReapplyOpenRootBackdropsIfEngineLoaded() noexcept
{
    GlassSettingsNative::ReapplyOpenRootBackdropsIfEngineLoaded();
}

extern "C" void WINAPI GlassSettings_ResetReapplyFingerprintIfEngineLoaded() noexcept
{
    GlassSettingsNative::ResetReapplyFingerprintIfEngineLoaded();
}

extern "C" void WINAPI GlassSettings_PostLiveRootBackdropReapplyAllIfEngineLoaded() noexcept
{
    GlassSettingsNative::PostLiveRootBackdropReapplyIfEngineLoaded(
        0u,
        kReloadRenderFlagRootLiveBackdropAll);
}

extern "C" void WINAPI GlassSettings_PostLiveRootBackdropReapplyAppliedIfEngineLoaded() noexcept
{
    GlassSettingsNative::PostLiveRootBackdropReapplyIfEngineLoaded(
        1u,
        kReloadRenderFlagRootLiveBackdropApplied);
}

extern "C" void WINAPI GlassSettings_InvalidateThemedChromeOnTrackedRootsIfEngineLoaded() noexcept
{
    GlassSettingsNative::InvalidateThemedChromeOnTrackedRootsIfEngineLoaded();
}

extern "C" void WINAPI GlassSettings_InvalidateTextGlowClientsOnTrackedRootsIfEngineLoaded() noexcept
{
    GlassSettingsNative::InvalidateTextGlowClientsOnTrackedRootsIfEngineLoaded();
}

extern "C" void WINAPI GlassSettings_PostLiveRootColorizationToAppliedRootsIfEngineLoaded(
    const LPARAM liveRootScope) noexcept
{
    GlassSettingsNative::PostLiveRootColorizationToAppliedRootsIfEngineLoaded(liveRootScope);
}

extern "C" void WINAPI GlassSettings_ReloadRenderTextInkFromRegistryIfEngineLoaded() noexcept
{
    GlassSettingsNative::ReloadRenderTextInkFromRegistryIfEngineLoaded();
}

extern "C" void WINAPI GlassSettings_SetRenderTextPreviewGlobalGlow(
    const void* data, const size_t byteCount) noexcept
{
    GlassSettingsNative::SetRenderTextPreviewGlobalGlowIfEngineLoaded(data, byteCount);
}

extern "C" HRESULT WINAPI GlassSettings_PaintTextPreviewToBuffer(
    BYTE* bgraPremul,
    int   width,
    int   height,
    int   stride,
    const GlassCoreTextPreviewRequest* request,
    BOOL  interactiveDuringGesture) noexcept
{
    return GlassSettingsNative::PaintTextPreviewToBufferIfEngineLoaded(
        bgraPremul, width, height, stride, request, interactiveDuringGesture);
}

extern "C" void WINAPI GlassSettings_FillMessageLogFont(LOGFONTW* outFont) noexcept
{
    if (!outFont)
        return;

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    (void)::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    *outFont = ncm.lfMessageFont;
}

extern "C" void WINAPI GlassSettings_FillPreviewLogFont(LOGFONTW* outFont, int pointSize) noexcept
{
    if (!outFont)
        return;

    GlassSettings_FillMessageLogFont(outFont);
    if (pointSize <= 0)
        return;

    HDC hdc = ::GetDC(nullptr);
    if (!hdc)
        return;

    const int logPixelsY = ::GetDeviceCaps(hdc, LOGPIXELSY);
    ::ReleaseDC(nullptr, hdc);

    if (logPixelsY <= 0)
        return;

    outFont->lfHeight = -::MulDiv(pointSize, logPixelsY, 72);
    outFont->lfWidth = 0;
}

namespace SystemFontsSpi
{
    static const wchar_t kRegistrySubKey[] = L"Control Panel\\Desktop\\WindowMetrics";

    static const wchar_t* const kRegValueNames[6] = {
        L"CaptionFont",
        L"SmCaptionFont",
        L"MenuFont",
        L"StatusFont",
        L"MessageFont",
        L"IconFont",
    };

    static NONCLIENTMETRICSW g_cachedNcm{};
    static LOGFONTW         g_cachedIcon{};
    static BOOL             g_hasCachedNcm = FALSE;
    static BOOL             g_hasCachedIcon = FALSE;

    static BOOL WriteRegistryFonts(const LOGFONTW toApply[6]) noexcept
    {
        HKEY hWm = nullptr;
        if (::RegCreateKeyExW(
                HKEY_CURRENT_USER,
                kRegistrySubKey,
                0,
                nullptr,
                0,
                KEY_SET_VALUE,
                nullptr,
                &hWm,
                nullptr) != ERROR_SUCCESS)
        {
            return FALSE;
        }

        for (int i = 0; i < 6; ++i)
        {
            (void)::RegSetValueExW(
                hWm,
                kRegValueNames[i],
                0,
                REG_BINARY,
                reinterpret_cast<const BYTE*>(&toApply[i]),
                sizeof(LOGFONTW));
        }

        ::RegCloseKey(hWm);
        return TRUE;
    }
} // namespace SystemFontsSpi

extern "C" BOOL WINAPI GlassSettings_LoadSystemFontsSpiCache() noexcept
{
    using namespace SystemFontsSpi;

    g_cachedNcm = {};
    g_cachedNcm.cbSize = sizeof(g_cachedNcm);
    g_hasCachedNcm = ::SystemParametersInfoW(
        SPI_GETNONCLIENTMETRICS,
        sizeof(g_cachedNcm),
        &g_cachedNcm,
        0);

    g_cachedIcon = {};
    g_hasCachedIcon = ::SystemParametersInfoW(
        SPI_GETICONTITLELOGFONT,
        sizeof(g_cachedIcon),
        &g_cachedIcon,
        0);

    return g_hasCachedNcm;
}

extern "C" BOOL WINAPI GlassSettings_TryCopyCachedNonClientMetrics(BYTE* buffer, DWORD bufferBytes) noexcept
{
    using namespace SystemFontsSpi;

    if (!buffer || !g_hasCachedNcm || bufferBytes < sizeof(g_cachedNcm))
        return FALSE;

    memcpy(buffer, &g_cachedNcm, sizeof(g_cachedNcm));
    return TRUE;
}

extern "C" BOOL WINAPI GlassSettings_TryCopyCachedIconLogFont(BYTE* buffer, DWORD bufferBytes) noexcept
{
    using namespace SystemFontsSpi;

    if (!buffer || !g_hasCachedIcon || bufferBytes < sizeof(LOGFONTW))
        return FALSE;

    memcpy(buffer, &g_cachedIcon, sizeof(LOGFONTW));
    return TRUE;
}

/// Эталон TextSystemFontsPageApplyToSystemIfNeeded.
extern "C" BOOL WINAPI GlassSettings_ApplySystemFontsSpi(
    const BOOL workCustom[6],
    const BYTE* workLogFonts,
    DWORD workLogFontsBytes,
    const BYTE* refLogFonts,
    DWORD refLogFontsBytes,
    UINT flags,
    int* outMenuLfHeight) noexcept
{
    using namespace SystemFontsSpi;

    if (!workCustom || !workLogFonts || !refLogFonts)
        return FALSE;

    constexpr DWORD kFontBlockBytes = static_cast<DWORD>(sizeof(LOGFONTW) * 6);
    if (workLogFontsBytes < kFontBlockBytes || refLogFontsBytes < kFontBlockBytes)
        return FALSE;

    const LOGFONTW* const workLf = reinterpret_cast<const LOGFONTW*>(workLogFonts);
    const LOGFONTW* const refLf = reinterpret_cast<const LOGFONTW*>(refLogFonts);

    LOGFONTW toApply[6]{};
    for (int i = 0; i < 6; ++i)
        toApply[i] = workCustom[i] ? workLf[i] : refLf[i];

    (void)WriteRegistryFonts(toApply);

    (void)::SystemParametersInfoW(
        SPI_SETICONTITLELOGFONT,
        sizeof(toApply[5]),
        &toApply[5],
        flags);

    if (!g_hasCachedNcm)
    {
        g_cachedNcm = {};
        g_cachedNcm.cbSize = sizeof(g_cachedNcm);
        if (!::SystemParametersInfoW(
                SPI_GETNONCLIENTMETRICS,
                sizeof(g_cachedNcm),
                &g_cachedNcm,
                0))
        {
            return FALSE;
        }

        g_hasCachedNcm = TRUE;
    }

    NONCLIENTMETRICSW ncm = g_cachedNcm;
    ncm.cbSize = sizeof(ncm);
    ncm.lfCaptionFont = toApply[0];
    ncm.lfSmCaptionFont = toApply[1];
    ncm.lfMenuFont = toApply[2];
    ncm.lfStatusFont = toApply[3];
    ncm.lfMessageFont = toApply[4];

    const BOOL ok = ::SystemParametersInfoW(
        SPI_SETNONCLIENTMETRICS,
        sizeof(ncm),
        &ncm,
        flags);

    if (ok)
    {
        g_cachedNcm = ncm;
        if (outMenuLfHeight)
            *outMenuLfHeight = ncm.lfMenuFont.lfHeight;
    }

    return ok;
}

extern "C" void WINAPI GlassSettings_ShutdownGlassRuntime() noexcept
{
    GlassSettingsNative::ShutdownGlassRuntime();
}

extern "C" BOOL WINAPI GlassSettings_PickExecutableFile(
    HWND owner, wchar_t* pathBuffer, DWORD cchPath) noexcept
{
    if (!pathBuffer || cchPath < MAX_PATH)
        return FALSE;

    pathBuffer[0] = L'\0';

    const HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool    coOwned = hrCo == S_OK;

    IFileOpenDialog* dialog = nullptr;
    const HRESULT    hrCreate = ::CoCreateInstance(
        CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));

    if (FAILED(hrCreate) || !dialog)
    {
        if (coOwned)
            ::CoUninitialize();
        return FALSE;
    }

    static constexpr COMDLG_FILTERSPEC kFilters[] = {
        { L"Programs (*.exe;*.com;*.bat;*.cmd)", L"*.exe;*.com;*.bat;*.cmd" },
        { L"All files (*.*)", L"*.*" },
    };

    DWORD options = 0;
    (void)dialog->SetFileTypes(2, kFilters);
    (void)dialog->SetFileTypeIndex(1);
    (void)dialog->SetDefaultExtension(L"exe");
    if (SUCCEEDED(dialog->GetOptions(&options)))
    {
        (void)dialog->SetOptions(
            options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR
                | FOS_DONTADDTORECENT);
    }

    const HRESULT hrShow = dialog->Show(owner);

    bool picked = false;
    if (SUCCEEDED(hrShow))
    {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item)
        {
            picked = CopyShellItemPath(item, pathBuffer, cchPath);
            item->Release();
        }
    }

    dialog->Release();

    if (coOwned)
        ::CoUninitialize();

    return picked ? TRUE : FALSE;
}

extern "C" BOOL WINAPI GlassSettings_PickImageFile(
    HWND owner, wchar_t* pathBuffer, DWORD cchPath) noexcept
{
    if (!pathBuffer || cchPath < MAX_PATH)
        return FALSE;

    pathBuffer[0] = L'\0';

    const HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool    coOwned = hrCo == S_OK;

    IFileOpenDialog* dialog = nullptr;
    const HRESULT    hrCreate = ::CoCreateInstance(
        CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));

    if (FAILED(hrCreate) || !dialog)
    {
        if (coOwned)
            ::CoUninitialize();
        return FALSE;
    }

    static constexpr COMDLG_FILTERSPEC kFilters[] = {
        { L"Images (*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff;*.jfif;*.webp;*.heic)",
          L"*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff;*.jfif;*.webp;*.heic" },
        { L"All files (*.*)", L"*.*" },
    };

    DWORD options = 0;
    (void)dialog->SetFileTypes(2, kFilters);
    (void)dialog->SetFileTypeIndex(1);
    (void)dialog->SetDefaultExtension(L"jpg");
    if (SUCCEEDED(dialog->GetOptions(&options)))
    {
        (void)dialog->SetOptions(
            options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR
                | FOS_DONTADDTORECENT);
    }

    const HRESULT hrShow = dialog->Show(owner);

    bool picked = false;
    if (SUCCEEDED(hrShow))
    {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item)
        {
            picked = CopyShellItemPath(item, pathBuffer, cchPath);
            item->Release();
        }
    }

    dialog->Release();

    if (coOwned)
        ::CoUninitialize();

    return picked ? TRUE : FALSE;
}

extern "C" BOOL WINAPI GlassSettings_PickFolder(
    HWND owner, wchar_t* pathBuffer, DWORD cchPath) noexcept
{
    if (!pathBuffer || cchPath < MAX_PATH)
        return FALSE;

    pathBuffer[0] = L'\0';

    const HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool    coOwned = hrCo == S_OK;

    IFileOpenDialog* dialog = nullptr;
    const HRESULT    hrCreate = ::CoCreateInstance(
        CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));

    if (FAILED(hrCreate) || !dialog)
    {
        if (coOwned)
            ::CoUninitialize();
        return FALSE;
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options)))
    {
        (void)dialog->SetOptions(
            options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR
                | FOS_DONTADDTORECENT);
    }

    const HRESULT hrShow = dialog->Show(owner);

    bool picked = false;
    if (SUCCEEDED(hrShow))
    {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item)
        {
            picked = CopyShellItemPath(item, pathBuffer, cchPath);
            item->Release();
        }
    }

    dialog->Release();

    if (coOwned)
        ::CoUninitialize();

    return picked ? TRUE : FALSE;
}

extern "C" BOOL WINAPI GlassSettings_CanShowTextVisualTuningDialog() noexcept
{
    return GlassSettingsNative::CanShowTextVisualTuningDialog() ? TRUE : FALSE;
}
