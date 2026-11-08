#pragma once
#include <cstdint>

// Shared, platform-neutral component policy. An install selection names the
// products to install or update. An unchecked installed product is kept. Explicit
// managed removal has independent row intents; the original inactive vendor MSI
// retains its separate commit-last protocol.
// All products are independently selectable; warnings do not imply a removal
// cascade. SecureUxTheme without Settings requires explicit interactive consent.
namespace GlassProducts {
struct Selection {
    bool platform64 = true;
    bool platform32 = true;
    bool glassSettings = true;
    bool openGlass = true;
    // Optional SecureUxTheme product selection (per-machine MSI).
    bool secureUxTheme = false;
    // Explicit original-MSI removal, not an unchecked install selection.
    bool secureUxThemeRemove = false;
    // Same row indices as GlassProductPlan::Product. SUT has its separate protocol.
    std::uint32_t removeMask = 0;
    unsigned confirmedSettingsRemovalRisk = 0;
    constexpr bool Removes(unsigned row) const noexcept { return (removeMask & (1u<<row))!=0; }
    static constexpr std::uint32_t kManagedRemovalMask = 0x2Fu;
    // Windows 11 certificate page: the user chose to add the author certificate
    // (signer of GlassShellSparse.msix) to LocalMachine\TrustedPeople so the
    // compact menu can be registered. Off when the page was not shown.
    bool trustSigner = false;
    // Explicit per-architecture choices. Legacy callers keep OS-native semantics.
    bool independentSettings = false;
    bool settings64 = false;
    bool settings32 = false;
    constexpr bool Settings64(bool os64) const noexcept { return os64 && (independentSettings?settings64:glassSettings); }
    constexpr bool Settings32(bool os64) const noexcept { return independentSettings?settings32:(!os64&&glassSettings); }
    constexpr void SyncSettings(bool os64) noexcept {
        if(!os64)settings64=false;
        if(independentSettings)glassSettings=settings64||settings32;
    }
};

// Pure managed removal needs the installed MSI baseline, not new release media.
constexpr bool OnlyManagedRemoval(const Selection& choice) noexcept {
    return choice.removeMask && !choice.secureUxTheme && !choice.secureUxThemeRemove &&
        !choice.platform64 && !choice.platform32 && !choice.openGlass &&
        !choice.glassSettings && !choice.settings64 && !choice.settings32;
}

// Removing an already inactive vendor MSI does not modify any Glass product.
constexpr bool OnlySecureUxThemeRemoval(const Selection& choice) noexcept {
    return !choice.removeMask && choice.secureUxThemeRemove && !choice.secureUxTheme &&
        !choice.platform64 && !choice.platform32 && !choice.openGlass &&
        !choice.glassSettings && !choice.settings64 && !choice.settings32;
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
// be chosen explicitly; retain the existing recommendation for a fresh install.
constexpr bool DefaultSelectedFor(ProductCondition condition, bool maintenance = false) noexcept {
    return !maintenance && condition == ProductCondition::NotInstalled;
}

constexpr Selection NormalizeInstall(Selection choice, bool os64) noexcept {
    // ОС-нативная платформа обязательна, но не принудительно отмечена: исправный
    // продукт остаётся снятым (Setup его не трогает), а её обязательность держит
    // замок карточки, когда по ней нужна обработка (установка/обновление/ремонт).
    if (!os64) {
        choice.platform64 = false;
        choice.openGlass = false;
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
    return choice;
}
} // namespace GlassProducts
