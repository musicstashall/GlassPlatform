#pragma once

// Shared, platform-neutral component policy. An install selection describes
// the desired final product set: previously installed, unchecked products are
// removed. The OS-native platform is mandatory; removing it in the separate
// uninstaller cascades to the other installed products.
namespace GlassProducts {
struct Selection {
    bool platform64 = true;
    bool platform32 = true;
    bool glassSettings = true;
    bool openGlass = true;
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
// removed. An unchecked absent product has no action and needs no badge.
enum class InstallAction { None, Install, Update, Remove };
constexpr InstallAction ActionForInstall(bool selected, bool installed) noexcept {
    if (selected) return installed ? InstallAction::Update : InstallAction::Install;
    return installed ? InstallAction::Remove : InstallAction::None;
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

// Installed products outside the desired set must be removed, never the
// mandatory platform. Individual MSI architectures can be mapped to one item.
constexpr Selection RemovalForInstall(Selection choice, Selection installed, bool os64) noexcept {
    const Selection desired = NormalizeInstall(choice, os64);
    return {
        os64 && installed.platform64 && !desired.platform64,
        installed.platform32 && !desired.platform32,
        installed.glassSettings && !desired.glassSettings,
        os64 && installed.openGlass && !desired.openGlass
    };
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
