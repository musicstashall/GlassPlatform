#include "../src/GlassMaintenanceExecutor.h"
#include <cassert>
#include <vector>
#include <iostream>
namespace P=GlassProductPlan;
namespace E=GlassMaintenanceExecutor;
int main(){
    std::size_t cases=0;
    for(unsigned encoded=0;encoded<4096;++encoded) {
        std::array<P::Facts,P::kProductCount> facts{};
        P::Request request;
        unsigned bits=encoded;
        for(std::size_t i=0;i<facts.size();++i) {
            facts[i]={true,P::Presence::Installed,P::Ownership::Managed,P::Condition::Healthy,P::Version::Same,P::Source::Verified,P::Transition::Idle,P::Activity::Inactive};
            const unsigned choice=bits&3;bits>>=2;
            request.actions[i]=choice==0?P::Action::Keep:choice==1?P::Action::Remove:choice==2?P::Action::Repair:P::Action::Update;
            if(choice==3)facts[i].version=P::Version::Older;
        }
        const auto plan=P::Build(facts,request);
        assert(plan.valid);
        std::array<unsigned,P::kProductCount> seen{};
        for(auto phase:{E::Phase::Helpers,E::Phase::Products,E::Phase::Vendor}) {
            auto result=E::RunPhase(plan,phase,[&](P::Product p,P::Action a)->unsigned {
                const auto i=P::Index(p);assert(a!=P::Action::Keep);assert(request.actions[i]!=P::Action::Keep);
                assert(++seen[i]==1);assert(E::PhaseOf(p,a)==phase);return 0;
            });assert(result.ok);
        }
        for(std::size_t i=0;i<seen.size();++i)assert(seen[i]==unsigned(request.actions[i]!=P::Action::Keep));
        ++cases;
    }
    P::Plan plan;
    for(auto& row:plan.rows)row.action=P::Action::Remove;
    std::vector<P::Product> sequence;
    auto result=E::RunPhase(plan,E::Phase::Products,[&](P::Product p,P::Action)->unsigned {sequence.push_back(p);return 0;});
    assert(result.ok && sequence.size()==6);
    assert(sequence==std::vector<P::Product>(E::kRemovalOrder.begin(),E::kRemovalOrder.end()));
    for(unsigned fail=0;fail<6;++fail) {
        unsigned count=0;
        result=E::RunPhase(plan,E::Phase::Products,[&](P::Product,P::Action)->unsigned {return count++==fail?1603:0;});
        assert(!result.ok && result.completed==fail && result.error==1603 && count==fail+1);
        std::uint32_t attempted=0,completed=0;
        for(unsigned i=0;i<=fail;++i)attempted|=1u<<P::Index(E::kRemovalOrder[i]);
        for(unsigned i=0;i<fail;++i)completed|=1u<<P::Index(E::kRemovalOrder[i]);
        assert(result.attemptedMask==attempted && result.completedMask==completed);
    }
    plan.valid=false;unsigned calls=0;
    result=E::RunPhase(plan,E::Phase::Products,[&](P::Product,P::Action)->unsigned {++calls;return 0;});
    assert(!result.ok && !calls);
    std::cout<<"PASS: "<<cases<<" explicit mixed plans; six independent products; shared phases/order; no added mutations; stop on each failure; invalid-plan refusal\n";
}
