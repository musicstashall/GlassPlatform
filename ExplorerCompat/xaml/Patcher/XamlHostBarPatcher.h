#pragma once
#include <xamlOM.h>
#include <windows.h>
#include <unordered_set>

struct IXamlDiagnostics;
struct IInspectable;

// ═══════════════════════════════════════════════════════════════════════════════
//  Логика патчинга XAML-элементов в explorer.exe.
//
//  Активные зоны (зона 1 — панель задач — переехала в TaskbarCompat.dll):
//   ЗОНА 3 — Шапка проводника  (WinUI host bars: навигация, адрес, табы, команды)
//   ЗОНА 4 — Клиентский остров (FolderView: папки и файлы)
//
//  Публичный patch/unpatch — только ApplyExplorerChrome / RestoreExplorerChrome.
// ═══════════════════════════════════════════════════════════════════════════════

namespace XamlHostBarPatcher
{
    enum class ExplorerChromeApplyScope
    {
        // Один узел — VTC Add (Attach).
        Incremental,
        // Replay всего кэша — live ON, pulse на открытом окне.
        FullReplay,
    };

    enum class ExplorerChromeRestoreScope
    {
        // Live NoBackdrop OFF: host bar + diag, без popup.
        HostBarOnly,
        // Shutdown / Detach_All: popup + host bar + fills.
        Full,
    };

    // ═══ Единственная точка входа PATCH ═══
    [[nodiscard]] int ApplyExplorerChrome(
        IXamlDiagnostics* diag,
        ExplorerChromeApplyScope scope,
        const VisualElement* element = nullptr,
        InstanceHandle parent = 0) noexcept;

    // ═══ Единственная точка входа RESTORE ═══
    void RestoreExplorerChrome(IXamlDiagnostics* diag, ExplorerChromeRestoreScope scope) noexcept;

    void TrackParent(InstanceHandle child, InstanceHandle parent) noexcept;
    void TrackElementMetadata(VisualElement element) noexcept;

    [[nodiscard]] bool IsExplorerXamlPatchingAllowed() noexcept;

    void OnElementRemoved(IXamlDiagnostics* diag, VisualElement element) noexcept;
    void RepatchTabElements(IXamlDiagnostics* diag) noexcept;
    void StopHostBarRepatchTimer() noexcept;

    void CollectExplorerShellHwndsWithWinUi(std::unordered_set<HWND>& out) noexcept;
    void RegisterExplorerShellHwnd(HWND hShell) noexcept;
    [[nodiscard]] const std::unordered_set<HWND>& GetPatchedExplorerShellHwnds() noexcept;
    void ClearPatchedExplorerShellHwnds() noexcept;
    void Cleanup() noexcept;

    [[nodiscard]] bool TypeOrNameHintsExplorerUi(BSTR type, BSTR name) noexcept;
    [[nodiscard]] bool ShouldCountVtcAddFilteredSample(BSTR type, BSTR name) noexcept;
    [[nodiscard]] bool MatchesExplorerDiagnosticIsland(BSTR type, BSTR name) noexcept;

    [[nodiscard]] HRESULT TryDiagnosticBlackenBackground(
        IXamlDiagnostics* diag,
        InstanceHandle handle,
        IInspectable* insp) noexcept;

    void RestoreDiagBackgrounds(IXamlDiagnostics* diag) noexcept;
    [[nodiscard]] bool AncestorChainIncludesShellChrome(IXamlDiagnostics* diag, InstanceHandle leaf) noexcept;

    void NotifyExplorerStartupPulse() noexcept;

    /// Live ON: FullReplay на UI-очереди острова (синхронно с pump вызывающего потока).
    [[nodiscard]] bool TryApplyExplorerHostBarChromeOnUiQueueSync(
        IXamlDiagnostics* diag,
        int* outPatched) noexcept;

    /// Live OFF: restore host bar на UI-очереди острова (VTC-поток через DispatcherQueue).
    [[nodiscard]] bool TryRestoreExplorerHostBarChromeOnUiQueueSync(
        IXamlDiagnostics* diag,
        bool* outRestored) noexcept;
}
