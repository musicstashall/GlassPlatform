#include "GlassOpenGlassPackage.h"
#include "GlassInnoExtract.h"
#include "GlassOpenGlassRelease.h"
#include "GlassReleaseClient.h"
#include "GlassMaintenanceCache.h"
#include "GlassSetupSourceLayout.h"
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <winternl.h>
#include <vector>
#include <array>
#include <algorithm>
#include <stdexcept>
#include <cwchar>
#include <utility>

namespace GlassOpenGlassPackage {
namespace {
struct File {
    HANDLE h=INVALID_HANDLE_VALUE;
    ~File(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
    File()=default;File(const File&)=delete;File& operator=(const File&)=delete;
};
void Require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void Pin(File& file,const std::wstring& path) {
    file.h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    Require(file.h!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(file.h,&info)&&info.nNumberOfLinks==1&&
        !(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),"OpenGlass file pin/link check failed");
}
std::array<unsigned char,32> Hash(const unsigned char* data,std::size_t size) {
    Require(size<=MAXULONG,"OpenGlass hash size limit");std::array<unsigned char,32> result{};
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    Require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"OpenGlass SHA256 provider");
    const auto status=BCryptHash(algorithm,nullptr,0,const_cast<PUCHAR>(data),static_cast<ULONG>(size),result.data(),static_cast<ULONG>(result.size()));
    BCryptCloseAlgorithmProvider(algorithm,0);Require(status>=0,"OpenGlass SHA256 failed");return result;
}
std::wstring Attribute(PCCERT_CONTEXT cert,const char* oid) {
    const auto n=CertGetNameStringW(cert,CERT_NAME_ATTR_TYPE,0,const_cast<char*>(oid),nullptr,0);
    if(n<2)return {};
    std::wstring result(n,L'\0');CertGetNameStringW(cert,CERT_NAME_ATTR_TYPE,0,const_cast<char*>(oid),result.data(),n);
    result.resize(n-1);return result;
}
std::array<unsigned char,32> VerifyPublisher(const std::wstring& path,HANDLE handle) {
    WINTRUST_FILE_INFO file{};file.cbStruct=sizeof(file);file.pcwszFilePath=path.c_str();file.hFile=handle;
    WINTRUST_DATA trust{};trust.cbStruct=sizeof(trust);trust.dwUIChoice=WTD_UI_NONE;
    trust.fdwRevocationChecks=WTD_REVOKE_NONE;trust.dwUnionChoice=WTD_CHOICE_FILE;trust.pFile=&file;
    trust.dwStateAction=WTD_STATEACTION_VERIFY;trust.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL|WTD_REVOCATION_CHECK_NONE;
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const auto status=WinVerifyTrust(nullptr,&action,&trust);
    struct Close {WINTRUST_DATA& t;GUID& g;~Close(){t.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&g,&t);}} close{trust,action};
    Require(status==ERROR_SUCCESS,"OpenGlass Authenticode is not Valid");
    const auto provider=WTHelperProvDataFromStateData(trust.hWVTStateData);
    const auto signer=provider?WTHelperGetProvSignerFromChain(provider,0,FALSE,0):nullptr;
    const auto cert=signer&&signer->csCertChain&&signer->pasCertChain?signer->pasCertChain[0].pCert:nullptr;
    Require(cert&&Attribute(cert,szOID_COMMON_NAME)==L"Open Source Developer, Zhe Yang"&&
        Attribute(cert,szOID_ORGANIZATION_NAME)==L"Open Source Developer"&&Attribute(cert,szOID_LOCALITY_NAME)==L"Guangzhou"&&
        Attribute(cert,szOID_STATE_OR_PROVINCE_NAME)==L"Guangdong"&&Attribute(cert,szOID_COUNTRY_NAME)==L"CN",
        "OpenGlass publisher subject mismatch");
    return Hash(cert->pbCertEncoded,cert->cbCertEncoded);
}
std::wstring Property(const std::vector<unsigned char>& version,const wchar_t* name) {
    struct Translation{WORD language,codepage;};Translation* translations=nullptr;UINT size=0;
    Require(VerQueryValueW(version.data(),L"\\VarFileInfo\\Translation",reinterpret_cast<void**>(&translations),&size)&&size>=sizeof(Translation),"OpenGlass version translations missing");
    std::wstring value;
    for(UINT i=0;i<size/sizeof(Translation);++i) {
        wchar_t key[160]{};swprintf_s(key,L"\\StringFileInfo\\%04x%04x\\%s",translations[i].language,translations[i].codepage,name);
        wchar_t* text=nullptr;UINT count=0;
        Require(VerQueryValueW(version.data(),key,reinterpret_cast<void**>(&text),&count)&&count>1,"OpenGlass version identity missing");
        const std::wstring current(text,count-1);
        Require(value.empty()||value==current,"OpenGlass ambiguous version identity");value=current;
    }
    return value;
}
struct Identity {std::wstring original,description;};
Identity Inspect(const std::wstring& path,const GlassInnoExtract::File& extracted,const GlassPackageRules::Version& expected) {
    const auto& bytes=extracted.bytes;const auto in=GlassInnoExtract::View(bytes);
    Require(in.size>=64&&in.LE(0,2)==0x5A4D,"OpenGlass payload not PE");const auto pe=in.LE(0x3c,4);
    Require(in.LE(static_cast<std::size_t>(pe),4)==0x4550&&in.LE(static_cast<std::size_t>(pe)+4,2)==0x8664&&
        in.LE(static_cast<std::size_t>(pe)+24,2)==0x20B,"OpenGlass requires PE32+ x64");
    const auto size=GetFileVersionInfoSizeW(path.c_str(),nullptr);Require(size>0&&size<1024*1024,"OpenGlass version resource size");
    std::vector<unsigned char> version(size);Require(GetFileVersionInfoW(path.c_str(),0,size,version.data())!=FALSE,"OpenGlass version resource unreadable");
    VS_FIXEDFILEINFO* fixed=nullptr;UINT length=0;
    Require(VerQueryValueW(version.data(),L"\\",reinterpret_cast<void**>(&fixed),&length)&&length>=sizeof(*fixed)&&fixed->dwSignature==0xFEEF04BD,"OpenGlass fixed version missing");
    const auto& v=expected.parts;
    Require(HIWORD(fixed->dwFileVersionMS)==v[0]&&LOWORD(fixed->dwFileVersionMS)==v[1]&&
        HIWORD(fixed->dwFileVersionLS)==v[2]&&LOWORD(fixed->dwFileVersionLS)==v[3]&&
        extracted.versionMS==fixed->dwFileVersionMS&&extracted.versionLS==fixed->dwFileVersionLS,"OpenGlass payload/release version mismatch");
    return {Property(version,L"OriginalFilename"),Property(version,L"FileDescription")};
}
void Write(const std::wstring& path,const GlassInnoExtract::Bytes& bytes) {
    File file;file.h=GlassReleaseClient::CreatePrivateFile(path);DWORD written=0;
    Require(file.h!=INVALID_HANDLE_VALUE&&bytes.size()<=MAXDWORD&&WriteFile(file.h,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&
        written==bytes.size()&&FlushFileBuffers(file.h),"Cannot stage OpenGlass extracted payload");
}
std::string ReadMetadata(const std::wstring& path) {
    Require(GlassMaintenanceCache::TrustedPath(path,false),"Untrusted OpenGlass metadata file");
    File file;Pin(file,path);LARGE_INTEGER length{};
    Require(GetFileSizeEx(file.h,&length)&&length.QuadPart>0&&length.QuadPart<=1024*1024,"OpenGlass metadata size");
    std::string text(static_cast<std::size_t>(length.QuadPart),'\0');DWORD read=0;
    Require(ReadFile(file.h,text.data(),static_cast<DWORD>(text.size()),&read,nullptr)&&read==text.size(),"OpenGlass metadata read");
    return text;
}
bool Metadata(const std::atomic<bool>& cancel,std::string& text,std::wstring& error) {
    {std::lock_guard<std::mutex> lock(GlassSetupSourceLayout::openGlassCatalogMutex);text=GlassSetupSourceLayout::openGlassCatalogText;}
    if(!text.empty())return !cancel.load();
    if(!GlassReleaseClient::Get(L"https://api.github.com/repos/ALTaleX531/OpenGlass/releases/latest",INVALID_HANDLE_VALUE,&text,1024*1024,cancel,error))return false;
    std::string version;Require(GlassOpenGlassRelease::Parse(text,version),"Unsupported OpenGlass metadata");
    {std::lock_guard<std::mutex> lock(GlassSetupSourceLayout::openGlassCatalogMutex);GlassSetupSourceLayout::openGlassCatalogText=text;}
    return !cancel.load();
}
bool Ends(const std::wstring& text,const wchar_t* suffix) {
    const std::wstring ending(suffix);return text.size()>=ending.size()&&text.compare(text.size()-ending.size(),ending.size(),ending)==0;
}
}
bool Describe(const std::atomic<bool>& cancel,std::wstring& version,std::uint64_t& size,std::wstring& error) noexcept {
    version.clear();size=0;error.clear();
    try {
        std::string text,parsed;if(!Metadata(cancel,text,error))return false;
        Require(GlassOpenGlassRelease::Parse(text,parsed),"Invalid OpenGlass version");
        const auto json=nlohmann::json::parse(text);unsigned matches=0;
        const auto tag=json.at("tag_name").get<std::string>();
        for(const auto& asset:json.at("assets"))if(asset.at("name")=="OpenGlassSetup.Signed.exe") {
            ++matches;size=asset.at("size").get<std::uint64_t>();
            Require(asset.at("browser_download_url").get<std::string>()=="https://github.com/ALTaleX531/OpenGlass/releases/download/"+tag+"/OpenGlassSetup.Signed.exe","Invalid OpenGlass asset URL");
        }
        Require(matches==1&&size>=512&&size<=GlassInnoExtract::MaxInput,"OpenGlass asset missing/size");
        version=GlassReleaseClient::Wide(parsed);return !cancel.load();
    }catch(const std::exception& e){error=L"OpenGlass discovery: "+GlassReleaseClient::Wide(e.what());}
     catch(...){error=L"OpenGlass discovery failed";}
    version.clear();size=0;return false;
}
bool PrepareForCurrentSystem(const std::wstring& workspace,const std::atomic<bool>& cancel,Package& package,std::wstring& error) noexcept {
    using Query=LONG (WINAPI*)(PRTL_OSVERSIONINFOW);
    const auto module=GetModuleHandleW(L"ntdll.dll");
    const auto query=module?reinterpret_cast<Query>(GetProcAddress(module,"RtlGetVersion")):nullptr;
    RTL_OSVERSIONINFOW version{};version.dwOSVersionInfoSize=sizeof(version);
    SYSTEM_INFO system{};GetNativeSystemInfo(&system);
    if(!query||query(&version)!=0||version.dwMajorVersion!=10||version.dwBuildNumber<17763||system.wProcessorArchitecture!=PROCESSOR_ARCHITECTURE_AMD64) {
        package={};error=L"OpenGlass: unsupported or unknown native Windows version/architecture";return false;
    }
    return Prepare(workspace,version.dwBuildNumber,cancel,package,error);
}
bool Prepare(const std::wstring& workspace,unsigned windowsBuild,const std::atomic<bool>& cancel,Package& package,std::wstring& error) noexcept {
    package={};error.clear();std::vector<std::wstring> created;
    const auto cancelled=[&]{return cancel.load();};
    try {
        Require(GlassMaintenanceCache::TrustedDirectory(workspace),"Untrusted OpenGlass workspace");
        std::string metadata,version;
        auto prepared=GlassSetupSourceLayout::Prepared(4);
        if(!prepared.empty()) {
            const auto slash=prepared.find_last_of(L"\\/");
            Require(slash!=std::wstring::npos&&GlassMaintenanceCache::TrustedDirectory(prepared.substr(0,slash)),"Untrusted prepared OpenGlass directory");
            metadata=ReadMetadata(prepared+L".metadata.json");
        }else if(!Metadata(cancel,metadata,error))return false;
        Require(GlassOpenGlassRelease::Parse(metadata,version),"Unsupported OpenGlass release tag");
        const auto json=nlohmann::json::parse(metadata);std::string url,digest;std::uint64_t size=0;unsigned matches=0;
        for(const auto& asset:json.at("assets"))if(asset.at("name")=="OpenGlassSetup.Signed.exe") {
            ++matches;url=asset.at("browser_download_url").get<std::string>();size=asset.at("size").get<std::uint64_t>();
            digest=asset.value("digest",std::string{});
        }
        const auto tag=json.at("tag_name").get<std::string>();
        Require(matches==1&&url=="https://github.com/ALTaleX531/OpenGlass/releases/download/"+tag+"/OpenGlassSetup.Signed.exe"&&
            size>=512&&size<=GlassInnoExtract::MaxInput,"Invalid OpenGlass release asset");
        Require(digest.compare(0,7,"sha256:")==0&&digest.size()==71&&std::all_of(digest.begin()+7,digest.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}),"OpenGlass release digest missing/invalid");
        const auto setup=workspace+L"\\OpenGlassSetup.Signed.exe";created.push_back(setup);
        {File output;output.h=GlassReleaseClient::CreatePrivateFile(setup);
         Require(output.h!=INVALID_HANDLE_VALUE,"Cannot create OpenGlass setup staging file");
         if(prepared.empty()) {
             if(!GlassReleaseClient::Get(GlassReleaseClient::Wide(url),output.h,nullptr,size,cancel,error))throw std::runtime_error("OpenGlass download failed");
         }else {
             Require(GlassMaintenanceCache::TrustedPath(prepared,false),"Untrusted prepared OpenGlass setup");
             File source;Pin(source,prepared);
             if(!GlassReleaseClient::VerifyHash(source.h,size,digest.substr(7),cancel,error))throw std::runtime_error("Prepared OpenGlass hash changed");
             LARGE_INTEGER zero{};Require(SetFilePointerEx(source.h,zero,nullptr,FILE_BEGIN)!=FALSE,"Prepared setup seek");
             std::array<unsigned char,65536> buffer{};
             for(;;) {
                 GlassInnoExtract::CheckCancel(cancelled);DWORD read=0,written=0;
                 Require(ReadFile(source.h,buffer.data(),static_cast<DWORD>(buffer.size()),&read,nullptr)!=FALSE,"Prepared setup read");
                 if(!read)break;
                 Require(WriteFile(output.h,buffer.data(),read,&written,nullptr)&&written==read,"Prepared setup copy");
             }
         }
         Require(FlushFileBuffers(output.h)!=FALSE,"OpenGlass download flush failed");}
        File pinned;Pin(pinned,setup);
        if(!GlassReleaseClient::VerifyHash(pinned.h,size,digest.substr(7),cancel,error))throw std::runtime_error("OpenGlass setup hash failed");
        const auto publisher=VerifyPublisher(setup,pinned.h);GlassInnoExtract::CheckCancel(cancelled);
        GlassInnoExtract::Bytes input(static_cast<std::size_t>(size));LARGE_INTEGER zero{};
        Require(SetFilePointerEx(pinned.h,zero,nullptr,FILE_BEGIN)!=FALSE,"OpenGlass input seek failed");DWORD read=0;
        Require(ReadFile(pinned.h,input.data(),static_cast<DWORD>(input.size()),&read,nullptr)&&read==input.size(),"OpenGlass input read failed");
        HMODULE module=LoadLibraryExW(setup.c_str(),nullptr,LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        Require(module!=nullptr,"OpenGlass resource mapping failed");
        struct Unmap{HMODULE h;~Unmap(){FreeLibrary(h);}} unmap{module};
        const auto resource=FindResourceW(module,MAKEINTRESOURCEW(11111),RT_RCDATA);
        Require(resource&&SizeofResource(module,resource)==64,"OpenGlass Inno offsets resource missing");
        const auto data=LoadResource(module,resource);const auto offsets=static_cast<const unsigned char*>(LockResource(data));
        Require(offsets!=nullptr,"OpenGlass Inno offsets unavailable");
        const auto files=GlassInnoExtract::Extract(GlassInnoExtract::View(input),{offsets,64},cancelled);
        GlassPackageRules::Version expected;Require(GlassPackageRules::ParseVersion(version,expected),"OpenGlass version invalid");
        bool legacy=false,milcomp=false,host=false,gui=false;Package result;
        result.version=GlassReleaseClient::Wide(version);result.setup=setup;result.milComp=windowsBuild>=28000;
        for(std::size_t i=0;i<files.size();++i) {
            GlassInnoExtract::CheckCancel(cancelled);const auto& file=files[i];
            Require(Hash(file.bytes.data(),file.bytes.size())==file.sha256,"OpenGlass extracted SHA256 mismatch");
            const auto path=workspace+L"\\OpenGlassPayload-"+std::to_wstring(i)+L".bin";created.push_back(path);Write(path,file.bytes);
            File verified;Pin(verified,path);Require(VerifyPublisher(path,verified.h)==publisher,"OpenGlass payload publisher differs from setup");
            const auto identity=Inspect(path,file,expected);
            if(identity.original==L"OpenGlass.dll"&&Ends(identity.description,L"(Legacy)")) {
                Require(!legacy,"Duplicate Legacy payload");legacy=true;if(!result.milComp)result.dll=path;
            }else if(identity.original==L"OpenGlass.dll"&&Ends(identity.description,L"(MILComp)")) {
                Require(!milcomp,"Duplicate MILComp payload");milcomp=true;if(result.milComp)result.dll=path;
            }else if(identity.original==L"OpenGlassHost.exe") {
                Require(!host,"Duplicate OpenGlassHost");host=true;result.host=path;
            }else if(identity.original==L"OpenGlassGUI.exe") {
                Require(!gui,"Duplicate OpenGlassGUI");gui=true;result.gui=path;
            } // Extra records are never installed by this provider.
        }
        Require(legacy&&milcomp&&host&&gui,"OpenGlass release lacks required payloads");
        GlassInnoExtract::CheckCancel(cancelled);
        const auto receipt=setup+L".metadata.json";created.push_back(receipt);
        Write(receipt,GlassInnoExtract::Bytes(metadata.begin(),metadata.end()));
        package=std::move(result);return true;
    }catch(const std::exception& e){if(error.empty())error=L"OpenGlass: "+GlassReleaseClient::Wide(e.what());}
     catch(...){if(error.empty())error=L"OpenGlass staging failed";}
    // Workspace is private and remains unpublished; never delete user paths.
    for(const auto& path:created)DeleteFileW(path.c_str());
    return false;
}
}
