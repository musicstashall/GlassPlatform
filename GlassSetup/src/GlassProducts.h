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
