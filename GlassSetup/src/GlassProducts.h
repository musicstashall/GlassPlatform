#pragma once

// Shared, platform-neutral component policy. An install selection names the
// products to install or update. Setup never removes a product: an installed
// product that is left unchecked simply stays as it is, and removal is the job
// of the separate uninstaller. The OS-native platform is mandatory; removing it
// in the uninstaller cascades to the other installed products.
namespace GlassProducts {
struct Selection {
    bool platform64 = true;
    bool platform32 = true;
    bool glassSettings = true;
    bool openGlass = true;
    // Windows 11 certificate page: the user chose to add the author certificate
    // (signer of GlassShellSparse.msix) to LocalMachine\TrustedPeople so the
    // compact menu can be registered. Off when the page was not shown.
    bool trustSigner = false;
};

// Recorded at wizard startup, independently of the later rollback snapshot.
struct OpenGlassStartupState {
    bool valid = false;
    bool filesPresent = false;
    bool wasRunning = false;
};
constexpr bool ShouldStartOpenGlass(const OpenGlassStartupState& startup) noexcept {
    return startup.valid && (!startup.filesPresent || startup.wasRunning);
}

// A checked product is installed or updated; an unchecked installed product is
// kept untouched. An unchecked absent product has no action and needs no badge.
enum class InstallAction { None, Install, Update, Keep };
constexpr InstallAction ActionForInstall(bool selected, bool installed) noexcept {
    if (selected) return installed ? InstallAction::Update : InstallAction::Install;
    return installed ? InstallAction::Keep : InstallAction::None;
}

constexpr Selection NormalizeInstall(Selection choice, bool os64) noexcept {
    if (os64) choice.platform64 = true;
    else {
        choice.platform64 = false;
        choice.platform32 = true;
        choice.openGlass = false;
    }
    return choice;
}

constexpr Selection NormalizeRemoval(Selection choice, Selection installed, bool os64) noexcept {
    choice.platform64 = os64 && choice.platform64 && installed.platform64;
    choice.platform32 = choice.platform32 && installed.platform32;
    choice.glassSettings = choice.glassSettings && installed.glassSettings;
    choice.openGlass = os64 && choice.openGlass && installed.openGlass;
    const bool coreSelected = os64 ? choice.platform64 : choice.platform32;
    if (coreSelected) {
        choice.platform32 = installed.platform32;
        choice.glassSettings = installed.glassSettings;
        choice.openGlass = os64 && installed.openGlass;
    }
    return choice;
}
} // namespace GlassProducts
