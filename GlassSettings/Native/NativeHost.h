#pragma once

#include <Windows.h>
#include <Core/TextPreviewExport.h>

namespace GlassSettingsNative
{
    [[nodiscard]] bool LoadGlassCoreDll() noexcept;
    [[nodiscard]] bool EnsureGlassCoreRuntime() noexcept;
    [[nodiscard]] bool IsGlassRuntimeLoaded() noexcept;
    void ShutdownGlassRuntime() noexcept;
    [[nodiscard]] bool CanShowTextVisualTuningDialog() noexcept;
}

extern "C"
{
    __declspec(dllexport) BOOL WINAPI GlassSettings_LoadGlassCoreDll() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_EnsureGlassCoreRuntime() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_IsGlassRuntimeLoaded() noexcept;
    __declspec(dllexport) void WINAPI GlassSettings_ReapplyOpenRootBackdropsIfEngineLoaded() noexcept;
    __declspec(dllexport) void WINAPI GlassSettings_ResetReapplyFingerprintIfEngineLoaded() noexcept;
    __declspec(dllexport) void WINAPI GlassSettings_PostLiveRootBackdropReapplyAllIfEngineLoaded() noexcept;
    __declspec(dllexport) void WINAPI GlassSettings_PostLiveRootBackdropReapplyAppliedIfEngineLoaded() noexcept;
    __declspec(dllexport) void WINAPI GlassSettings_InvalidateThemedChromeOnTrackedRootsIfEngineLoaded() noexcept;
    __declspec(dllexport) void WINAPI GlassSettings_InvalidateTextGlowClientsOnTrackedRootsIfEngineLoaded() noexcept;
    __declspec(dllexport) void WINAPI GlassSettings_PostLiveRootColorizationToAppliedRootsIfEngineLoaded(
        LPARAM liveRootScope) noexcept;
    __declspec(dllexport) void WINAPI GlassSettings_ReloadRenderTextInkFromRegistryIfEngineLoaded() noexcept;
    __declspec(dllexport) void WINAPI GlassSettings_SetRenderTextPreviewGlobalGlow(
        const void* data, size_t byteCount) noexcept;
    __declspec(dllexport) HRESULT WINAPI GlassSettings_PaintTextPreviewToBuffer(
        BYTE* bgraPremul,
        int width,
        int height,
        int stride,
        const GlassCoreTextPreviewRequest* request,
        BOOL interactiveDuringGesture) noexcept;
    __declspec(dllexport) void WINAPI GlassSettings_FillMessageLogFont(LOGFONTW* outFont) noexcept;
    __declspec(dllexport) void WINAPI GlassSettings_FillPreviewLogFont(LOGFONTW* outFont, int pointSize) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_LoadSystemFontsSpiCache() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_ApplySystemFontsSpi(
        const BOOL workCustom[6],
        const BYTE* workLogFonts,
        DWORD workLogFontsBytes,
        const BYTE* refLogFonts,
        DWORD refLogFontsBytes,
        UINT flags,
        int* outMenuLfHeight) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_TryCopyCachedNonClientMetrics(BYTE* buffer, DWORD bufferBytes) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_TryCopyCachedIconLogFont(BYTE* buffer, DWORD bufferBytes) noexcept;
    __declspec(dllexport) void WINAPI GlassSettings_ShutdownGlassRuntime() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_PickExecutableFile(
        HWND owner, wchar_t* pathBuffer, DWORD cchPath) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_PickImageFile(
        HWND owner, wchar_t* pathBuffer, DWORD cchPath) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_PickFolder(
        HWND owner, wchar_t* pathBuffer, DWORD cchPath) noexcept;

    __declspec(dllexport) BOOL WINAPI GlassSettings_CanShowTextVisualTuningDialog() noexcept;

    __declspec(dllexport) int WINAPI GlassSettings_QueryOpenGlassHostUiState() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_LegacyHostTaskExists() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_TryStartOpenGlassHostNoWait() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_TryLaunchOpenGlassStartup() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_TryLaunchOpenGlassShutdown() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_SetLegacyHostTaskSchedulerEnabled(BOOL enable) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_RemoveLegacyHostTaskCompletely() noexcept;
    __declspec(dllexport) int WINAPI GlassSettings_QueryGlassAutorunUiState() noexcept;
    __declspec(dllexport) int WINAPI GlassSettings_QueryGlassAutorunUiStateForArch(int arch) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_GlassAutorunTaskExists() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_GlassAutorunTaskExistsForArch(int arch) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_SignalPlatformStop() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_SignalPlatformStopForArch(int arch) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_WaitForPlatformStopped(DWORD timeoutMs) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_WaitForPlatformStoppedForArch(int arch, DWORD timeoutMs) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_StopOrphanAutorunTaskInstanceForArch(int arch) noexcept;
    __declspec(dllexport) void WINAPI GlassSettings_StartPlatform() noexcept;
    __declspec(dllexport) void WINAPI GlassSettings_StartPlatformForArch(int arch) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_SetGlassAutorunTaskSchedulerEnabled(BOOL enable) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_SetGlassAutorunTaskSchedulerEnabledForArch(int arch, BOOL enable) noexcept;
    /// Обновить Local\\GlassPlatform.TrayXamlPolicy.v3 до SignalReloadRender (UICompat / AppContainer).
    __declspec(dllexport) BOOL WINAPI GlassSettings_PublishTrayXamlPolicy() noexcept;

    __declspec(dllexport) BOOL WINAPI GlassSettings_BeginShellExtensionChange(BOOL registerNotUnregister) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_IsShellExtensionOperationInProgress() noexcept;
    __declspec(dllexport) DWORD WINAPI GlassSettings_GetShellExtensionLastOpMessageId() noexcept;
    __declspec(dllexport) DWORD WINAPI GlassSettings_CopyShellExtensionLastErrorMessage(
        wchar_t* buffer, DWORD cch) noexcept;
    __declspec(dllexport) void WINAPI GlassSettings_NotifyShellCachesAfterRegistrationChange() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_TryReadPresenceSnapshot(
        void* snapshotOut) noexcept;
    __declspec(dllexport) HANDLE WINAPI GlassSettings_OpenPresenceChangedEvent() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_IsInjectMapAvailable() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_TryReadInjectMapSlots(
        void* outSlots, DWORD maxSlots, DWORD* outCount) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_ClearInjectMapPidForArch(
        DWORD arch,
        DWORD pid) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_TryReadInjectMapSlotsForArch(
        DWORD arch, void* outSlots, DWORD maxSlots, DWORD* outCount) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_RequestProcessUnload(DWORD pid) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_ForceUnloadProcess(DWORD pid) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_HasShellInjectResiduals() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_HasShellInjectResidualsForArch(int arch) noexcept;
    /// 0=Idle, 1=Running, 2=Stopping, 3=Residual (GlassPlatformTask::InjectLifecycleUi).
    __declspec(dllexport) int WINAPI GlassSettings_QueryInjectLifecycleUiForArch(int arch) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_RefreshInjectResidualStickyForArch(int arch) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_ScanGlassModules(
        const DWORD* pids,
        DWORD pidsCount,
        void* outResults,
        DWORD maxResults,
        DWORD* outCount) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_IsOpenGlassNotificationWindowPresent() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_IsOpenGlassBackdropPipelineActive() noexcept;
    /// Pipeline/модуль OG или служба/задача — можно писать Composition/DWM policy.
    __declspec(dllexport) BOOL WINAPI GlassSettings_ShouldApplyDwmCompositionPolicy() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_IsOpenGlassHostProcessRunning() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_OpenGlassOrchestrationInstalled() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_TryGetOpenGlassServiceBinPath(
        wchar_t* buffer, DWORD cchBuffer) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_IsOpenGlassServiceRunning() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_IsOpenGlassServiceAutostartEnabled() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_SetOpenGlassServiceAutostartEnabled(BOOL enable) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_InstallOpenGlassService(const wchar_t* exePath) noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_StartOpenGlassService() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_StopOpenGlassService() noexcept;
    __declspec(dllexport) BOOL WINAPI GlassSettings_RemoveOpenGlassService() noexcept;

    __declspec(dllexport) HRESULT WINAPI GlassSettings_TryBuildSymbolModuleDownloadInfo(
        const wchar_t* modulePath,
        wchar_t* pdbFileName,
        DWORD cchPdbFileName,
        wchar_t* symbolIdentifier,
        DWORD cchSymbolIdentifier,
        wchar_t* downloadUrl,
        DWORD cchDownloadUrl) noexcept;

    // applyVisualStyle != 0: uxtheme #65 (flags: 0 без шрифтов, 65 со шрифтами).
    // applyDwmPolicy != 0: OpenGlass DWM policy (no-op без OG). При живом GlassCompat host — обычно false.
    __declspec(dllexport) HRESULT WINAPI GlassSettings_ApplyVisualStyleAndDwmPolicy(
        const wchar_t* msstylesPath,
        const wchar_t* colorStyle,
        const wchar_t* size,
        int flags,
        BOOL applyVisualStyle,
        BOOL applyDwmPolicy) noexcept;

    __declspec(dllexport) void WINAPI GlassSettings_ApplyDwmPolicyOnVisualStyleChange() noexcept;
}
