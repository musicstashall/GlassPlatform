#pragma once

#include <Windows.h>

// Маска granular reload для GlassGuiThread (GlassCompat) и GlassSettings live-update.
inline constexpr wchar_t kReloadRenderFlagsMappingName[] = L"Local\\GlassPlatform.ReloadRenderFlags";
// Синхронизация reload (дублирует HostPidHelper.h; GlassShared не зависит от GlassCompat).
inline constexpr wchar_t kReloadRenderSemaphoreName[] = L"GlassPlatform.ReloadRenderSemaphore";
inline constexpr wchar_t kReloadGenerationMappingName[] = L"Local\\GlassPlatform.ReloadGeneration";
/// lParam live-post для Root composition: kGlassLiveRootScope* (GlassMessages.h).
inline constexpr wchar_t kReloadRootLiveScopeMappingName[] = L"Local\\GlassPlatform.ReloadRootLiveScope";

/// ThemePaint + SyncExplorerFrameBackdropHook (без process-wide ReapplyOpenRootBackdrops).
inline constexpr DWORD kReloadRenderFlagGlassCore = 0x00000001u;
inline constexpr DWORD kReloadRenderFlagTaskbarSwca = 0x00000002u;
inline constexpr DWORD kReloadRenderFlagTaskbarXaml = 0x00000004u;
/// Process-wide sweep g_rootWindows / SnapshotGlassAppliedRootHwnds (тяжёлый; отдельно от live broadcast).
inline constexpr DWORD kReloadRenderFlagRootBackdropReapply = 0x00000008u;
inline constexpr DWORD kReloadRenderFlagTaskbarBoth =
    kReloadRenderFlagTaskbarSwca | kReloadRenderFlagTaskbarXaml;
inline constexpr DWORD kReloadRenderFlagAll =
    kReloadRenderFlagGlassCore | kReloadRenderFlagTaskbarSwca | kReloadRenderFlagTaskbarXaml
    | kReloadRenderFlagRootBackdropReapply;
/// OK / Применить / teardown GlassSettings: live уже дал COLORIZATION + broadcast; без повторного REAPPLY sweep.
inline constexpr DWORD kReloadRenderFlagGlassSettingsCommit =
    kReloadRenderFlagGlassCore | kReloadRenderFlagTaskbarSwca | kReloadRenderFlagTaskbarXaml;
/// Перечитать AttachList в инжектированных процессах (GlassCompat_ReloadAttachList).
inline constexpr DWORD kReloadRenderFlagAttachList = 0x00000010u;
/// Live Root backdrop: PostMessage WM_GLASS_REAPPLY_ROOT_BACKDROP по g_rootWindows.
inline constexpr DWORD kReloadRenderFlagRootLiveBackdropAll = 0x00000020u;
/// Live Root backdrop: только SnapshotGlassAppliedRootHwnds.
inline constexpr DWORD kReloadRenderFlagRootLiveBackdropApplied = 0x00000080u;
/// Live DrawTheme alpha: soft-invalidate chrome по g_rootWindows.
inline constexpr DWORD kReloadRenderFlagRootLiveThemePaint = 0x00000040u;
/// Live Root composition: PostMessage WM_DWMCOLORIZATIONCHANGED (wParam=0, lParam=scope)
/// по g_rootWindows ∩ DidGlassApplyRootBackdrop (SetProp).
inline constexpr DWORD kReloadRenderFlagRootLiveColorizationApplied = 0x00000100u;
/// Перечитать GlobalTextGlow из реестра (GlassCore_ReloadRenderTextInkFromRegistry).
inline constexpr DWORD kReloadRenderFlagRenderText = 0x00000200u;
/// Visual style / ImmersiveColorSet: UICompat force re-brush без ClearValue-Recapture.
/// Не ставить на GS commit / обычный TaskbarXaml — только host theme debounce.
inline constexpr DWORD kReloadRenderFlagUiThemeBrushRefresh = 0x00000400u;
