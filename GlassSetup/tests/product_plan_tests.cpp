#include "../src/GlassProducts.h"
#include "../src/GlassProductPlan.h"
#include <cassert>
#include <iostream>

namespace P = GlassProductPlan;
using Facts = std::array<P::Facts, P::kProductCount>;

P::Facts KnownProduct() {
    return {true, P::Presence::Absent, P::Ownership::Managed, P::Condition::Healthy,
        P::Version::Same, P::Source::Verified, P::Transition::Idle, P::Activity::Inactive};
}
Facts KnownFacts() {
    Facts facts{};
    for (auto& f : facts) f = KnownProduct();
    return facts;
}

int main() {
    // Neither discovery nor recommendations constitute install consent.
    static_assert(!GlassProducts::DefaultSelectedFor(GlassProducts::ProductCondition::NotInstalled, false));
    static_assert(!GlassProducts::DefaultSelectedFor(GlassProducts::ProductCondition::NotInstalled, true));
    static_assert(!GlassProducts::DefaultSelectedFor(GlassProducts::ProductCondition::UpdateAvailable, false));
    static_assert(!GlassProducts::DefaultSelectedFor(GlassProducts::ProductCondition::Damaged, false));

    constexpr auto fresh = P::FreshInstall(true);
    static_assert(fresh.selected[0] && fresh.selected[1] && fresh.selected[2] && fresh.selected[3]);
    static_assert(!fresh.selected[4]);
    constexpr auto fresh32 = P::FreshInstall(false);
    static_assert(!fresh32.selected[0] && fresh32.selected[1] && !fresh32.selected[2] && !fresh32.selected[3] && fresh32.selected[5]);
    static_assert(P::AssessStyle({P::Style::TrustedSystem, P::Session::OtherLive, false, false}) == P::StyleStep::Preserve);
    static_assert(P::AssessStyle({P::Style::Unknown, P::Session::OtherLive, false, false}) == P::StyleStep::Inspect);
    static_assert(P::AssessStyle({P::Style::ResetRequired, P::Session::OtherLive, false, false}) == P::StyleStep::WaitForLogoff);
    static_assert(P::AssessStyle({P::Style::ResetRequired, P::Session::OtherLive, true, false}) == P::StyleStep::ApplyAndVerify);
    static_assert(P::AssessStyle({P::Style::ResetRequired, P::Session::Offline, false, true}) == P::StyleStep::ResetOfflineAndVerify);
    static_assert(P::AssessStyle({P::Style::ResetRequired, P::Session::Offline, false, false}) == P::StyleStep::Inspect);
    static_assert(P::AssessStyle({P::Style::ResetRequired, P::Session::Current, false, false}) == P::StyleStep::ApplyAndVerify);

    // Exhaustive six-product presence/selection matrix. No automatic cascade,
    // including Settings/SUT, and Keep means still installed after Setup.
    std::size_t cases = 0;
    for (bool os64 : {false, true}) for (auto mode : {P::Mode::Install, P::Mode::RemoveInteractive})
    for (unsigned present = 0; present < 64; ++present) for (unsigned selected = 0; selected < 64; ++selected) {
        // Seventh row is append-only: existing IPC/removal bit positions stay fixed.
    static_assert(P::Index(P::Product::OpenGlassResources)==6);
    static_assert(P::Index(P::Product::Settings32)==5);
    static_assert(!P::SupportedOnOs(P::Product::OpenGlassResources,false));
    static_assert(P::SupportedOnOs(P::Product::OpenGlassResources,true));
    // Complete core/add-on presence, explicit intent, and architecture matrix.
    for(bool os64:{false,true})for(unsigned present=0;present<4;++present)
    for(unsigned coreIntent=0;coreIntent<4;++coreIntent)for(unsigned graphicsIntent=0;graphicsIntent<4;++graphicsIntent) {
        auto f=KnownFacts();P::Request r;r.os64=os64;
        const auto og=P::Index(P::Product::OpenGlass),res=P::Index(P::Product::OpenGlassResources);
        f[og].presence=(present&1)?P::Presence::Installed:P::Presence::Absent;
        f[res].presence=(present&2)?P::Presence::Installed:P::Presence::Absent;
        const auto intent=[](unsigned i){return i==0?P::Action::Keep:i==1?P::Action::Install:i==2?P::Action::Remove:P::Action::Repair;};
        r.actions[og]=intent(coreIntent);r.actions[res]=intent(graphicsIntent);
        const auto p=P::Build(f,r);
        if(p.valid) {
            const auto& graphics=p.rows[res];
            if(P::IsMutation(graphics.action)&&graphics.action!=P::Action::Remove)
                assert(os64&&p.rows[og].after==P::Presence::Installed);
            if(p.rows[og].action==P::Action::Remove && graphicsIntent==0)assert(graphics.after==f[res].presence);
        }
        if(graphicsIntent==0)assert(!P::IsMutation(p.rows[res].action)); // no removal cascade
        if(coreIntent==0)assert(!P::IsMutation(p.rows[og].action)); // no prerequisite auto-apply
    }
    {
        auto f=KnownFacts();P::Request r;
        r.selected[P::Index(P::Product::OpenGlassResources)]=true;
        auto p=P::Build(f,r);assert(!p.valid&&p.rows[6].error==P::Error::OpenGlassRequired);
        r.selected[3]=true;f[3].source=P::Source::Invalid;
        p=P::Build(f,r);assert(!p.valid&&p.rows[6].error==P::Error::OpenGlassRequired);
        f[3].source=P::Source::Verified;
        p=P::Build(f,r);assert(p.valid&&p.rows[6].action==P::Action::Install);
        f[3].presence=P::Presence::Installed;r.selected[3]=false;
        p=P::Build(f,r);assert(p.valid&&p.rows[3].action==P::Action::Keep);
        f[6].presence=P::Presence::Installed;r.selected[6]=false;r.actions[3]=P::Action::Remove;
        p=P::Build(f,r);assert(p.valid&&p.rows[3].action==P::Action::Remove&&p.rows[6].action==P::Action::Keep);
        r.actions[6]=P::Action::Remove;p=P::Build(f,r);assert(p.valid&&p.rows[6].action==P::Action::Remove);
        f[3].presence=P::Presence::Absent;r.actions[3]=P::Action::None;
        p=P::Build(f,r);assert(p.valid&&p.rows[6].action==P::Action::Remove); // orphan add-on removal
    }
    Facts facts = KnownFacts();
        P::Request request;
        request.os64 = os64;
        request.mode = mode;
        for (std::size_t i = 0; i < P::kProductCount; ++i) {
            facts[i].presence = (present & (1u << i)) ? P::Presence::Installed : P::Presence::Absent;
            facts[i].activity = facts[i].presence == P::Presence::Installed ? P::Activity::Active : P::Activity::Inactive;
            request.selected[i] = (selected & (1u << i)) != 0;
        }
        const auto plan = P::Build(facts, request);
        for (std::size_t i = 0; i < P::kProductCount; ++i) {
            const auto& row = plan.rows[i];
            if (!request.selected[i]) {
                assert(row.after == facts[i].presence);
                assert(!P::IsMutation(row.action));
            } else if (!os64 && (i == 0 || i == 2 || i == 3) &&
                (mode == P::Mode::Install || facts[i].presence == P::Presence::Installed)) {
                assert(row.error == P::Error::Unsupported);
            } else if (mode == P::Mode::Install) {
                assert(row.after == P::Presence::Installed);
                assert(row.action == (facts[i].presence == P::Presence::Installed ? P::Action::Repair : P::Action::Install));
            } else {
                assert(row.after == P::Presence::Absent);
                assert(row.action == (facts[i].presence == P::Presence::Installed ? P::Action::Remove : P::Action::None));
            }
        }
        assert(P::Has(plan.rows[0].warnings, P::Warning::NoPlatform64) ==
            (os64 && plan.rows[0].after == P::Presence::Absent));
        assert(P::Has(plan.rows[1].warnings, P::Warning::NoPlatform32) ==
            (plan.rows[1].after == P::Presence::Absent));
        assert(P::Has(plan.rows[2].warnings, P::Warning::ManualSettings) ==
            (plan.rows[2].after == P::Presence::Absent && plan.rows[5].after == P::Presence::Absent));
        assert(P::Has(plan.rows[3].warnings, P::Warning::ManualOpenGlass) ==
            (plan.rows[3].after == P::Presence::Installed && plan.rows[2].after == P::Presence::Absent && plan.rows[5].after == P::Presence::Absent));
        assert(plan.prepareVisualStyleReset == (plan.valid && plan.rows[4].action == P::Action::Remove));
        ++cases;
    }

    Facts facts = KnownFacts();
    P::Request request;
    request.selected[4] = true;
    auto plan = P::Build(facts, request);
    assert(plan.valid && plan.rows[4].action == P::Action::Install);
    assert(plan.rows[2].action == P::Action::None); // Settings not auto-added.
    assert(P::Has(plan.rows[4].warnings, P::Warning::ManualVisualStyles));
    facts[2].presence = P::Presence::Installed;
    plan = P::Build(facts, request);
    assert(!P::Has(plan.rows[4].warnings, P::Warning::ManualVisualStyles)); // Keep satisfies management availability.

    facts[4].presence = P::Presence::Installed;
    facts[4].activity = P::Activity::Active;
    request.mode = P::Mode::RemoveInteractive;
    request.selected = {false, false, true, false, false};
    plan = P::Build(facts, request);
    assert(plan.valid && plan.rows[2].action == P::Action::Remove && plan.rows[4].action == P::Action::Keep);
    assert(P::Has(plan.rows[4].warnings, P::Warning::ManualVisualStyles));
    assert(!plan.prepareVisualStyleReset); // Do not disable SUT when only Settings is removed.
    request.mode = P::Mode::RemoveSilent;
    plan = P::Build(facts, request);
    assert(!plan.valid && plan.rows[2].error == P::Error::SettingsRemovalNeedsInteractivePlan);
    request.selected[4] = true;
    assert(!P::Build(facts, request).valid); // Even joint removal requires explicit UI.
    request.selected[2] = false;
    assert(P::Build(facts, request).prepareVisualStyleReset); // SUT alone remains removable headlessly after safe-style checks.
    facts[4].presence = P::Presence::Unknown;
    request.selected = {false, false, true, false, false};
    assert(!P::Build(facts, request).valid);

    // Missing discovery never authorizes mutations.
    Facts unknown{};
    P::Request unknownRequest;
    unknownRequest.selected[0] = true;
    assert(!P::Build(unknown, unknownRequest).valid);
    assert(!P::SelectAll(unknown, unknownRequest).selected[0]);

    // Mutation gates do not affect retained unrelated products.
    facts = KnownFacts();
    request = {};
    request.selected[0] = true;
    const auto checkError = [&](P::Error expected) {
        auto p = P::Build(facts, request);
        assert(!p.valid && p.rows[0].action == P::Action::Blocked && p.rows[0].error == expected);
    };
    facts[0].presence = P::Presence::Unknown; checkError(P::Error::UnknownPresence);
    facts[0] = KnownProduct(); facts[0].ownership = P::Ownership::External; checkError(P::Error::ExternalOwnership);
    facts[0].ownership = P::Ownership::Unknown; checkError(P::Error::UnknownOwnership);
    facts[0] = KnownProduct(); facts[0].supported = false; checkError(P::Error::Unsupported);
    facts[0] = KnownProduct(); facts[0].condition = P::Condition::Unverifiable; checkError(P::Error::Unverifiable);
    facts[0] = KnownProduct(); facts[0].transition = P::Transition::Busy; checkError(P::Error::Busy);
    facts[0].transition = P::Transition::RebootPending; checkError(P::Error::RebootPending);
    facts[0].transition = P::Transition::RecoveryRequired; checkError(P::Error::RecoveryRequired);
    facts[0] = KnownProduct(); facts[0].source = P::Source::Missing; checkError(P::Error::MissingSource);
    facts[0].source = P::Source::Invalid; checkError(P::Error::InvalidSource);
    facts[0] = KnownProduct(); facts[0].presence = P::Presence::Installed; facts[0].version = P::Version::Newer; checkError(P::Error::Downgrade);
    facts[0].version = P::Version::Unknown; checkError(P::Error::UnknownVersion);
    facts[0].version = P::Version::Older;
    assert(P::Build(facts, request).rows[0].action == P::Action::Update);
    facts[0].condition = P::Condition::Damaged;
    request.selected[0] = false;
    assert(P::Has(P::Build(facts, request).rows[0].warnings, P::Warning::DamageRetained));
    request.selected[0] = true;
    request.mode = P::Mode::RemoveInteractive;
    facts[0].source = P::Source::Missing;
    assert(P::Build(facts, request).rows[0].action == P::Action::Remove);
    request.cleanupAllProfiles = true;
    auto all = P::SelectAll(facts, request);
    assert(all.cleanupAllProfiles); // Explicit consent preserved, not overwritten.
    assert(!all.selected[1]); // Absent removal not selected.
    assert(P::Build(facts, all).cleanupAllProfiles);
    request.cleanupAllProfiles = false;
    all = P::SelectAll(facts, request);
    assert(!all.cleanupAllProfiles);
    request.mode = P::Mode::RemoveSilent;
    request.cleanupAllProfiles = true;
    assert(!P::Build(facts, request).cleanupAllProfiles);
    facts = KnownFacts();
    request = {};
    facts[0].source = P::Source::Invalid;
    facts[1].ownership = P::Ownership::External;
    facts[2].presence = P::Presence::Installed; facts[2].version = P::Version::Newer;
    facts[3].transition = P::Transition::Busy;
    all = P::SelectAll(facts, request);
    assert(!all.selected[0] && !all.selected[1] && !all.selected[2] && !all.selected[3] && all.selected[4]);

    // Trusted system styles never require logoff, including another user's live
    // session or an unavailable hive. Unknown never becomes trusted by default.
    const P::StyleFacts safe[] = {
        {P::Style::TrustedSystem, P::Session::Current, false, false},
        {P::Style::TrustedSystem, P::Session::OtherLive, false, false},
        {P::Style::TrustedSystem, P::Session::Offline, false, false}
    };
    auto styles = P::AssessStyles(safe, 3, true);
    assert(styles.readyToRemove && styles.preserve == 3 && styles.wait == 0);
    assert(!P::AssessStyles(safe, 3, false).readyToRemove);
    const P::StyleFacts mixed[] = {
        {P::Style::TrustedSystem, P::Session::OtherLive, false, false},
        {P::Style::ResetRequired, P::Session::OtherLive, false, false},
        {P::Style::ResetRequired, P::Session::Offline, false, true},
        {P::Style::Unknown, P::Session::Offline, false, true},
        {P::Style::ResetRequired, P::Session::Current, false, false}
    };
    styles = P::AssessStyles(mixed, 5, true);
    assert(!styles.readyToRemove && styles.preserve == 1 && styles.wait == 1 &&
        styles.offlineReset == 1 && styles.inspect == 1 && styles.liveApply == 1);
    assert(!P::AssessStyles(nullptr, 2, true).readyToRemove);
    std::cout << "PASS: " << cases << " presence/selection/OS/mode cases; mutation gates, warnings, bulk selection and style preflight\n";
}
