#pragma once
#include <cstdint>

// Shared, platform-neutral component policy. An install selection names the
// products to install or update. An unchecked installed product is kept. Explicit
// managed removal has independent row intents; the original vendor MSI
// retains its separate commit-last protocol.
// All products are independently selectable; warnings do not imply a removal
// cascade. SecureUxTheme without Settings requires explicit interactive consent.
namespace GlassProducts {
// ActivationPending means registration exists but the native loaded event is
// absent. It is not an obstacle to cancelling that installation via vendor MSI.
template<class State> constexpr bool SecureUxUnloadedRemoval(State state) noexcept {
    return state==State::Inactive || state==State::ActivationPending;
}
template<class State> constexpr bool SecureUxKnownRemoval(State state) noexcept {
    return SecureUxUnloadedRemoval(state)||state==State::Active||state==State::DeactivationPending;
}
// The author's MSI schedules reboot unconditionally. Refine its advisory 3010
// only with a verified post-operation runtime state; unknown states retain it.
template<class State> constexpr bool SecureUxRestartAfter(bool installing,State state,bool vendorRestart) noexcept {
    if(installing) {
        if(state==State::Active)return false;
        if(state==State::ActivationPending)return true;
    }else {
        if(state==State::Inactive || state==State::Unavailable)return false;
        if(state==State::Active || state==State::DeactivationPending)return true;
    }
    return vendorRestart;
}
struct Selection {
    bool platform64 = true;
    bool platform32 = true;
    bool glassSettings = true;
    bool openGlass = true;
    // Optional SecureUxTheme product selection (per-machine MSI).
    bool secureUxTheme = false;
    bool openGlassResources = false; // Graphics-only x64 MSI, row 6.
    // Explicit original-MSI removal, not an unchecked install selection.
    bool secureUxThemeRemove = false;
    // Explicit interactive risk acceptance; valid only while BOTH Settings MSI
    // products are absent. Never infer absence from a missing Native DLL.
    bool secureUxRemovalWithoutSettings = false;
    // Same row indices as GlassProductPlan::Product. SUT has its separate protocol.
    std::uint32_t removeMask = 0;
    unsigned confirmedSettingsRemovalRisk = 0;
    constexpr bool Removes(unsigned row) const noexcept { return (removeMask & (1u<<row))!=0; }
    static constexpr std::uint32_t kManagedRemovalMask = 0x6Fu;
    // Windows 11 certificate page: the user chose to add the author certificate
    // (signer of GlassShellSparse.msix) to LocalMachine\TrustedPeople so the
    // compact menu can be registered. Off when the page was not shown.
    bool trustSigner = false;
    // Explicit per-architecture choices. Legacy callers keep OS-native semantics.
    bool independentSettings = false;
    bool settings64 = false;
    bool settings32 = false;
    bool cleanupUserData = false; // Explicit full-removal consent, never inferred.
    constexpr bool Settings64(bool os64) const noexcept { return os64 && (independentSettings?settings64:glassSettings); }
    constexpr bool Settings32(bool os64) const noexcept { return independentSettings?settings32:(!os64&&glassSettings); }
    constexpr void SyncSettings(bool os64) noexcept {
        if(!os64)settings64=false;
        if(independentSettings)glassSettings=settings64||settings32;
    }
};

// Temporary official-EXE lane: reject unsupported combinations before mutation.
constexpr bool StandaloneOpenGlassApply(const Selection& s,bool forceClean,bool os64) noexcept {
    return !s.openGlassResources&&os64&&s.openGlass&&!s.platform64&&!s.platform32&&!s.glassSettings&&
        !s.settings64&&!s.settings32&&!s.secureUxTheme&&!s.secureUxThemeRemove&&
        !s.removeMask&&!s.cleanupUserData&&!s.trustSigner&&!forceClean;
}

constexpr bool StandaloneOpenGlassRemoval(const Selection& s,bool forceClean,bool os64) noexcept {
    return !s.openGlassResources&&os64&&!s.openGlass&&s.removeMask==(1u<<3)&&!s.platform64&&!s.platform32&&
        !s.glassSettings&&!s.settings64&&!s.settings32&&!s.secureUxTheme&&!s.secureUxThemeRemove&&
        !s.cleanupUserData&&!s.trustSigner&&!forceClean;
}

// Sequential commits: managed Glass apply first, official Inno apply last.
// Two different external vendors and mixed removal need a separate protocol.
constexpr bool MixedOpenGlassApply(const Selection& s,bool forceClean,bool os64) noexcept {
    return os64&&s.openGlass&&(s.openGlassResources||s.platform64||s.platform32||s.Settings64(os64)||s.Settings32(os64))&&
        !s.secureUxTheme&&!s.secureUxThemeRemove&&!s.removeMask&&!s.cleanupUserData&&!forceClean;
}

// Managed apply can precede official OG removal just as it precedes OG apply.
constexpr bool MixedOpenGlassRemoval(const Selection& s,bool forceClean,bool os64) noexcept {
    return !s.openGlassResources&&os64&&!s.openGlass&&s.removeMask==(1u<<3)&&
        (s.platform64||s.platform32||s.Settings64(os64)||s.Settings32(os64))&&
        !s.secureUxTheme&&!s.secureUxThemeRemove&&!s.cleanupUserData&&!forceClean;
}

// Graphics can follow an existing OG or its explicit official Apply in this plan.
constexpr bool ResourcesApplyReady(const Selection& s,bool os64,bool upstreamReady) noexcept {
    return os64&&(upstreamReady||s.openGlass)&&!s.Removes(3)&&
        (!s.openGlass||(!s.secureUxTheme&&!s.secureUxThemeRemove&&!s.removeMask&&!s.cleanupUserData));
}

constexpr bool ResourcesPlanReady(const Selection& s,bool os64,bool upstreamReady) noexcept {
    return (!s.openGlassResources||ResourcesApplyReady(s,os64,upstreamReady))&&
        (!s.Removes(6)||(os64&&!s.openGlass&&!s.Removes(3)));
}

// Pure managed removal needs the installed MSI baseline, not new release media.
constexpr bool OnlyManagedRemoval(const Selection& choice) noexcept {
    return choice.removeMask && !choice.secureUxTheme && !choice.secureUxThemeRemove &&
        !choice.platform64 && !choice.platform32 && !choice.openGlass && !choice.openGlassResources &&
        !choice.glassSettings && !choice.settings64 && !choice.settings32;
}

// Vendor-only removal does not modify any Glass product (loaded style preparation
// is separately authorized and may reset the current user style).
constexpr bool OnlySecureUxThemeRemoval(const Selection& choice) noexcept {
    return !choice.removeMask && choice.secureUxThemeRemove && !choice.secureUxTheme &&
        !choice.platform64 && !choice.platform32 && !choice.openGlass && !choice.openGlassResources &&
        !choice.glassSettings && !choice.settings64 && !choice.settings32;
}

// Full-removal review uses current installed facts, not unchecked Apply flags.
constexpr bool FullRemovalSelected(const Selection& choice,unsigned installedMask,bool vendorInstalled) noexcept {
    return (OnlyManagedRemoval(choice)||OnlySecureUxThemeRemoval(choice))&&
        (installedMask||vendorInstalled)&&
        (choice.removeMask&installedMask)==installedMask&&(!vendorInstalled||choice.secureUxThemeRemove);
}

// Recorded at wizard startup, independently of the later rollback snapshot.
struct OpenGlassStartupState {
    bool valid = false;
    bool filesPresent = false;
    bool wasRunning = false;
};
constexpr bool ShouldStartOpenGlass(const OpenGlassStartupState& startup) noexcept {
    return startup.valid && (!startup.filesPresent || startup.wasRunning);
}

// Removal has no new media/controller to publish. Keep the running cache intact.
constexpr bool HasPackageApply(const Selection& s) noexcept {
    return s.platform64||s.platform32||s.glassSettings||s.settings64||s.settings32||s.openGlass||s.secureUxTheme||s.openGlassResources;
}
// Only an explicitly selected startup-capable product owns the launch page.
// Restoring a previously running, temporarily stopped retained host is part of
// maintenance finalization, not a new launch choice or a five-second UI phase.
constexpr bool NeedsHostLaunch(const Selection& s,const OpenGlassStartupState& og) noexcept {
    return s.platform64||s.platform32||(s.openGlass&&ShouldStartOpenGlass(og));
}

// A checked product is installed or updated; an unchecked installed product is
// kept untouched. An unchecked absent product has no action and needs no badge.
enum class InstallAction { None, Install, Update, Keep, Repair };

// Inspection classification of an installed product (Task F stages 1-2).
// NotInstalled — пакет не установлен; Healthy — целостность не нарушена;
// UpdateAvailable — версия установщика выше; Damaged — обнаружены повреждения.
enum class ProductCondition { NotInstalled, Healthy, UpdateAvailable, Damaged };

constexpr InstallAction ActionForInstall(bool selected, bool installed) noexcept {
    if (selected) return installed ? InstallAction::Update : InstallAction::Install;
    return installed ? InstallAction::Keep : InstallAction::None;
}

// Карточка выбирается по состоянию, а не по галочке: принудительная отметка
// здорового продукта не меняет этикетку («Установлено» остаётся зелёной).
constexpr InstallAction ActionForCondition(bool selected, ProductCondition condition) noexcept {
    if (condition == ProductCondition::NotInstalled)
        return selected ? InstallAction::Install : InstallAction::None;
    if (!selected)
        return InstallAction::Keep;
    if (condition == ProductCondition::UpdateAvailable)
        return InstallAction::Update;
    if (condition == ProductCondition::Damaged)
        return InstallAction::Repair;
    return InstallAction::Repair; // Healthy + принудительная отметка
}

// Installed health is information, not consent to mutate. Repair/update must
// be chosen explicitly, including a fresh install.
constexpr bool DefaultSelectedFor(ProductCondition condition, bool maintenance = false) noexcept {
    (void)condition;(void)maintenance;
    return false;
}

constexpr Selection NormalizeInstall(Selection choice, bool os64) noexcept {
    // ОС-нативная платформа обязательна, но не принудительно отмечена: исправный
    // продукт остаётся снятым (Setup его не трогает), а её обязательность держит
    // замок карточки, когда по ней нужна обработка (установка/обновление/ремонт).
    if (!os64) {
        choice.platform64 = false;
        choice.openGlass = false;
        choice.openGlassResources = false;
    }
    choice.SyncSettings(os64);
    return choice;
}

constexpr Selection NormalizeRemoval(Selection choice, Selection installed, bool os64) noexcept {
    choice.platform64 = os64 && choice.platform64 && installed.platform64;
    choice.platform32 = choice.platform32 && installed.platform32;
    choice.glassSettings = choice.glassSettings && installed.glassSettings;
    choice.openGlass = os64 && choice.openGlass && installed.openGlass;
    choice.secureUxTheme = choice.secureUxTheme && installed.secureUxTheme;
    choice.openGlassResources = os64 && choice.openGlassResources && installed.openGlassResources;
    return choice;
}
} // namespace GlassProducts
