#pragma once
#include "GlassProductPlan.h"
#include <array>

// Shared row scheduler for Setup and Unins. It never expands a selection or
// infers dependencies: only mutations explicitly authorized by the plan run.
// Adapters own Windows-specific snapshots, registration and style preparation.
namespace GlassMaintenanceExecutor {
namespace P=GlassProductPlan;
enum class Phase { Helpers, Products, Vendor };
constexpr Phase PhaseOf(P::Product product,P::Action action) noexcept {
    if(action!=P::Action::Remove && (product==P::Product::Settings64 || product==P::Product::Settings32))return Phase::Helpers;
    if(action!=P::Action::Remove && product==P::Product::SecureUxTheme)return Phase::Vendor;
    return Phase::Products;
}
constexpr std::array<P::Product,P::kProductCount> kRemovalOrder{
    P::Product::SecureUxTheme,P::Product::OpenGlass,P::Product::Platform32,
    P::Product::Platform64,P::Product::Settings64,P::Product::Settings32};
constexpr std::array<P::Product,P::kProductCount> kInstallOrder{
    P::Product::Settings64,P::Product::Settings32,P::Product::Platform64,
    P::Product::Platform32,P::Product::OpenGlass,P::Product::SecureUxTheme};
struct Result {
    bool ok=true;
    unsigned error=0;
    P::Product product=P::Product::Count;
    std::size_t completed=0;
    // The failed callback may already have changed MSI/registry state. Adapters
    // must include it in recovery, not only rows that returned success.
    std::uint32_t attemptedMask=0;
    std::uint32_t completedMask=0;
};
// Validation/snapshots for the entire request must finish before any phase is
// entered. A failed callback may have mutated MSI state: retain its journal and
// invoke the adapter's whole-session rollback, not just the completed rows.
template<class Apply>
Result RunPhase(const P::Plan& plan,Phase phase,Apply apply) {
    Result result;
    if(!plan.valid){result.ok=false;result.error=87;return result;}
    for(bool removal:{true,false}) {
        const auto& order=removal?kRemovalOrder:kInstallOrder;
        for(auto product:order) {
            const auto action=plan.rows[P::Index(product)].action;
            if(!P::IsMutation(action) || (action==P::Action::Remove)!=removal || PhaseOf(product,action)!=phase)continue;
            const auto bit=std::uint32_t{1}<<P::Index(product);
            result.attemptedMask|=bit;
            const unsigned code=apply(product,action);
            if(code){result.ok=false;result.error=code;result.product=product;return result;}
            result.completedMask|=bit;
            ++result.completed;
        }
    }
    return result;
}
}
