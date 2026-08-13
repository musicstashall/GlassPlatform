#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace Shared::RootBackdropPolicy
{
    // Нормализация имени класса (как WndCompat::NormalizeWindowClass) — для проверки exempt без зависимости от WndCompat.
    [[nodiscard]] std::wstring NormalizeWindowClass(HWND hWnd) noexcept;

    // Жёсткий список «чужой / native backdrop» — Glass не применяет и не снимает DWM/SWCA (субкласс root допускается).
    // Единственный источник строк: RootBackdropPolicy.cpp (WindowClassifier::IsHardBlockedClass делегирует сюда).
    [[nodiscard]] bool IsNormalizedClassBackdropExempt(const std::wstring& cls) noexcept;

    [[nodiscard]] bool IsHwndBackdropExempt(HWND hWnd) noexcept;

    /// Win11 Notification/Action Center host: ShellExperienceHost.exe / ShellHost.exe.
    [[nodiscard]] bool IsShellNotificationHostProcess() noexcept;

    /// Quick Settings host (24H2+): ShellHost.exe — IXDE только с ControlCenterWindow.
    [[nodiscard]] bool IsShellHostProcess() noexcept;

    /// Notification Center host: ShellExperienceHost.exe.
    [[nodiscard]] bool IsShellExperienceHostProcess() noexcept;

    /// Win11 Start Menu host: StartMenuExperienceHost.exe.
    [[nodiscard]] bool IsStartMenuHostProcess() noexcept;

    /// Win11 Search UI host: SearchHost.exe / SearchApp.exe.
    [[nodiscard]] bool IsSearchHostProcess() noexcept;

    /// Любой CoreWindow host для UICompat: SEH / ShellHost / SMEH / SearchHost.
    [[nodiscard]] bool IsCoreWindowHostProcess() noexcept;

    // Топ-level окно проводника Win11: CabinetWClass / ExploreWClass.
    // Только для подавления root SWCA/DWM в GlassCore — НЕ для IsHwndBackdropExempt (иначе ломается GlassAttachGate).
    [[nodiscard]] bool IsFileExplorerShellRootHwnd(HWND hWnd) noexcept;

    /// Win11: в поддереве Cabinet/Explore — класс ShellTabWindowClass (типичный проводник с вкладками;
    /// без него чаще классическая Панель управления на том же CabinetWClass).
    [[nodiscard]] bool ExplorerShellRootHasShellTabDescendant(HWND hRoot) noexcept;

    // Флаг: Glass реально выставил root backdrop на HWND (DWM или SWCA). Снимается при OnRootDetach.
    void NotifyGlassAppliedRootBackdrop(HWND hWnd) noexcept;
    void NotifyGlassClearedRootBackdrop(HWND hWnd) noexcept;
    [[nodiscard]] bool DidGlassApplyRootBackdrop(HWND hWnd) noexcept;

    /// In-memory снимок g_appliedHwnds в **текущем модуле** (GlassShared static lib — у GlassCore и WndCompat
    /// отдельные копии). Для sweep в GlassCore. WndCompat live-reload applied roots: g_rootWindows +
    /// DidGlassApplyRootBackdrop (SetProp, cross-DLL).
    [[nodiscard]] std::vector<HWND> SnapshotGlassAppliedRootHwnds() noexcept;
}
