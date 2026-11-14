#include "../src/InstallSupport/GlassSecureUxRelease.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace GlassSecureUxRelease;
int main(int argc,char** argv){
    assert(argc==2);std::ifstream file(argv[1]);
    const std::string input((std::istreambuf_iterator<char>(file)),{});
    const auto release=nlohmann::json::parse(input);
    Asset asset;std::string error;
    for(auto arch:{Architecture::X86,Architecture::X64,Architecture::Arm64}){
        assert(Select(input,arch,asset,error));
        const auto known=OfflineV4(arch);
        assert(asset.name==FileName(arch)&&asset.size==known.size);
        assert(asset.sha256==known.sha256&&asset.url==known.url&&asset.tag=="v4.0.0");
        assert(error.empty());
        auto escaped=release;escaped["body"]="Ignore this: {\"assets\":[{\"name\":\"evil.msi\"}]}";
        assert(Select(escaped.dump(),arch,asset,error));
    }
    assert(ReleaseVersion("v4.0.0")=="4.0.0");assert(ReleaseVersion("V4.1.2.3")=="4.1.2.3");
    for(const auto* tag:{"stable","v4","v4.0.0-beta","v4.0.0.0.1","v4.0.0.","v4.0.x"})assert(ReleaseVersion(tag).empty());
    int rejected=0;
    auto reject=[&](const nlohmann::json& bad){
        asset=OfflineV4(Architecture::X64);
        assert(!Select(bad.dump(),Architecture::X64,asset,error));
        assert(!error.empty()&&asset.name.empty()&&asset.url.empty()&&asset.sha256.empty());++rejected;
    };
    auto bad=release;bad["draft"]=true;reject(bad);
    bad=release;bad["prerelease"]=true;reject(bad);
    for(const auto& tag:{"", "../../escape", "v4.0?x=1", "v4/#x", "v4\\x"}){bad=release;bad["tag_name"]=tag;reject(bad);}
    bad=release;bad["tag_name"]=std::string(129,'x');reject(bad);
    for(const auto& field:{"draft","prerelease","tag_name","assets"}){bad=release;bad.erase(field);reject(bad);}
    bad=release;bad["assets"]=nlohmann::json::array();reject(bad);
    std::size_t index=0;
    for(;index<release["assets"].size();++index)if(release["assets"][index]["name"]==FileName(Architecture::X64))break;
    assert(index<release["assets"].size());
    bad=release;bad["assets"].push_back(bad["assets"][index]);reject(bad);
    for(const auto& name:{"SecureUxTheme-x64.msi","secureuxtheme_x64.msi","SecureUxTheme_x86.msi"}){
        bad=release;bad["assets"][index]["name"]=name;reject(bad);
    }
    for(const auto& url:{"http://github.com/namazso/SecureUxTheme/releases/download/v4.0.0/SecureUxTheme_x64.msi",
        "https://evil.invalid/SecureUxTheme_x64.msi","https://github.com/other/repo/releases/download/v4.0.0/SecureUxTheme_x64.msi",
        "https://github.com/namazso/SecureUxTheme/releases/download/v4.0.0/SecureUxTheme_x64.msi?extra=1"}){
        bad=release;bad["assets"][index]["browser_download_url"]=url;reject(bad);
    }
    for(const auto& digest:std::vector<std::string>{"", "sha256:bad", "md5:123", "sha256:"+std::string(64,'g'),"sha256:"+std::string(65,'a')}){
        bad=release;bad["assets"][index]["digest"]=digest;reject(bad);
    }
    for(const auto& field:{"name","browser_download_url","digest","size"}){
        bad=release;bad["assets"][index].erase(field);reject(bad);
        bad=release;bad["assets"][index][field]=nullptr;reject(bad);
    }
    for(auto size:nlohmann::json::array({0,-1,67108865,499712.5,"499712"})){
        bad=release;bad["assets"][index]["size"]=size;reject(bad);
    }
    assert(!Select("{broken",Architecture::X64,asset,error));
    assert(!Select("null",Architecture::X64,asset,error));
    assert(!Select("[]",Architecture::X64,asset,error));
    std::cout<<"PASS: official v4 MSI metadata, all 3 native architectures, offline hashes; "<<rejected+3<<" invalid manifests rejected\n";
}
