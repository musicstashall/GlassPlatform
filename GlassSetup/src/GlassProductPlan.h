#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Portable policy only: no registry/MSI/filesystem side effects. Discovery must
// supply verified facts; UI and workers must consume the same resulting plan.
namespace GlassProductPlan {
enum class Product : std::size_t { Platform64, Platform32, Settings, OpenGlass, SecureUxTheme, Settings32, OpenGlassResources, Count, Settings64=Settings };
constexpr std::size_t kProductCount = static_cast<std::size_t>(Product::Count);
constexpr std::size_t Index(Product p) noexcept { return static_cast<std::size_t>(p); }

enum class Mode { Install, RemoveInteractive, RemoveSilent };
enum class Presence { Absent, Installed, Unknown };
enum class Ownership { Managed, External, Unknown };
enum class Condition { Healthy, Damaged, Unverifiable };
enum class Version { Same, Older, Newer, Unknown };
enum class Source { Verified, Missing, Invalid };
enum class Transition { Idle, RebootPending, RecoveryRequired, Busy };
enum class Activity { Inactive, Active, Unknown };
enum class Action { None, Keep, Install, Update, Repair, Remove, Blocked };
enum class Error {
    None, Unsupported, UnknownPresence, ExternalOwnership, UnknownOwnership,
    Busy, RebootPending, RecoveryRequired, Unverifiable, UnknownVersion,
    Downgrade, MissingSource, InvalidSource, SettingsRemovalNeedsInteractivePlan,
    OpenGlassRequired
};
enum class Warning : std::uint32_t {
    None = 0, NoPlatform64 = 1u << 0, NoPlatform32 = 1u << 1,
    ManualSettings = 1u << 2, ManualVisualStyles = 1u << 3,
    DamageRetained = 1u << 4, GlobalSettingsCleanup = 1u << 5, ManualOpenGlass = 1u << 6, NoHelper64 = 1u << 7, NoHelper32 = 1u << 8
};
constexpr Warning operator|(Warning a, Warning b) noexcept {
    return static_cast<Warning>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr bool Has(Warning value, Warning flag) noexcept {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0;
}
struct Facts {
    // Unpopulated discovery must never authorize an installation or removal.
    bool supported = false;
    Presence presence = Presence::Unknown;
    Ownership ownership = Ownership::Unknown;
    Condition condition = Condition::Unverifiable;
    Version version = Version::Unknown;
    Source source = Source::Missing;
    Transition transition = Transition::Idle;
    // Distinct from presence: prepared MSI does not mean an active hook.
    Activity activity = Activity::Unknown;
};
struct Request {
    Mode mode = Mode::Install;
    bool os64 = true;
    std::array<bool, kProductCount> selected{};
    // Explicit per-product intents allow mixed maintenance; None uses selected/mode.
    std::array<Action,kProductCount> actions{};
    // Separate explicit consent; SelectAll never changes it.
    bool cleanupAllProfiles = false;
};
struct Row {
    Action action = Action::None;
    Error error = Error::None;
    Presence after = Presence::Unknown;
    Warning warnings = Warning::None;
};
struct Plan {
    std::array<Row, kProductCount> rows{};
    bool valid = true;
    bool cleanupAllProfiles = false;
    bool prepareVisualStyleReset = false;
    std::size_t selectedCount = 0;
    std::size_t actionCount = 0;
    Warning warnings = Warning::None;
};
constexpr bool IsMutation(Action action) noexcept {
    return action == Action::Install || action == Action::Update ||
        action == Action::Repair || action == Action::Remove;
}
constexpr Error MutationError(const Facts& f, bool install) noexcept {
    if (!f.supported) return Error::Unsupported;
    if (f.presence == Presence::Unknown) return Error::UnknownPresence;
    if (f.ownership == Ownership::External) return Error::ExternalOwnership;
    if (f.ownership == Ownership::Unknown) return Error::UnknownOwnership;
    switch (f.transition) {
    case Transition::Busy: return Error::Busy;
    case Transition::RebootPending: return Error::RebootPending;
    case Transition::RecoveryRequired: return Error::RecoveryRequired;
    default: break;
    }
    if (f.condition == Condition::Unverifiable) return Error::Unverifiable;
    if (!install) return Error::None; // Removal does not require installation media.
    if (f.presence == Presence::Installed) {
        if (f.version == Version::Newer) return Error::Downgrade;
        if (f.version == Version::Unknown) return Error::UnknownVersion;
    }
    if (f.source == Source::Missing) return Error::MissingSource;
    if (f.source == Source::Invalid) return Error::InvalidSource;
    return Error::None;
}
constexpr bool SupportedOnOs(Product p, bool os64) noexcept {
    return os64 || (p != Product::Platform64 && p != Product::OpenGlass && p != Product::Settings64 && p != Product::OpenGlassResources);
}
constexpr void AddWarning(Plan& plan, Product product, Warning warning) noexcept {
    plan.rows[Index(product)].warnings = plan.rows[Index(product)].warnings | warning;
    plan.warnings = plan.warnings | warning;
}
// Architecture pairs are independent. A stopped but registered Platform still
// needs its helper for subsequent removal; the other architecture is irrelevant.
constexpr unsigned SettingsRemovalRiskMask(bool settings64, bool settings32,
    bool platform64Present, bool platform32Present, bool removesPlatform64, bool removesPlatform32) noexcept {
    return (settings64 && platform64Present && !removesPlatform64 ? 1u : 0u) |
        (settings32 && platform32Present && !removesPlatform32 ? 2u : 0u);
}
constexpr bool RequiresInteractiveSettingsRemoval(Mode mode, bool removesSettings,
    Presence sutPresence, Activity sutActivity) noexcept {
    return mode == Mode::RemoveSilent && removesSettings &&
        (sutPresence != Presence::Absent || sutActivity != Activity::Inactive);
}
constexpr Plan Build(const std::array<Facts, kProductCount>& facts, const Request& request) noexcept {
    Plan result;
    const bool install = request.mode == Mode::Install;
    // Silent product removal has no global-cleanup consent protocol.
    result.cleanupAllProfiles = request.mode == Mode::RemoveInteractive && request.cleanupAllProfiles;
    for (std::size_t i = 0; i < kProductCount; ++i) {
        const auto& f = facts[i];
        auto& row = result.rows[i];
        row.after = f.presence;
        row.action = f.presence == Presence::Installed ? Action::Keep : Action::None;
        const auto intent=request.actions[i];
        const bool explicitMutation=IsMutation(intent);
        if ((!request.selected[i] && !explicitMutation) || intent==Action::Keep) continue;
        const bool rowInstall=explicitMutation?intent!=Action::Remove:install;
        ++result.selectedCount;
        // Known absent removal is an idempotent no-op, not a source check.
        if (!rowInstall && f.presence == Presence::Absent) continue;
        row.error = !SupportedOnOs(static_cast<Product>(i), request.os64)
            ? Error::Unsupported : MutationError(f, rowInstall);
        if (row.error != Error::None) {
            row.action = Action::Blocked;
            result.valid = false;
            continue;
        }
        if (!rowInstall) {
            row.action = Action::Remove;
            row.after = Presence::Absent;
        } else {
            row.action = f.presence == Presence::Absent ? Action::Install :
                f.version == Version::Older ? Action::Update : Action::Repair;
            row.after = Presence::Installed;
        }
    }
    // Resources is a dependent graphics MSI, never an implicit OG selection.
    // Check the planned after-state; a failed/blocked OG apply cannot satisfy it.
    const auto og = Index(Product::OpenGlass);
    const auto resources = Index(Product::OpenGlassResources);
    auto& graphics = result.rows[resources];
    if (IsMutation(graphics.action) && graphics.action != Action::Remove &&
        (result.rows[og].after != Presence::Installed || result.rows[og].action == Action::Blocked)) {
        graphics.action = Action::Blocked;
        graphics.error = Error::OpenGlassRequired;
        graphics.after = facts[resources].presence;
        result.valid = false;
    }
    // Removing OG does not implicitly remove retained graphics/user choices.
    // An orphan graphics MSI can still be removed without its prerequisite.
    // Entry-point guard, NOT a SUT -> Settings dependency/cascade. Even a silent
    // request to remove both is refused: the user must see an explicit UI plan.
    const auto settings = Index(Product::Settings);
    const auto sut = Index(Product::SecureUxTheme);
    for(const auto settingsRow:{settings,Index(Product::Settings32)})
    if (RequiresInteractiveSettingsRemoval(request.mode, result.rows[settingsRow].action == Action::Remove,
        facts[sut].presence, facts[sut].activity)) {
        auto& row = result.rows[settingsRow];
        row.action = Action::Blocked;
        row.error = Error::SettingsRemovalNeedsInteractivePlan;
        row.after = facts[settingsRow].presence;
        result.valid = false;
    }
    // Warnings describe the proposed after-state. An invalid plan must never be
    // executed; consumers display the blocking reasons and rebuild after edits.
    if (request.os64 && result.rows[Index(Product::Platform64)].after == Presence::Absent)
        AddWarning(result, Product::Platform64, Warning::NoPlatform64);
    if (result.rows[Index(Product::Platform32)].after == Presence::Absent)
        AddWarning(result, Product::Platform32, Warning::NoPlatform32);
    if(request.os64 && result.rows[Index(Product::Platform64)].after==Presence::Installed &&
       result.rows[Index(Product::Settings64)].after==Presence::Absent) {
        AddWarning(result,Product::Platform64,Warning::NoHelper64);
        AddWarning(result,Product::Settings64,Warning::NoHelper64);
    }
    if(result.rows[Index(Product::Platform32)].after==Presence::Installed &&
       result.rows[Index(Product::Settings32)].after==Presence::Absent) {
        AddWarning(result,Product::Platform32,Warning::NoHelper32);
        AddWarning(result,Product::Settings32,Warning::NoHelper32);
    }
    const bool noSettings=result.rows[settings].after==Presence::Absent && result.rows[Index(Product::Settings32)].after==Presence::Absent;
    if (noSettings) {
        AddWarning(result, Product::Settings, Warning::ManualSettings);
        AddWarning(result, Product::Settings32, Warning::ManualSettings);
    }
    if (result.rows[Index(Product::OpenGlass)].after == Presence::Installed &&
        noSettings)
        AddWarning(result, Product::OpenGlass, Warning::ManualOpenGlass);
    const bool sutWillBeUsed = result.rows[sut].action != Action::Remove &&
        (facts[sut].activity != Activity::Inactive ||
         (IsMutation(result.rows[sut].action) && result.rows[sut].action != Action::Remove));
    if (sutWillBeUsed && noSettings)
        AddWarning(result, Product::SecureUxTheme, Warning::ManualVisualStyles);
    for (std::size_t i = 0; i < kProductCount; ++i) {
        if (result.rows[i].action == Action::Keep && facts[i].condition == Condition::Damaged)
            AddWarning(result, static_cast<Product>(i), Warning::DamageRetained);
        if (IsMutation(result.rows[i].action)) ++result.actionCount;
    }
    if (result.cleanupAllProfiles) result.warnings = result.warnings | Warning::GlobalSettingsCleanup;
    // This requests an inspection/reset phase, NOT an unconditional style reset.
    result.prepareVisualStyleReset = result.valid && result.rows[sut].action == Action::Remove;
    return result;
}
constexpr Request SelectAll(const std::array<Facts, kProductCount>& facts, Request request) noexcept {
    const bool install = request.mode == Mode::Install;
    for (std::size_t i = 0; i < kProductCount; ++i) {
        request.actions[i]=Action::None;
        request.selected[i] = SupportedOnOs(static_cast<Product>(i), request.os64) &&
            (install || facts[i].presence == Presence::Installed) &&
            MutationError(facts[i], install) == Error::None;
    }
    // Honor the entry point guard for bulk headless callers as well.
    if (RequiresInteractiveSettingsRemoval(request.mode, true,
        facts[Index(Product::SecureUxTheme)].presence, facts[Index(Product::SecureUxTheme)].activity))
        {request.selected[Index(Product::Settings)] = false;request.selected[Index(Product::Settings32)] = false;}
    return request;
}
constexpr Request FreshInstall(bool os64) noexcept {
    Request request;
    request.os64 = os64;
    request.selected = {os64, true, os64, os64, false, true};
    return request;
}

// Session style assessment is deliberately separate from package presence and
// from the cleanup checkbox. TrustedSystem must come from a verified assessor,
// never from just a basename (aero.msstyles) or a theme display name.
enum class Style { TrustedSystem, ResetRequired, Unknown };
enum class Session { Current, OtherLive, Offline };
enum class StyleStep { Preserve, ApplyAndVerify, ResetOfflineAndVerify, WaitForLogoff, Inspect };
struct StyleFacts {
    Style style = Style::Unknown;
    Session session = Session::Offline;
    bool sessionHelperVerified = false;
    bool hiveAccessible = false;
};
constexpr StyleStep AssessStyle(const StyleFacts& f) noexcept {
    if (f.style == Style::TrustedSystem) return StyleStep::Preserve;
    if (f.style == Style::Unknown) return StyleStep::Inspect;
    if (f.session == Session::Current || (f.session == Session::OtherLive && f.sessionHelperVerified))
        return StyleStep::ApplyAndVerify;
    if (f.session == Session::OtherLive) return StyleStep::WaitForLogoff;
    return f.hiveAccessible ? StyleStep::ResetOfflineAndVerify : StyleStep::Inspect;
}
struct StylePreparation {
    bool readyToRemove = true;
    std::size_t preserve = 0, liveApply = 0, offlineReset = 0, wait = 0, inspect = 0;
};
// readyToRemove requires verified results, not merely the existence of a plan.
// Call again after executing/rechecking steps, classifying verified safe styles
// as TrustedSystem. Discovery must enumerate every relevant profile/session.
inline StylePreparation AssessStyles(const StyleFacts* facts, std::size_t count,
    bool discoveryComplete) noexcept {
    StylePreparation result;
    result.readyToRemove = discoveryComplete;
    if (!facts && count != 0) { result.readyToRemove = false; result.inspect = count; return result; }
    for (std::size_t i = 0; i < count; ++i) {
        switch (AssessStyle(facts[i])) {
        case StyleStep::Preserve: ++result.preserve; break;
        case StyleStep::ApplyAndVerify: ++result.liveApply; result.readyToRemove = false; break;
        case StyleStep::ResetOfflineAndVerify: ++result.offlineReset; result.readyToRemove = false; break;
        case StyleStep::WaitForLogoff: ++result.wait; result.readyToRemove = false; break;
        case StyleStep::Inspect: ++result.inspect; result.readyToRemove = false; break;
        }
    }
    return result;
}
} // namespace GlassProductPlan
