#pragma once
#include "../../third_party/nlohmann/json.hpp"
#include <algorithm>
#include <cstdint>
#include <string>
#include <stdexcept>

namespace GlassSecureUxRelease {
enum class Architecture { X86, X64, Arm64 };
struct Asset {
    std::string tag, name, url, sha256;
    std::uint64_t size=0;
};
inline const char* FileName(Architecture arch) {
    return arch==Architecture::X64?"SecureUxTheme_x64.msi":
        arch==Architecture::Arm64?"SecureUxTheme_ARM64.msi":"SecureUxTheme_x86.msi";
}
inline const wchar_t* UpgradeCode(Architecture arch) {
    return arch==Architecture::X64?L"{965B5B74-4040-4E89-947F-AF8D99A76706}":
        arch==Architecture::Arm64?L"{E0E6C0EA-103B-4E8A-B1A9-1703853A49F4}":L"{3FF010A5-2E94-4621-A260-3A7ABCDDB37C}";
}
inline bool HexDigest(const std::string& value) {
    return value.size()==64&&std::all_of(value.begin(),value.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
inline bool Select(const std::string& text,Architecture arch,Asset& out,std::string& error) {
    out={};
    try {
        const auto release=nlohmann::json::parse(text);
        if(release.at("draft").get<bool>()||release.at("prerelease").get<bool>())throw std::runtime_error("Not a stable release");
        const auto tag=release.at("tag_name").get<std::string>();
        if(tag.empty()||tag.size()>128||!std::all_of(tag.begin(),tag.end(),[](char c){
            return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-';
        }))throw std::runtime_error("Unsupported release tag");
        bool found=false;
        for(const auto& asset:release.at("assets")) {
            if(asset.at("name").get<std::string>()!=FileName(arch))continue;
            if(found)throw std::runtime_error("Duplicate MSI asset");
            found=true;
            if(!asset.at("size").is_number_unsigned())throw std::runtime_error("Invalid asset size");
            Asset candidate{tag,FileName(arch),asset.at("browser_download_url").get<std::string>(),{},asset.at("size").get<std::uint64_t>()};
            const std::string expected="https://github.com/namazso/SecureUxTheme/releases/download/"+tag+"/"+candidate.name;
            if(candidate.url!=expected||!candidate.size||candidate.size>64*1024*1024)throw std::runtime_error("Unexpected MSI URL or size");
            const auto digest=asset.at("digest").get<std::string>();
            if(digest.rfind("sha256:",0)!=0||!HexDigest(digest.substr(7)))throw std::runtime_error("GitHub did not supply an SHA-256 digest");
            candidate.sha256=digest.substr(7);out=candidate;
        }
        if(!found)throw std::runtime_error("Native-architecture MSI asset is missing");
        error.clear();return true;
    } catch(const std::exception& e){out={};error=e.what();return false;}
}
// Official API metadata captured from v4.0.0. These hashes allow a manually
// supplied package to be used offline, without trusting its file name alone.
inline Asset OfflineV4(Architecture arch) {
    Asset asset{"v4.0.0",FileName(arch),{},
        arch==Architecture::X64?"af6ab67e0a283a0138b827b6731d5bb1f4ffc59e6d905f1c41567eb3305a9537":
        arch==Architecture::Arm64?"2e5cf62249abdb1d9f6c3b81567587bd13c2094a067b2fa753fde9bd31a14dfb":
        "ce9d803c1717ef2f67dc55a30dbdf5fc83a80cbd5e7cec5dc42304696c2eb816",
        arch==Architecture::X86?442368u:499712u};
    asset.url="https://github.com/namazso/SecureUxTheme/releases/download/"+asset.tag+"/"+asset.name;
    return asset;
}
}
