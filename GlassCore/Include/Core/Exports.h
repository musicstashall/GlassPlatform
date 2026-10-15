#pragma once
#include <Windows.h>
#include <GlassCoreProviders.h>
#include <Core/TextPreviewExport.h>

extern "C" __declspec(dllexport)
void WINAPI GlassCore_Shutdown();

extern "C" __declspec(dllexport)
void WINAPI GlassCore_HostExitShutdown() noexcept;

// Единая точка получения всех провайдеров GlassCore.
// Заполняет структуру GlassCoreProviders и возвращает TRUE при успехе.
extern "C" __declspec(dllexport)
BOOL WINAPI GlassCore_GetProviders(GlassCoreProviders* out);

// Окно настроек: после записи GlassDrawThemeConstantAlpha в реестр — вызвать из процесса с загруженным GlassCore.
extern "C" __declspec(dllexport)
void WINAPI GlassCore_OnUserThemePaintSettingsChanged(void);

// GlassSettings: превью глобального glow (16 байт GlobalTextGlow).
extern "C" __declspec(dllexport)
void WINAPI GlassCore_SetRenderTextPreviewGlobalGlow(const void* data, size_t byteCount);

// Hero-превью текста GlassSettings (см. TextPreviewExport.h).
extern "C" __declspec(dllexport)
HRESULT WINAPI GlassCore_PaintTextPreviewToBuffer(
    BYTE* bgraPremul,
    int   width,
    int   height,
    int   stride,
    const GlassCoreTextPreviewRequest* request,
    BOOL  interactiveDuringGesture) noexcept;

// Повторно прочитать настройки текста из реестра (без RAM-превью). Для LogViewer и др. хостов без GlassSettings.
extern "C" __declspec(dllexport)
void WINAPI GlassCore_ReloadRenderTextInkFromRegistry(void);

// Переарм IAT-хуков текста с glow после reload GlobalTextGlow.
extern "C" __declspec(dllexport)
void WINAPI GlassCore_SyncTextGlowHooksFromRegistry(void);

// GlassSettings: превью альфы DrawTheme для Button / ComboBox / TabControl окна настроек (слайдер Root и т.п.).
extern "C" __declspec(dllexport)
void WINAPI GlassCore_SetThemePaintPreviewForGlassSettingsUi(BYTE alpha);

extern "C" __declspec(dllexport)
void WINAPI GlassCore_SetThemePaintPreviewPairForGlassSettingsUi(BYTE activeAlpha, BYTE inactiveAlpha);

// После LoadFromRegistry: значения из RAM движка (без превью).
extern "C" __declspec(dllexport)
void WINAPI GlassCore_GetThemePaintConstantAlphasFromEngine(BYTE* outActive, BYTE* outInactive);

extern "C" __declspec(dllexport)
void WINAPI GlassCore_ClearThemePaintPreview(void);

// Идемпотентно: Engine::Initialize (DllMain H64 + повтор из хостов). При AV — AbortPartialInit.
extern "C" __declspec(dllexport)
void WINAPI GlassCore_HostUiEnsureEngine(void);

// TRUE только после полного успешного Initialize (не после SEH-abort).
// x86: WINAPI + GlassCore.def — иначе GetProcAddress("…IsEngineReady") = null (старт платформы).
extern "C" __declspec(dllexport)
BOOL WINAPI GlassCore_IsEngineReady(void) noexcept;

// Устаревшее имя экспорта — то же, что GlassCore_HostUiEnsureEngine.
extern "C" __declspec(dllexport)
void WINAPI GlassCore_LogViewerEnsureEngine(void);

// GlassCompat: режим инъекции (Shared::InjectionMode) до первого Root_Attach.
extern "C" __declspec(dllexport)
void WINAPI GlassCore_SetInjectionMode(DWORD mode);

extern "C" __declspec(dllexport)
void WINAPI GlassCore_RegisterTrackedRootsBackdropApplier(void (WINAPI* fn)() noexcept) noexcept;
void WINAPI GlassCore_RegisterRootIsAttachedQuery(bool (WINAPI* fn)(HWND) noexcept) noexcept;
void WINAPI GlassCore_RegisterRootAttachRequest(void (WINAPI* fn)(HWND) noexcept) noexcept;

extern "C" __declspec(dllexport)
BOOL WINAPI GlassCore_IsExplorerAuxiliaryRootHwnd(HWND hRoot) noexcept;

