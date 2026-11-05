#pragma once

// Shared, platform-neutral component policy. An install selection names the
// products to install or update. Setup never removes a product: an installed
// product that is left unchecked simply stays as it is, and removal is the job
// of the separate uninstaller. All products are independently selectable; warnings do not imply a removal
// cascade. SecureUxTheme without Settings requires explicit interactive consent.
namespace GlassProducts {
struct Selection {
    bool platform64 = true;
    bool platform32 = true;
    bool glassSettings = true;
    bool openGlass = true;
    // Optional SecureUxTheme product selection (per-machine MSI).
    bool secureUxTheme = false;
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

// По умолчанию сняты только неповреждённые установленные продукты;
// повреждённые и устаревшие отмечены для переустановки/обновления.
constexpr bool DefaultSelectedFor(ProductCondition condition) noexcept {
    return condition != ProductCondition::Healthy;
}

constexpr Selection NormalizeInstall(Selection choice, bool os64) noexcept {
    // ОС-нативная платформа обязательна, но не принудительно отмечена: исправный
    // продукт остаётся снятым (Setup его не трогает), а её обязательность держит
    // замок карточки, когда по ней нужна обработка (установка/обновление/ремонт).
    if (!os64) {
        choice.platform64 = false;
        choice.openGlass = false;
    }
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
