// GlassCore/src/Core/Exports.cpp
#include "CorePch.h"
#include <cstdio>
#include "Subsystems/MenuEvents.h"
#include <Subsystems/FlyoutFrame.h>
#include <Hooks/DropDown.h>
#include <Hooks/ComboLBox.h>
#include <Hooks/MonthCal.h>
#include <Hooks/DirectUI.h>
#include <Hooks/Button.h>
#include <Hooks/Static.h>
#include <Hooks/Rebar.h>
#include <GlassCoreProviders.h>
#include <Core/Engine.h>
#include <Hooks/RootWindowHooks.h>
#include <Hooks/Tab.h>
#include <Hooks/ExplorerFrame.h>
#include <Hooks/UiRibbonHooks.h>
#include <Subsystems/Tooltip.h>
#include <Subsystems/ThemePaintRegistry.h>
#include <Subsystems/TextGlowClientRegistry.h>
#include <Subsystems/CtlColorProbe.h>
#include <Subsystems/TextInkClientRegistry.h>
#include <Subsystems/RenderText.h>
#include <Core/TextPreviewExport.h>
#include <Module/ModuleHandler.h>
// Root events: DWM backdrop + SWCA active/inactive + detach + colorization
namespace GlassCore::Root
{
    void OnRootDwmBackdropApply(HWND hRoot);
    void OnRootSwcaBackdropActiveApply(HWND hRoot);
    void OnRootSwcaBackdropInactiveApply(HWND hRoot);
    void OnRootDetach(HWND hRoot);
    void OnRootDwmColorizationChanged(HWND hRoot, WPARAM colorizationWave, LPARAM liveRootScope);
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_Shutdown()
{
    GlassCore::Engine::Shutdown();
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_HostExitShutdown() noexcept
{
    GlassCore::Engine::HostExitShutdown();
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_OnUserThemePaintSettingsChanged(void)
{
    GlassCore::ThemePaint::OnUserThemePaintSettingsChanged();
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_SetRenderTextPreviewGlobalGlow(const void* data, size_t byteCount)
{
    GlassCore::Text::SetRenderTextPreviewGlobalGlowBytes(data, byteCount);
}

extern "C" __declspec(dllexport)
HRESULT WINAPI GlassCore_PaintTextPreviewToBuffer(
    BYTE* bgraPremul,
    int   width,
    int   height,
    int   stride,
    const GlassCoreTextPreviewRequest* request,
    BOOL  interactiveDuringGesture) noexcept
{
    return GlassCore::Text::PaintTextPreviewToBuffer(
        bgraPremul,
        width,
        height,
        stride,
        request,
        interactiveDuringGesture != FALSE);
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_ReloadRenderTextInkFromRegistry(void)
{
    GlassCore::Text::ReloadRenderTextInkFromRegistry();
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_SyncTextGlowHooksFromRegistry(void)
{
    GlassCore::Text::SyncTextGlowHooksFromRegistry();
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_SetThemePaintPreviewForGlassSettingsUi(BYTE alpha)
{
    GlassCore::ThemePaint::SetThemePaintPreviewForGlassSettingsUi(static_cast<std::uint8_t>(alpha));
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_SetThemePaintPreviewPairForGlassSettingsUi(BYTE activeAlpha, BYTE inactiveAlpha)
{
    GlassCore::ThemePaint::SetThemePaintPreviewPairForGlassSettingsUi(
        static_cast<std::uint8_t>(activeAlpha),
        static_cast<std::uint8_t>(inactiveAlpha));
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_GetThemePaintConstantAlphasFromEngine(BYTE* outActive, BYTE* outInactive)
{
    if (!outActive || !outInactive)
        return;
    *outActive   = GlassCore::ThemePaint::GetDrawThemeConstantAlpha();
    *outInactive = GlassCore::ThemePaint::GetDrawThemeConstantAlphaInactive();
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_ClearThemePaintPreview(void)
{
    GlassCore::ThemePaint::ClearThemePaintPreviewAlpha();
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_RefreshCachedForegroundWindow(void)
{
    GlassCore::ThemePaint::RefreshCachedForegroundWindow();
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_FinalizeRootActivationVisualUpdate(HWND hRoot, BOOL ncActivateFromWmNcActivate)
{
    GlassCore::ThemePaint::FinalizeRootActivationVisualUpdate(
        hRoot,
        ncActivateFromWmNcActivate != FALSE);
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_RemoveThemePaintRootNcState(HWND hRoot)
{
    GlassCore::ThemePaint::RemoveThemePaintRootNcState(hRoot);
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_RegisterThemedChromeClient(HWND hControl) noexcept
{
    GlassCore::ThemePaint::RegisterThemedChromeClient(hControl);
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_UnregisterThemedChromeClient(HWND hControl) noexcept
{
    GlassCore::ThemePaint::UnregisterThemedChromeClient(hControl);
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_RebindThemedChromeClientsAfterWinFormsWindowMarked(HWND hMarkedWinFormsWindow) noexcept
{
    GlassCore::ThemePaint::RebindThemedChromeClientsAfterWinFormsWindowMarked(hMarkedWinFormsWindow);
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_RunDeferredThemedChromeSoftInvalidate(HWND hRoot) noexcept
{
    GlassCore::ThemePaint::RunDeferredThemedChromeSoftInvalidate(hRoot);
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_TryRegisterTextGlowClient(HWND hControl) noexcept
{
    GlassCore::TextGlow::TryRegisterTextGlowClient(hControl);
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_SetCtlColorBkColorProbeAttach(BOOL enable) noexcept
{
    GlassCore::CtlColorProbe::SetBkColorProbeAttach(enable != FALSE);
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_RegisterTextGlowPopupClient(HWND hControl, BYTE category) noexcept
{
    GlassCore::TextGlow::RegisterTextGlowPopupClient(
        hControl,
        static_cast<GlassCore::TextGlow::TextGlowSpreadCategory>(category));
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_UnregisterTextGlowClient(HWND hControl) noexcept
{
    GlassCore::TextGlow::UnregisterTextGlowClient(hControl);
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_TryRegisterMenuBarTextGlowOwner(HWND hOwner) noexcept
{
    GlassCore::TextGlow::TryRegisterMenuBarOwner(hOwner);
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_RebindTextGlowClientsFromMarkedWindows() noexcept
{
    GlassCore::TextGlow::RebindTextGlowClientsFromMarkedWindows();
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_RunDeferredTextGlowSoftInvalidate(HWND hRoot) noexcept
{
    GlassCore::TextGlow::RunDeferredTextGlowSoftInvalidate(hRoot);
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_TryRegisterTextInkClient(HWND hControl) noexcept
{
    GlassCore::TextInk::TryRegisterTextInkClient(hControl);
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_UnregisterTextInkClient(HWND hControl) noexcept
{
    GlassCore::TextInk::UnregisterTextInkClient(hControl);
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_RemoveTextInkClientsForRoot(HWND hRoot) noexcept
{
    GlassCore::TextInk::RemoveTextInkClientsForRoot(hRoot);
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_RunDeferredTextInkSoftInvalidate(HWND hRoot) noexcept
{
    GlassCore::TextInk::RunDeferredTextInkSoftInvalidate(hRoot);
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_RemoveTextGlowClientsForRoot(HWND hRoot) noexcept
{
    GlassCore::TextGlow::RemoveTextGlowClientsForRoot(hRoot);
}

namespace
{
    // __try нельзя рядом с C++ объектами — отдельная функция.
    bool EnsureEngineSealed(HMODULE hMod) noexcept
    {
        return GlassCore::Engine::InitializeSealed(hMod);
    }
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_HostUiEnsureEngine(void)
{
    HMODULE hMod = nullptr;
    (void)::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&GlassCore_HostUiEnsureEngine),
        &hMod);
    if (!hMod)
        hMod = ::GetModuleHandleW(L"GlassCore.dll");
    (void)EnsureEngineSealed(hMod);
}

extern "C" __declspec(dllexport)
BOOL WINAPI GlassCore_IsEngineReady(void) noexcept
{
    return GlassCore::Engine::IsReady() ? TRUE : FALSE;
}

extern "C" __declspec(dllexport)
void WINAPI GlassCore_LogViewerEnsureEngine(void)
{
    GlassCore_HostUiEnsureEngine();
}

// Хосты UI (GlassSettings, LogViewer, …): показ DropDown без гарантированного WM_SHOWWINDOW у субкласса — тот же путь, что и из DropDownSubclassProc.
extern "C" __declspec(dllexport)
void WINAPI GlassCore_ApplyDropDownShow(HWND hWndDropDown)
{
    GlassCore::DropDown::OnDropDownShow(hWndDropDown);
}

extern "C" __declspec(dllexport)
BOOL WINAPI GlassCore_GetProviders(GlassCoreProviders* out)
{
    if (!out)
        return FALSE;

    *out = {};

    // ─── Menu domain ────────────────────────────────────────────────────────
    out->menu.OnPopupCreated = &GlassCore::Menu::OnPopupCreated;
    out->menu.OnPopupShown = &GlassCore::Menu::OnPopupShown;
    out->menu.OnPopupSized = &GlassCore::Menu::OnPopupSized;
    out->menu.OnPopupDestroyed = &GlassCore::Menu::OnPopupDestroyed;
    out->menu.OnPaintFlyoutHostBorder = &GlassCore::Menu::OnPaintFlyoutHostBorder;
    out->menu.IsFlyoutBorderDeferred = &GlassCore::FlyoutFrame::IsHostBorderDeferred;
    out->menu.OnOwnerDrawItem = &GlassCore::Menu::OnOwnerDrawItem;
    out->menu.OnOwnerDrawBegin = &GlassCore::Menu::OnOwnerDrawBegin;
    out->menu.OnOwnerDrawEnd = &GlassCore::Menu::OnOwnerDrawEnd;
    out->menu.OnButtonUp = &GlassCore::Menu::OnButtonUp;
    out->menu.OnAttachHooks = &GlassCore::Menu::OnAttachHooks;
    out->menu.OnDetachHooks = &GlassCore::Menu::OnDetachHooks;
    out->menu.SyncMenuContextFromMenuHandler = &GlassCore::Menu::SyncMenuContextFromMenuHandler;
    out->menu.ApplyUaInitMenuNativeOrClassic = &GlassCore::Menu::ApplyUaInitMenuNativeOrClassic;
    out->menu.FinalizeNativeThemeAfterPrintWindowProbe = &GlassCore::Menu::FinalizeNativeThemeAfterPrintWindowProbe;
    out->menu.OnUxHooksBegin = &GlassCore::Menu::OnUxHooksBegin;
    out->menu.OnUxHooksEnd = &GlassCore::Menu::OnUxHooksEnd;
    out->menu.OnMenuBarUxThemeDrawBegin = &GlassCore::Menu::OnMenuBarUxThemeDrawBegin;
    out->menu.OnMenuBarUxThemeDrawEnd = &GlassCore::Menu::OnMenuBarUxThemeDrawEnd;
    out->menu.OnMenuBarClassicNcActivateBracketBegin = &GlassCore::Menu::OnMenuBarClassicNcActivateBracketBegin;
    out->menu.OnMenuBarClassicNcActivateBracketEnd = &GlassCore::Menu::OnMenuBarClassicNcActivateBracketEnd;
    out->menu.OnMenuBarClassicNcPaintBracketBegin = &GlassCore::Menu::OnMenuBarClassicNcPaintBracketBegin;
    out->menu.OnMenuBarClassicNcPaintBracketEnd = &GlassCore::Menu::OnMenuBarClassicNcPaintBracketEnd;
    out->menu.OnMenuBarClassicEtwHoldEnsure = &GlassCore::Menu::OnMenuBarClassicEtwHoldEnsure;
    out->menu.OnMenuBarClassicEtwHoldRelease = &GlassCore::Menu::OnMenuBarClassicEtwHoldRelease;
    out->menu.OnMenuBarClassicUahInitMenuBracketBegin = &GlassCore::Menu::OnMenuBarClassicUahInitMenuBracketBegin;
    out->menu.OnMenuBarClassicUahInitMenuBracketEnd = &GlassCore::Menu::OnMenuBarClassicUahInitMenuBracketEnd;
    out->menu.OnClassicMenuPopupPaintExtTextOutBegin = &GlassCore::Menu::OnClassicMenuPopupPaintExtTextOutBegin;
    out->menu.OnClassicMenuPopupPaintExtTextOutEnd = &GlassCore::Menu::OnClassicMenuPopupPaintExtTextOutEnd;
    out->menu.OnMenuBarBackdropPolicyDisabled = &GlassCore::Menu::ShutdownClassicMenuBarNcExtTextOut;

    // ─── DropDown domain ────────────────────────────────────────────────────
    out->dropdown.OnPaintBegin = &GlassCore::DropDown::OnPaintBegin;
    out->dropdown.OnPaintEnd = &GlassCore::DropDown::OnPaintEnd;
    out->dropdown.OnDrawItemBegin = &GlassCore::DropDown::OnDrawItemBegin;
    out->dropdown.OnDrawItemEnd = &GlassCore::DropDown::OnDrawItemEnd;
    out->dropdown.OnListviewPopupAttach = &GlassCore::DropDown::OnListviewPopupAttach;
    out->dropdown.OnViewControlAttach = &GlassCore::DropDown::OnViewControlAttach;
    // Toolbar: SubclassProc удалён, хуки теперь через PaintDispatcher (ToolbarHooks.cpp)
    // out->dropdown.OnToolbarPaintBegin/End больше не используются
    out->dropdown.OnDropDownShow = &GlassCore::DropDown::OnDropDownShow;
    out->dropdown.IsFlyoutBorderDeferred = &GlassCore::FlyoutFrame::IsHostBorderDeferred;
    out->dropdown.ClearFlyoutBorderDeferred = &GlassCore::DropDown::ClearFlyoutBorderDeferred;
    out->dropdown.QueryDropDownContext = &GlassCore::DropDown::QueryDropDownContext;
    out->dropdown.OnComboLBoxShow              = &GlassCore::ComboLBox::OnComboLBoxShow;
    out->dropdown.OnComboLBoxPrintClientBegin  = &GlassCore::ComboLBox::OnComboLBoxPrintClientBegin;
    out->dropdown.OnComboLBoxPrintClientEnd    = &GlassCore::ComboLBox::OnComboLBoxPrintClientEnd;
    out->dropdown.OnComboLBoxDetach            = &GlassCore::ComboLBox::OnComboLBoxDetach;
    out->dropdown.OnComboBoxDropDown           = &GlassCore::ComboLBox::OnComboBoxDropDown;

    // ─── Net UI Tool Window (ленточные popup; реализации в UiRibbonHooks.cpp) ─
    out->dropdown.OnNetUiToolWindowHooksAttach = &GlassCore::UiRibbon::OnNetUiToolWindowHooksAttach;
    out->dropdown.OnNetUiToolWindowAttach      = &GlassCore::UiRibbon::OnNetUiToolWindowAttach;
    out->dropdown.OnNetUiToolWindowDetach      = &GlassCore::UiRibbon::OnNetUiToolWindowDetach;

    // ─── MonthCal callbacks (через DropDown-провайдер) ──────────────────────
    out->dropdown.OnMonthCalAttach             = &GlassCore::MonthCal::OnMonthCalAttach;
    out->dropdown.OnMonthCalPrintClientBegin   = &GlassCore::MonthCal::OnMonthCalPrintClientBegin;
    out->dropdown.OnMonthCalPrintClientEnd     = &GlassCore::MonthCal::OnMonthCalPrintClientEnd;

    // ─── Root domain ─────────────────────────────────────────────────────────
    out->root.OnRootDwmBackdropApply = &GlassCore::Root::OnRootDwmBackdropApply;
    out->root.OnRootSwcaBackdropActiveApply = &GlassCore::Root::OnRootSwcaBackdropActiveApply;
    out->root.OnRootSwcaBackdropInactiveApply = &GlassCore::Root::OnRootSwcaBackdropInactiveApply;
    out->root.OnRootDetach = &GlassCore::Root::OnRootDetach;
    out->root.OnRootDwmColorizationChanged = &GlassCore::Root::OnRootDwmColorizationChanged;
    out->root.OnRootHooksAttach = &GlassCore::Root::OnRootHooksAttach;
    out->root.OnRootHooksDetach = &GlassCore::Root::OnRootHooksDetach;
    out->root.OnRootWmThemeChangedForTabChroma = &GlassCore::TabControl::OnRootWmThemeChangedForTabChroma;
    out->root.IsWin11ExplorerShellCabinetChromeHwnd =
        &GlassCore::ExplorerFrameControl::IsWin11ExplorerShellCabinetChromeHwnd;

    // ─── DirectUI domain ─────────────────────────────────────────────────────
    out->directui.OnDuiPaintBegin = &GlassCore::DirectUI::OnDuiPaintBegin;
    out->directui.OnDuiPaintEnd = &GlassCore::DirectUI::OnDuiPaintEnd;
    out->directui.OnDirectUIRootHooksAttach = &GlassCore::DirectUI::OnDirectUIRootHooksAttach;

    // ─── Rebar domain ──────────────────────────────────────────────────────
    out->rebar.OnPaintBegin = &GlassCore::RebarControl::OnPaintBegin;
    out->rebar.OnPaintEnd   = &GlassCore::RebarControl::OnPaintEnd;

    // ─── Tooltip / KbxLabel ───────────────────────────────────────────────────
    out->tooltip.IsTooltipIatReady = &GlassCore::Tooltip::IsTooltipIatReady;
    out->tooltip.IsTooltipCustomizationDisabled = &GlassCore::Tooltip::IsTooltipCustomizationDisabledForAttach;
    out->tooltip.OnTooltipSubclassAttached = &GlassCore::Tooltip::OnTooltipSubclassAttached;
    out->tooltip.OnTooltipSubclassDetached = &GlassCore::Tooltip::OnTooltipSubclassDetached;
    out->tooltip.OnTooltipPaintSessionBegin = &GlassCore::Tooltip::OnTooltipPaintSessionBegin;
    out->tooltip.OnTooltipPaintSessionEnd = &GlassCore::Tooltip::OnTooltipPaintSessionEnd;
    out->tooltip.OnKbxLabelSubclassAttached = &GlassCore::Tooltip::OnKbxLabelSubclassAttached;
    out->tooltip.OnKbxLabelSubclassDetached = &GlassCore::Tooltip::OnKbxLabelSubclassDetached;
    out->tooltip.OnKbxLabelPaintSessionBegin = &GlassCore::Tooltip::OnKbxLabelPaintSessionBegin;
    out->tooltip.OnKbxLabelPaintSessionEnd = &GlassCore::Tooltip::OnKbxLabelPaintSessionEnd;
    out->tooltip.OnSubsystemUpdate = &GlassCore::Tooltip::OnSubsystemUpdate;

    return TRUE;
}

extern "C" __declspec(dllexport) void WINAPI GlassCore_SetXamlHostDllNotify(
    void(WINAPI* fn)(HMODULE loadedModule, const wchar_t* baseName)) noexcept
{
    GlassCore::ModuleHandler::SetXamlHostDllNotify(
        reinterpret_cast<GlassCore::ModuleHandler::XamlHostDllNotifyFn>(fn));
}

extern "C" __declspec(dllexport) void WINAPI GlassCore_NotifyExistingXamlHostDlls() noexcept
{
    GlassCore::ModuleHandler::NotifyExistingXamlHostDlls();
}