extern "C" __declspec(dllexport)
BOOL WINAPI GlassCore_ShouldApplyRootSwcaOnShowWindow(HWND hRoot) noexcept;

extern "C" __declspec(dllexport)
void WINAPI GlassCore_RemoveRootSwcaApplyCacheEntry(HWND hRoot) noexcept;

extern "C" __declspec(dllexport)
void WINAPI GlassCore_ApplyRootBackdropPolicyTransitionForHwnd(HWND hRoot) noexcept;

extern "C" __declspec(dllexport)
void WINAPI GlassCore_ResetReapplyFingerprint() noexcept;

extern "C" __declspec(dllexport)
void WINAPI GlassCore_SetNextRootSwcaApplyReason(const wchar_t* reason) noexcept;

extern "C" __declspec(dllexport)
void WINAPI GlassCore_BeginExplorerShellChromeRestore() noexcept;

// ThemePaint: реестр chrome-HWND (Button/Combo/Tab) — WndCompat при attach/detach.
extern "C" __declspec(dllexport)
void WINAPI GlassCore_RegisterThemedChromeClient(HWND hControl) noexcept;

extern "C" __declspec(dllexport)
void WINAPI GlassCore_UnregisterThemedChromeClient(HWND hControl) noexcept;

extern "C" __declspec(dllexport)
void WINAPI GlassCore_RebindThemedChromeClientsAfterWinFormsWindowMarked(HWND hMarkedWinFormsWindow) noexcept;

extern "C" __declspec(dllexport)
void WINAPI GlassCore_RunDeferredThemedChromeSoftInvalidate(HWND hRoot) noexcept;

extern "C" __declspec(dllexport)
void WINAPI GlassCore_RunDeferredTextGlowSoftInvalidate(HWND hRoot) noexcept;

extern "C" __declspec(dllexport)
void WINAPI GlassCore_TryRegisterTextInkClient(HWND hControl) noexcept;

extern "C" __declspec(dllexport)
void WINAPI GlassCore_UnregisterTextInkClient(HWND hControl) noexcept;

extern "C" __declspec(dllexport)
void WINAPI GlassCore_RemoveTextInkClientsForRoot(HWND hRoot) noexcept;

extern "C" __declspec(dllexport)
void WINAPI GlassCore_RunDeferredTextInkSoftInvalidate(HWND hRoot) noexcept;

extern "C" __declspec(dllexport)
void WINAPI GlassCore_RemoveTextGlowClientsForRoot(HWND hRoot) noexcept;

extern "C" __declspec(dllexport)
void WINAPI GlassCore_TryRegisterTextGlowClient(HWND hControl) noexcept;

extern "C" __declspec(dllexport)
void WINAPI GlassCore_SetCtlColorBkColorProbeAttach(BOOL enable) noexcept;

extern "C" __declspec(dllexport)
void WINAPI GlassCore_RegisterTextGlowPopupClient(HWND hControl, BYTE category) noexcept;

extern "C" __declspec(dllexport)
void WINAPI GlassCore_UnregisterTextGlowClient(HWND hControl) noexcept;

extern "C" __declspec(dllexport)
void WINAPI GlassCore_TryRegisterMenuBarTextGlowOwner(HWND hOwner) noexcept;

extern "C" __declspec(dllexport)
void WINAPI GlassCore_RebindTextGlowClientsFromMarkedWindows() noexcept;

extern "C" __declspec(dllexport)
void WINAPI GlassCore_InvalidateTextGlowClientsOnTrackedRoots() noexcept;

extern "C" __declspec(dllexport)
void WINAPI GlassCore_InvalidateTextInkClientsOnTrackedRoots() noexcept;

// ExplorerCompat + XAML: колбэк при загрузке Microsoft/Windows.UI.Xaml (см. GlassCore::ModuleHandler).
extern "C" __declspec(dllexport)
void WINAPI GlassCore_SetXamlHostDllNotify(void(WINAPI* fn)(HMODULE loadedModule, const wchar_t* baseName)) noexcept;

// После SetXamlHostDllNotify — оповестить об уже загруженных XAML-модулях (тот же обход, что при Startup).
extern "C" __declspec(dllexport)
void WINAPI GlassCore_NotifyExistingXamlHostDlls() noexcept;