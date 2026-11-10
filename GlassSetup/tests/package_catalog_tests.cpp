#include "../src/InstallSupport/GlassPackageCatalog.h"
#include <cassert>
#include <iostream>
using namespace GlassPackageRules;
using nlohmann::json;
Version V(const char* text) { Version v; assert(ParseVersion(text,v)); return v; }
void Equal(const Choice& c, std::size_t index) { assert(c.failure==Failure::None && c.index && *c.index==index); }
json MakeAsset(const char* file, const char* version="1.2.3.4") {
    return {{"file",file},{"version",version},{"size",1024u},{"sha256",std::string(64,'a')},
        {"url",std::string("https://github.com/musicstashall/GlassPlatform/releases/download/v1.2.3/")+file}};
}
int main() {
    for (const char* text : {"1.2.3", "1.2.3.0", "1.2.3.4294967295"}) { Version v; assert(ParseVersion(text,v)); }
    for (const char* text : {"", "1", "1.2", ".1.2.3", "1.2.3.", "1.2..3", "1.2.3.4.5", "1.2.3.-1", "1.2.3.4294967296", " 1.2.3", "1.2.3x", "1.2.3\n"}) {
        Version v=V("9.9.9"); assert(!ParseVersion(text,v)); assert(Compare(v,Version{})==0);
    }
    assert(Compare(V("1.2.3"),V("1.2.3.0"))==0);
    assert(Compare(V("1.2.10"),V("1.2.9"))>0);
    std::vector<Candidate> candidates{{Source::Current,V("1.0.0"),true},{Source::LocalPackages,V("2.0.0"),true},
        {Source::LocalAdjacent,V("2.0.0"),true},{Source::Release,V("2.0.0"),true}};
    Equal(Select({V("1.0.0"),{}},candidates),1);
    Equal(Select({V("1.0.0"),V("1.0.0")},candidates),0);
    candidates[0].eligible=false;
    assert(Select({V("1.0.0"),V("1.0.0")},candidates).failure==Failure::NoSource);
    candidates[0].eligible=true;
    assert(Select({{},V("1.0.0")},candidates).failure==Failure::NoSource); // Cache is not a clean-install source.
    candidates[3].version=V("3.0.0");
    Equal(Select({V("1.0.0"),{}},candidates),3);
    candidates[1].version=V("4.0.0");
    Equal(Select({V("1.0.0"),{}},candidates),1); // Local dev newer than release.
    candidates[1].eligible=false;
    Equal(Select({V("1.0.0"),{}},candidates),3); // Invalid local doesn't defeat valid release.
    candidates[3].eligible=false;
    Equal(Select({V("1.0.0"),{}},candidates),2); // Offline local kit.
    assert(Select({V("5.0.0"),{}},candidates).failure==Failure::Downgrade);
    assert(Select({V("5.0.0"),V("1.0.0")},candidates).failure==Failure::Downgrade);
    assert(!Select({V("1.0.0"),{}},{{Source::Current,V("1.0.0"),true}}).index);
    // Exhaustive tie/priority and eligibility for all 4 sources.
    for (unsigned mask=0;mask<16;++mask) {
        std::vector<Candidate> same;
        for(unsigned i=0;i<4;++i) same.push_back({static_cast<Source>(i),V("1.0.0"),(mask&(1u<<i))!=0});
        const auto choice=Select({V("1.0.0"),V("1.0.0")},same);
        if(!mask) assert(!choice.index);
        else { for(unsigned i : {1u,2u,0u,3u})if(mask&(1u<<i)){Equal(choice,i);break;} }
    }
    using namespace GlassPackageCatalog;
    Catalog out; std::string error;
    auto good=json{{"setup","1.2.3.4"},{"glass","1.2.3.4"},{"setupPackage",MakeAsset("GlassSetup.exe")},{"products",json::array()}};
    for(std::size_t i=0;i<kCount;++i){auto a=MakeAsset(kFiles[i]);a["id"]=kIds[i];good["products"].push_back(a);}
    assert(Parse(good.dump(),out,error) && error.empty() && out.setup->downloadable);
    for(const auto& product:out.products) assert(product && product->downloadable);
    // A partial product release may link to a different release tag and version.
    auto partial=good;partial["products"][3]["version"]="2.0.0.5";
    partial["products"][3]["url"]="https://github.com/musicstashall/GlassPlatform/releases/download/settings-only/GlassSettings-x86.msi";
    assert(Parse(partial.dump(),out,error));assert(Compare(out.products[3]->version,V("2.0.0.5"))==0);
    auto missing=good;missing["products"][0].erase("sha256");
    assert(Parse(missing.dump(),out,error));assert(!out.products[0]->downloadable && out.products[1]->downloadable);
    assert(Parse("{\"glass\":\"1.2.3\",\"url\":\"https://github.com/legacy\"}",out,error));
    assert(!out.setup && out.setupVersion); // Legacy metadata is not a verified EXE download.
    auto rejected=[&](const json& value){assert(!Parse(value.dump(),out,error));assert(!error.empty() && !out.setup && !out.products[0]);};
    auto bad=good;bad["products"].push_back(bad["products"][0]);rejected(bad);
    for(const char* url : {"http://github.com/musicstashall/GlassPlatform/releases/download/v1/Glass-x64.msi",
        "https://github.com.evil/musicstashall/GlassPlatform/releases/download/v1/Glass-x64.msi",
        "https://github.com/musicstashall/GlassPlatform/releases/download/../Glass-x64.msi",
        "https://github.com/musicstashall/GlassPlatform/releases/download/%2e%2e/Glass-x64.msi",
        "https://github.com/musicstashall/GlassPlatform/releases/download/v1/Glass-x64.msi?x=1",
        "https://github.com/musicstashall/GlassPlatform/releases/download/v1/Glass-x86.msi"}) {
        bad=good;bad["products"][0]["url"]=url;rejected(bad);
    }
    for(const auto& size : {json(0u),json(-1),json(1.5),json("1024"),json(1024ull*1024*1024+1)}) {
        bad=good;bad["products"][0]["size"]=size;rejected(bad);
    }
    bad=good;bad["products"][0]["sha256"]=std::string(64,'z');rejected(bad);
    bad=good;bad["products"][0]["file"]="../Glass-x64.msi";rejected(bad);
    bad=good;bad["setupPackage"]["version"]="9.9.9";rejected(bad);
    bad=good;bad["products"]=json::object();rejected(bad);
    assert(!Parse("{\"setup\":\"1.2.3\",\"setup\":\"2.0.0\"}",out,error));
    assert(!Parse(std::string(1024*1024+1,' '),out,error));
    assert(!Parse(std::string(40,'[')+"0"+std::string(40,']'),out,error));
    std::cout<<"PASS: package versions/source selection and bounded release catalog parser\n";
}
