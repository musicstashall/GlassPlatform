#pragma once
#include <windows.h>

namespace WndCompat::Root
{
    struct RootEventProvider
    {
        /// DWM backdrop (DwmBlurBehindWindow / DwmExtendFrameIntoClientArea) — только в Root_Attach.
        void (*OnRootDwmBackdropApply)(HWND hRoot) = nullptr;

        /// SWCA backdrop активного окна: Root_Attach + WM_SHOWWINDOW(show) + WM_NCACTIVATE(activate).
        /// Цвет из ColorizationColorOverride → ColorizationColor → COLOR_WINDOW + альфа GlassOpacity.
        void (*OnRootSwcaBackdropActiveApply)(HWND hRoot) = nullptr;

        /// SWCA backdrop неактивного окна: WM_NCACTIVATE(deactivate).
        /// Цвет из ColorizationColorInactive → Override → Color → COLOR_WINDOW + альфа GlassOpacityInactive.
        void (*OnRootSwcaBackdropInactiveApply)(HWND hRoot) = nullptr;

        // Вызывается при детаче root-окна (cleanup)
        void (*OnRootDetach)(HWND hRoot) = nullptr;

        // IAT-хуки по сценариям BWM PaintHandler («окно»), привязка к Root_Attach / Root_Detach
        void (*OnRootHooksAttach)(HWND hRoot) = nullptr;
        void (*OnRootHooksDetach)(HWND hRoot) = nullptr;

        /// WM_THEMECHANGED: кэш TabChroma в GlassCore (GlassShared — без состояния).
        void (*OnRootWmThemeChangedForTabChroma)(HWND themeAnchorOrNull) = nullptr;

        /// WM_DWMCOLORIZATIONCHANGED: wParam — wave-id (DwmGlassOpacity::Notify).
        /// liveRootScope — lParam live-post (kGlassLiveRootScope*); иначе Infer.
        /// hRoot != nullptr — per-root ApplyRootBackdropPolicyForHwnd (без process-wide Reapply).
        /// hRoot == nullptr — dispatch hub: один ReapplyOpenRootBackdrops на wave (fingerprint).
        void (*OnRootDwmColorizationChanged)(HWND hRoot, WPARAM colorizationWave, LPARAM liveRootScope) = nullptr;

        /// Win11: «хромовый» shell-проводник (вкладки и/или IAT explorerframe) — SWCA только из
        /// `DwmExtendFrameIntoClientArea` до orig; Панель управления и прочий Cabinet без этого — false.
        bool (*IsWin11ExplorerShellCabinetChromeHwnd)(HWND hRoot) noexcept = nullptr;
    };

    void SetRootEventProvider(const RootEventProvider& provider);
    const RootEventProvider* GetRootEventProvider();
}
