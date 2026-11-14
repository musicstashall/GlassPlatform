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
inline std::wstring KnownPublishedReleaseRiskText(bool russian,unsigned windowsBuild=26300) {
    if(windowsBuild==26200)return russian?
        L"Опасность применения: при установке OpenGlass 3.0.2.3749 учитывайте сообщения о сбоях DWM на Windows 11 25H2 (26200); возможны чёрный экран, нестабильная работа Проводника и сеанса. Применение этой версии не рекомендуется. Совместимость зависит от точной ревизии Windows и доступности символов; отметка 25H2 в таблице разработчика не гарантирует работу опубликованного бинарника. Обсуждение: https://github.com/ALTaleX531/OpenGlass/issues/362 ; https://github.com/ALTaleX531/OpenGlass/issues/387":
        L"Danger: when installing OpenGlass 3.0.2.3749, consider reports of DWM crashes on Windows 11 25H2 (26200); black screens, Explorer and session instability are possible. Applying this version is not recommended. Compatibility depends on the exact Windows revision and symbol availability; the developer's 25H2 support table does not guarantee this published binary will work. Reports: https://github.com/ALTaleX531/OpenGlass/issues/362 ; https://github.com/ALTaleX531/OpenGlass/issues/387";
    return russian?
        L"Опасность применения: разработчик предупреждает о серьёзных проблемах OpenGlass 3.0.2.3749 на Windows 11 26H2 (26300; отдельно отмечена 26300.9818). Возможны сбои DWM, мерцание/чёрный экран и повторный возврат к экрану входа. Не рекомендуем применять эту версию. Предупреждение разработчика: https://github.com/ALTaleX531/OpenGlass/releases/tag/v3.0.2.3749":
        L"Danger: the developer warns of severe OpenGlass 3.0.2.3749 issues on Windows 11 26H2 (26300; specifically 26300.9818). DWM crashes, flickering/black screens and repeated returns to the sign-in screen are possible. Applying this version is not recommended. Developer warning: https://github.com/ALTaleX531/OpenGlass/releases/tag/v3.0.2.3749";
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
