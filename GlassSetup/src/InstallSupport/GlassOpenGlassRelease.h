#pragma once
#include "GlassPackageRules.h"
#include "../../third_party/nlohmann/json.hpp"
#include <string>

namespace GlassOpenGlassRelease {
// Product update indication compares DLL payloads, never the Glass MSI wrapper.
inline bool PayloadUpdateAvailable(const std::wstring& installed,const std::wstring& offered) {
    for(const auto* text:{&installed,&offered})for(wchar_t c:*text)if(static_cast<unsigned>(c)>127)return false;
    GlassPackageRules::Version current{},target{};
    return GlassPackageRules::ParseVersion(std::string(installed.begin(),installed.end()),current)&&
        GlassPackageRules::ParseVersion(std::string(offered.begin(),offered.end()),target)&&
        GlassPackageRules::Compare(current,target)<0;
}
// Planning is not authorization to install. Discovery is best-effort; Next
// retries metadata, verifies/extracts the payload and rechecks downgrade rules.
inline bool CanPlanApply(const std::wstring& installed,const std::wstring& offered) {
    return offered.empty() || !PayloadUpdateAvailable(offered,installed);
}
// Reviewed published release, DISPLAY ONLY when live discovery is unavailable.
// Never use this value to authorize or select installation media.
inline std::wstring DisplayVersion(const std::wstring& discovered) {
    return discovered.empty()?L"3.0.2.3749":discovered;
}
// Upstream v3.0.2.3749 warns about 26H2; 25H2 has DWM crash reports
// (#362/#387) and local Explorer crash evidence. Scope to this release, not
// future fixed releases or all Windows 11. This is a warning, not a trust gate.
inline bool KnownPublishedReleaseRisk(unsigned windowsBuild,const std::wstring& offered) {
    if(windowsBuild!=26300&&windowsBuild!=26200)return false;
    GlassPackageRules::Version version{},affected{};
    for(wchar_t c:offered)if(static_cast<unsigned>(c)>127)return false;
    return GlassPackageRules::ParseVersion(std::string(offered.begin(),offered.end()),version)&&
        GlassPackageRules::ParseVersion("3.0.2.3749",affected)&&GlassPackageRules::Compare(version,affected)==0;
}
// Unknown upstream metadata must not silence the known-build safety warning.
inline bool ApplyRisk(unsigned build,const std::wstring& offered) {
    return KnownPublishedReleaseRisk(build,DisplayVersion(offered));
}
// Windows 11 23H2 is build 22631. Older systems can still receive OpenGlass,
// but the published builds are not treated as safe to start there.
inline bool OlderThan23H2(unsigned windowsBuild) {
    return windowsBuild!=0&&windowsBuild<22631;
}
inline std::wstring OlderThan23H2RiskText(bool russian) {
    return russian?
        L"Ваша Windows старше версии 11 23H2. На ней OpenGlass может нарушать отображение окон, работу Проводника и всего сеанса. OpenGlass будет установлен, но не будет запущен.":
        L"Your Windows is older than version 11 23H2. On it OpenGlass can break window display, Explorer and the whole session. OpenGlass will be installed but not started.";
}
inline std::wstring KnownPublishedReleaseRiskText(bool russian,unsigned windowsBuild=26300) {
    if(windowsBuild==26200)return russian?
        L"Не рекомендуется: пользователи сообщают, что OpenGlass 3.0.2.3749 на Windows 11 25H2 (26200) вызывает сбои отображения окон — возможны чёрный экран, нестабильная работа Проводника и всего сеанса. Даже если разработчик заявляет поддержку 25H2, работа этой версии на вашем компьютере не гарантируется. Подробнее: https://github.com/ALTaleX531/OpenGlass/issues/362 ; https://github.com/ALTaleX531/OpenGlass/issues/387":
        L"Not recommended: users report that OpenGlass 3.0.2.3749 on Windows 11 25H2 (26200) breaks window display — black screens and unstable Explorer and session are possible. Even though the developer lists 25H2 as supported, this version is not guaranteed to work on your computer. Details: https://github.com/ALTaleX531/OpenGlass/issues/362 ; https://github.com/ALTaleX531/OpenGlass/issues/387";
    return russian?
        L"Не рекомендуется: разработчик предупреждает о серьёзных проблемах OpenGlass 3.0.2.3749 на Windows 11 26H2 (26300, особенно 26300.9818). Возможны сбои отображения окон, мерцание или чёрный экран и повторный выход к экрану входа. Подробнее: https://github.com/ALTaleX531/OpenGlass/releases/tag/v3.0.2.3749":
        L"Not recommended: the developer warns of serious OpenGlass 3.0.2.3749 problems on Windows 11 26H2 (26300, especially 26300.9818). Window display failures, flickering or black screens and repeated returns to the sign-in screen are possible. Details: https://github.com/ALTaleX531/OpenGlass/releases/tag/v3.0.2.3749";
}
// Metadata only: release tags do not authorize installing a different MSI or EXE.
inline bool Parse(const std::string& text,std::string& version) noexcept {
    version.clear();
    try {
        if(text.size()>1024*1024)return false;
        const auto j=nlohmann::json::parse(text);
        if(!j.is_object()||j.value("draft",false)||j.value("prerelease",false))return false;
        auto tag=j.at("tag_name").get<std::string>();
        if(!tag.empty()&&(tag[0]=='v'||tag[0]=='V'))tag.erase(0,1);
        GlassPackageRules::Version parsed;
        if(!GlassPackageRules::ParseVersion(tag,parsed))return false;
        version=tag;return true;
    }catch(...){return false;}
}
}
