#pragma once



#include <GlassControlKind.h>

#include <Windows.h>

#include <cstdint>



namespace GlassCore::TextGlow

{

    enum class TextGlowSpreadCategory : std::uint8_t

    {

        Minimal  = 0,

        Moderate = 1,

        Global   = 2,

    };



    enum class TextGlowClientRole : std::uint8_t

    {

        Control      = 0,

        MenuBarOwner = 1,

    };



    [[nodiscard]] bool IsUnderWindowClassAncestor(HWND hWnd, LPCWSTR cls) noexcept;



    [[nodiscard]] TextGlowSpreadCategory CategoryForControlKind(

        GlassShared::GlassControlKind kind) noexcept;



    [[nodiscard]] TextGlowSpreadCategory ResolveTextGlowSpreadCategory(

        HWND hWnd,

        GlassShared::GlassControlKind kind) noexcept;



    /// Live effective category: kind+nested override; иначе registry popup; иначе Global.

    [[nodiscard]] TextGlowSpreadCategory ResolveEffectiveSpreadCategory(HWND hWnd) noexcept;



    [[nodiscard]] TextGlowSpreadCategory ResolveSpreadCategoryForWindow(HWND hWnd) noexcept;



    [[nodiscard]] bool IsTextGlowRegisterCandidate(

        GlassShared::GlassControlKind kind) noexcept;



    void RegisterTextGlowClient(

        HWND hControl,

        TextGlowSpreadCategory category,

        TextGlowClientRole role = TextGlowClientRole::Control) noexcept;

    void UnregisterTextGlowClient(HWND hControl) noexcept;



    void TryRegisterTextGlowClient(HWND hControl) noexcept;

    void EnsureTextGlowClientRegistered(HWND hControl) noexcept;



    void RegisterTextGlowPopupClient(HWND hControl, TextGlowSpreadCategory category) noexcept;

    void TryRegisterMenuBarOwner(HWND hOwner) noexcept;



    void SoftInvalidateTextGlowClientsForRoot(HWND hRoot) noexcept;

    void RunDeferredTextGlowSoftInvalidate(HWND hRoot) noexcept;



    [[nodiscard]] bool IsInkGlowAllowedForWindow(HWND hWnd) noexcept;

    /// Glow разрешён только для hwnd, присутствующего в реестре (после фильтра spread).
    [[nodiscard]] bool IsTextGlowClientRegistered(HWND hWnd) noexcept;

    void RebindTextGlowClientsFromMarkedWindows() noexcept;

    /// При понижении spread: invalidate клиентов, которые были в реестре по previousSpread, но выпадают из newSpread (до purge).
    void SoftInvalidateRegisteredClientsDroppedBySpreadDecrease(
        std::uint32_t previousSpread,
        std::uint32_t newSpread) noexcept;

    void RemoveTextGlowClientsForRoot(HWND hRoot) noexcept;

    void ClearAllTextGlowClients() noexcept;

} // namespace GlassCore::TextGlow

