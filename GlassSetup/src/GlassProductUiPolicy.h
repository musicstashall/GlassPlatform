#pragma once
#include "GlassProductPlan.h"
#include "GlassProducts.h"
#include <string>
#include "WizardUi/Window.h"
#include "../res/resource.h"

namespace GlassProductUiPolicy {
// Discovery/installer execution still perform MSI identity/path/source validation.
// This adapter describes the selection after-state; it does not grant trust to files.
inline GlassProductPlan::Plan Preview(const GlassProducts::Selection& choice,
    const GlassProducts::Selection& installed, bool os64, bool removal, bool sutActive) {
    namespace P = GlassProductPlan;
    const std::array<bool, P::kProductCount> present{installed.platform64, installed.platform32,
        installed.Settings64(os64), installed.openGlass, installed.secureUxTheme, installed.Settings32(os64), installed.openGlassResources};
    P::Request request;
    request.os64=os64;
    request.mode=removal?P::Mode::RemoveInteractive:P::Mode::Install;
    request.selected={choice.platform64,choice.platform32,choice.Settings64(os64),choice.openGlass,choice.secureUxTheme,choice.Settings32(os64), choice.openGlassResources};
    if(!removal)for(std::size_t i=0;i<P::kProductCount;++i)
        if(choice.Removes(static_cast<unsigned>(i)))request.actions[i]=P::Action::Remove;
    if(!removal && choice.secureUxThemeRemove)request.actions[P::Index(P::Product::SecureUxTheme)]=P::Action::Remove;
    std::array<P::Facts,P::kProductCount> facts{};
    for (std::size_t i=0;i<facts.size();++i) {
        auto& f=facts[i];
        f.supported=P::SupportedOnOs(static_cast<P::Product>(i),os64);
        f.presence=present[i]?P::Presence::Installed:P::Presence::Absent;
        f.ownership=P::Ownership::Managed;
        f.condition=P::Condition::Healthy;
        f.version=P::Version::Same;
        f.source=P::Source::Verified;
        f.activity=i==4&&sutActive?P::Activity::Active:P::Activity::Inactive;
    }
    auto plan=P::Build(facts,request);
    // An empty installed inventory has no retained-product dependencies to
    // warn about. This is preview-only; execution still validates the plan.
    bool hasInstalled=false;
    for(bool value:present)hasInstalled=hasInstalled||value;
    if(!hasInstalled&&!removal) {
        plan.warnings=P::Warning::None;
        for(auto& row:plan.rows)row.warnings=P::Warning::None;
    } else {
        // Absent unselected products are not dependencies of this plan.
        // Preserve warnings on retained/selected products, never advertise a
        // missing platform/helper on every unrelated Available card.
        plan.warnings=P::Warning::None;
        for(std::size_t i=0;i<plan.rows.size();++i) {
            if(!present[i]&&!request.selected[i]&&request.actions[i]!=P::Action::Remove)
                plan.rows[i].warnings=P::Warning::None;
            plan.warnings=plan.warnings|plan.rows[i].warnings;
        }
    }
    return plan;
}
inline std::wstring ShellSuffix(bool ru) {
    const LANGID language=ru?MAKELANGID(LANG_RUSSIAN,SUBLANG_DEFAULT):MAKELANGID(LANG_ENGLISH,SUBLANG_ENGLISH_US);
    return WizardUi::LoadText(GetModuleHandleW(nullptr),language,IDS_COMP_SHELL_SUFFIX);
}
inline std::wstring PlatformDescription(std::wstring base,bool platform64,bool os64,bool ru) {
    if(platform64==os64)base+=ShellSuffix(ru);
    return base+L".";
}
inline std::wstring WarningText(GlassProductPlan::Warning warning, bool ru, bool os64=true) {
    using W=GlassProductPlan::Warning;
    std::wstring text;
    const HINSTANCE instance=GetModuleHandleW(nullptr);
    const LANGID language=ru?MAKELANGID(LANG_RUSSIAN,SUBLANG_DEFAULT):MAKELANGID(LANG_ENGLISH,SUBLANG_ENGLISH_US);
    const auto add=[&](W flag,UINT id) {
        if (!GlassProductPlan::Has(warning,flag)) return;
        if (!text.empty()) text+=L"\n\n";
        text+=WizardUi::LoadText(instance,language,id);
    };
    const auto platformWarning=[&](W flag,bool platform64) {
        if(!GlassProductPlan::Has(warning,flag))return;
        add(flag,platform64?IDS_PLAN_WARN_NO_PLATFORM64:IDS_PLAN_WARN_NO_PLATFORM32);
        if(platform64==os64)text+=ShellSuffix(ru);
        text+=WizardUi::LoadText(instance,language,IDS_PLAN_WARN_NO_PLATFORM_SUFFIX);
    };
    platformWarning(W::NoPlatform64,true);
    platformWarning(W::NoPlatform32,false);
    // NoHelper* заменяет отдельную страницу риска при удалении GlassSettings:
    // без Settings своей разрядности Platform нельзя безопасно выгрузить позже.
    add(W::NoHelper64,IDS_PLAN_WARN_NO_HELPER64);
    add(W::NoHelper32,IDS_PLAN_WARN_NO_HELPER32);
    add(W::ManualSettings,IDS_PLAN_WARN_MANUAL_SETTINGS);
    add(W::ManualOpenGlass,IDS_PLAN_WARN_MANUAL_OPENGLASS);
    add(W::ManualVisualStyles,IDS_PLAN_WARN_MANUAL_VISUAL_STYLES);
    add(W::DamageRetained,IDS_PLAN_WARN_DAMAGE_RETAINED);
    return text;
}
inline std::size_t Index(const wchar_t* id) {
    if (std::wstring(id)==L"x64") return 0;
    if (std::wstring(id)==L"x86") return 1;
    if (std::wstring(id)==L"settings" || std::wstring(id)==L"settings64") return 2;
    if (std::wstring(id)==L"settings32") return 5;
    if (std::wstring(id)==L"openglassResources") return 6;
    if (std::wstring(id)==L"openglass") return 3;
    return 4;
}
}
