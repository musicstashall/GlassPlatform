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
        text+=ru?L". Остальные продукты работают независимо. Продолжение разрешено.":L". Other products are independent. You can continue.";
    };
    platformWarning(W::NoPlatform64,true);
    platformWarning(W::NoPlatform32,false);
    add(W::NoHelper64,L"Platform x64 остаётся без GlassSettings x64. При последующем обслуживании может быть недоступен доверенный Native x64 для контроля и выгрузки модулей. Разрядности независимы; установка Settings x86 не заменяет helper x64.",L"Platform x64 will remain without GlassSettings x64. A trusted Native x64 helper for module monitoring and unloading may be unavailable during later maintenance. Architectures are independent; Settings x86 does not replace the x64 helper.");
    add(W::NoHelper32,L"Platform x86 остаётся без GlassSettings x86. При последующем обслуживании может быть недоступен доверенный Native x86 для контроля и выгрузки модулей. Разрядности независимы; установка Settings x64 не заменяет helper x86.",L"Platform x86 will remain without GlassSettings x86. A trusted Native x86 helper for module monitoring and unloading may be unavailable during later maintenance. Architectures are independent; Settings x64 does not replace the x86 helper.");
    add(W::ManualSettings,L"Без GlassSettings потребуется вручную редактировать реестр и управлять службами и задачами. Продолжение разрешено.",L"Without GlassSettings, registry settings, services and scheduled tasks must be managed manually. You can continue.");
    add(W::ManualOpenGlass,L"Без GlassSettings настройка OpenGlass и управление его службами и задачами выполняются вручную. Продолжение разрешено.",L"Without GlassSettings, OpenGlass registry settings, services and scheduled tasks must be managed manually. You can continue.");
    add(W::ManualVisualStyles,L"SecureUxTheme остаётся без GlassSettings. Вы принимаете ответственность за ручной контроль визуальных стилей и перезапуск Windows. Перед удалением SecureUxTheme необходимо вернуть штатный визуальный стиль.",L"SecureUxTheme will operate without GlassSettings. You accept responsibility for managing visual styles and restarting Windows manually. Restore a stock visual style before removing SecureUxTheme.");
    add(W::DamageRetained,L"Повреждённый продукт остаётся без восстановления.",L"A damaged product is being retained without repair.");
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
