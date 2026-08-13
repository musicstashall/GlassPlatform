#pragma once

#include <GlassControlKind.h>

#include <Windows.h>

namespace GlassCore::TextInk
{
    [[nodiscard]] bool IsTextInkRegisterCandidate(GlassShared::GlassControlKind kind) noexcept;

    void RegisterTextInkClient(HWND hControl) noexcept;
    void UnregisterTextInkClient(HWND hControl) noexcept;

    void TryRegisterTextInkClient(HWND hControl) noexcept;
    void EnsureTextInkClientRegistered(HWND hControl) noexcept;

    [[nodiscard]] bool IsTextInkClientRegistered(HWND hWnd) noexcept;

    void SoftInvalidateTextInkClientsForBucket(HWND hBucket) noexcept;
    void SoftInvalidateTextInkClientsForGaRootTree(HWND hRoot) noexcept;
    void RunDeferredTextInkSoftInvalidate(HWND hRoot) noexcept;

    void RemoveTextInkClientsForRoot(HWND hRoot) noexcept;
    void ClearAllTextInkClients() noexcept;
} // namespace GlassCore::TextInk
