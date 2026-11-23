#pragma once
#include "GlassProductPlan.h"
#include "GlassProducts.h"
#include <string>
#include <windows.h>
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
    HINSTANCE instance=GetModuleHandleW(nullptr);
    HRSRC res=FindResourceExW(instance,RT_STRING,MAKEINTRESOURCEW((IDS_COMP_SHELL_SUFFIX>>4)+1),
        ru?MAKELANGID(LANG_RUSSIAN,SUBLANG_DEFAULT):MAKELANGID(LANG_ENGLISH,SUBLANG_ENGLISH_US));
    if(!res)return {};
    auto* entry=static_cast<const WORD*>(LockResource(LoadResource(instance,res)));
    if(!entry)return {};
    for(UINT i=0;i<(IDS_COMP_SHELL_SUFFIX&15);++i)entry+=1+*entry;
    return std::wstring(reinterpret_cast<const wchar_t*>(entry+1),*entry);
}
inline std::wstring PlatformDescription(std::wstring base,bool platform64,bool os64,bool ru) {
    if(platform64==os64)base+=ShellSuffix(ru);
    return base+L".";
}
inline std::wstring WarningText(GlassProductPlan::Warning warning, bool ru, bool os64=true) {
    using W=GlassProductPlan::Warning;
    std::wstring text;
    const auto add=[&](W flag,const wchar_t* russian,const wchar_t* english) {
        if (!GlassProductPlan::Has(warning,flag)) return;
        if (!text.empty()) text+=L"\n\n";
        text+=ru?russian:english;
    };
    const auto platformWarning=[&](W flag,bool platform64) {
        if(!GlassProductPlan::Has(warning,flag))return;
        if(!text.empty())text+=L"\n\n";
        text+=ru?(platform64?L"Без Platform x64 не будет оформления 64-битных приложений":L"Без Platform x86 не будет оформления 32-битных приложений"):
            (platform64?L"Without Platform x64, styling is unavailable for 64-bit applications":L"Without Platform x86, styling is unavailable for 32-bit applications");
        if(platform64==os64)text+=ShellSuffix(ru);
        text+=ru?L". Остальные продукты от этого не зависят.":L". Other products don't depend on it.";
    };
    platformWarning(W::NoPlatform64,true);
    platformWarning(W::NoPlatform32,false);
    // NoHelper* заменяет отдельную страницу риска при удалении GlassSettings:
    // без Settings своей разрядности Platform нельзя безопасно выгрузить позже.
    add(W::NoHelper64,L"Platform x64 останется без GlassSettings x64. Тогда при обновлении или удалении Platform x64 её не удастся безопасно отключить от запущенных программ: может понадобиться перезагрузка, иначе возможны сбои программ. Рекомендуем установить или оставить GlassSettings x64 — версия x86 его не заменяет.",
        L"Platform x64 will be left without GlassSettings x64. When Platform x64 is later updated or removed, it can't be safely detached from running applications: a restart may be needed, otherwise applications may crash. We recommend installing or keeping GlassSettings x64 — the x86 version doesn't replace it.");
    add(W::NoHelper32,L"Platform x86 останется без GlassSettings x86. Тогда при обновлении или удалении Platform x86 её не удастся безопасно отключить от запущенных программ: может понадобиться перезагрузка, иначе возможны сбои программ. Рекомендуем установить или оставить GlassSettings x86 — версия x64 его не заменяет.",
        L"Platform x86 will be left without GlassSettings x86. When Platform x86 is later updated or removed, it can't be safely detached from running applications: a restart may be needed, otherwise applications may crash. We recommend installing or keeping GlassSettings x86 — the x64 version doesn't replace it.");
    add(W::ManualSettings,L"Без GlassSettings не останется программы настроек: включать, выключать и настраивать оформление придётся вручную, через реестр Windows.",
        L"Without GlassSettings there is no settings app: styling has to be turned on, off and adjusted manually in the Windows registry.");
    add(W::ManualOpenGlass,L"Без GlassSettings OpenGlass придётся включать, выключать и настраивать вручную.",
        L"Without GlassSettings, OpenGlass has to be turned on, off and adjusted manually.");
    add(W::ManualVisualStyles,L"SecureUxTheme останется без GlassSettings. Темы оформления придётся переключать вручную, иногда с перезапуском Windows. Перед удалением SecureUxTheme сначала верните стандартную тему Windows.",
        L"SecureUxTheme will be left without GlassSettings. Visual styles will have to be switched manually, sometimes with a Windows restart. Before removing SecureUxTheme, switch back to the standard Windows theme first.");
    add(W::DamageRetained,L"Этот продукт повреждён и не будет исправлен.",L"This product is damaged and will not be repaired.");
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
