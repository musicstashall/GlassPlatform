#pragma once
#include <Windows.h>

// Копия сценария WndCompat ComboLBox_Attach для выпадающего списка ComboBox в LogViewer (GlassCore уже в процессе).
namespace LogViewer::ComboLBoxUi
{
    [[nodiscard]] bool IsAttached(HWND hList) noexcept;
    void Attach(HWND hList) noexcept;
    void Detach(HWND hList) noexcept;
    void DetachAll() noexcept;
    // CBN_DROPDOWN → GlassCore::ComboLBox::OnComboBoxDropDown (как WndCompat RootSubclass).
    void NotifyComboBoxDropDown(HWND hComboBox) noexcept;
    // Как WndCompat WinEventHook_Shutdown: PeekMessage/DispatchMessage, пока GlassCore ещё загружен.
    void FlushPendingUiMessages(DWORD maxWaitMs) noexcept;
    // После FreeLibrary(GlassCore): как SetDropDownEventProvider({}) — не держать указатель в s_providers.
    void ClearCachedGlassProviders() noexcept;
}
