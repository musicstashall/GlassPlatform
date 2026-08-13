#pragma once

// HWND_MESSAGE на потоке колбэков IVisualTreeServiceCallback — для синхронного
// XamlTapSite::ShutdownVisualTreeWatcher (Restore + Unadvise) с потока Glass GUI.
namespace XamlShutdownMarshal
{
    void EnsureMessageWindow() noexcept;
    [[nodiscard]] HWND GetMessageWindowHwnd() noexcept;
    /// Поток HWND_MESSAGE (marshal policy/shutdown); 0 если окно ещё не создано.
    [[nodiscard]] DWORD GetMessageWindowThreadId() noexcept;
    void RunShutdownVisualTreeWatcherOnXamlThread() noexcept;
    void RunRootBackdropPolicyChangeOnXamlThread(BOOL enabled) noexcept;
    void RunPostLiveDisableChromeRedrawOnXamlThread() noexcept;
    // После Unadvise: уничтожить HWND_MESSAGE (иначе остаётся якорь на код в ExplorerCompat.dll).
    void DestroyMessageWindow() noexcept;

    // ID таймера для периодического re-patch tab-элементов.
    inline constexpr UINT_PTR kRepatchTimerId = 42;
    // Одноразовый WM_TIMER после Advise: RedrawWindow(Shell_TrayWnd) — сброс устаревшего композитного кадра.
    inline constexpr UINT_PTR kTrayRedrawAfterAdviseTimerId = 43;
}
