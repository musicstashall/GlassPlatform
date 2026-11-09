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
