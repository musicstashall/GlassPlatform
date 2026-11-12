#include "GlassOpenGlassPackage.h"
#include "GlassOpenGlassTrust.h"
#include "GlassOpenGlassInstall.h"
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
#include <limits>

namespace GlassOpenGlassPackage {
namespace {
using namespace GlassOpenGlassTrust;
// Only the staged, verified upstream installer is unblocked. Never change the
// original adjacent/Downloads file, other streams, trust policy or certificates.
void RemoveInternetZone(const std::wstring& setup) {
    const auto zone=setup+L":Zone.Identifier";
    if(!DeleteFileW(zone.c_str())) {
        const auto error=GetLastError();
        if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)
            throw std::runtime_error("Cannot remove OpenGlass installer Zone.Identifier ("+std::to_string(error)+")");
    }
    // Absence must be observable; access denied is not evidence of absence.
    File check;check.h=CreateFileW(zone.c_str(),FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(check.h!=INVALID_HANDLE_VALUE)throw std::runtime_error("OpenGlass installer Zone.Identifier is still present");
    const auto error=GetLastError();
    if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)
        throw std::runtime_error("Cannot confirm OpenGlass installer Zone.Identifier removal ("+std::to_string(error)+")");
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
std::wstring SetupFileVersion(const std::wstring& path) {
    DWORD ignored=0;const auto size=GetFileVersionInfoSizeW(path.c_str(),&ignored);
    Require(size>0&&size<=1024*1024,"OpenGlass setup version is missing/too large");
    std::vector<BYTE> bytes(size);Require(GetFileVersionInfoW(path.c_str(),0,size,bytes.data())!=FALSE,"Cannot read OpenGlass setup version");
    VS_FIXEDFILEINFO* info=nullptr;UINT count=0;
    Require(VerQueryValueW(bytes.data(),L"\\",reinterpret_cast<void**>(&info),&count)&&info&&count>=sizeof(*info)&&
        info->dwSignature==0xfeef04bd,"Invalid OpenGlass setup fixed version");
    return std::to_wstring(HIWORD(info->dwFileVersionMS))+L"."+std::to_wstring(LOWORD(info->dwFileVersionMS))+L"."+
        std::to_wstring(HIWORD(info->dwFileVersionLS))+L"."+std::to_wstring(LOWORD(info->dwFileVersionLS));
}
void StageAttachedInstaller(const std::wstring& source,const std::wstring& setup,
    const std::atomic<bool>& cancel,Package& package,std::vector<std::wstring>& created) {
    File input;Pin(input,source);const auto signer=VerifyPublisher(source,input.h);
    LARGE_INTEGER size{};Require(GetFileSizeEx(input.h,&size)&&size.QuadPart>=512&&
        static_cast<std::uint64_t>(size.QuadPart)<=GlassInnoExtract::MaxInput,"Invalid adjacent OpenGlass setup size");
    {
        File output;output.h=GlassReleaseClient::CreatePrivateFile(setup);
        Require(output.h!=INVALID_HANDLE_VALUE,"Cannot stage adjacent OpenGlass setup");
        created.push_back(setup); // Cleanup owns only successfully created files.
        std::array<BYTE,65536> buffer{};std::uint64_t copied=0;
        for(;;) {
            GlassInnoExtract::CheckCancel([&]{return cancel.load();});DWORD read=0,written=0;
            Require(ReadFile(input.h,buffer.data(),static_cast<DWORD>(buffer.size()),&read,nullptr)!=FALSE,"Adjacent OpenGlass setup read failed");
            if(!read)break;
            Require(WriteFile(output.h,buffer.data(),read,&written,nullptr)&&written==read,"Adjacent OpenGlass setup write failed");
            copied+=read;Require(copied<=static_cast<std::uint64_t>(size.QuadPart),"Adjacent OpenGlass setup grew");
        }
        Require(copied==static_cast<std::uint64_t>(size.QuadPart)&&FlushFileBuffers(output.h),"Adjacent OpenGlass setup flush/size failed");
    }
    {File verified;Pin(verified,setup);Require(VerifyPublisher(setup,verified.h)==signer,"Adjacent OpenGlass setup signer changed");}
    RemoveInternetZone(setup);
    File verified;Pin(verified,setup);Require(VerifyPublisher(setup,verified.h)==signer,"Adjacent OpenGlass setup signer changed after unblock");
    package.setup=setup;package.version=SetupFileVersion(setup);
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
static bool PrepareImpl(const std::wstring& workspace,unsigned windowsBuild,const std::atomic<bool>& cancel,Package& package,std::wstring& error,bool extractPayload) noexcept {
    package={};error.clear();std::vector<std::wstring> created;
    const auto cancelled=[&]{return cancel.load();};
    try {
        Require(GlassMaintenanceCache::TrustedDirectory(workspace),"Untrusted OpenGlass workspace");
        if(!extractPayload&&!GlassSetupSourceLayout::directory.empty()) {
            const auto local=GlassSetupSourceLayout::directory+L"\\OpenGlassSetup.Signed.exe";
            const auto attributes=GetFileAttributesW(local.c_str());
            if(attributes!=INVALID_FILE_ATTRIBUTES) {
                Require(!(attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),"Unsafe adjacent OpenGlass setup");
                const auto setup=workspace+L"\\OpenGlassSetup.Signed.exe";
                StageAttachedInstaller(local,setup,cancel,package,created);
                GlassInnoExtract::CheckCancel(cancelled);return true;
            }
            const auto failure=GetLastError();
            Require(failure==ERROR_FILE_NOT_FOUND||failure==ERROR_PATH_NOT_FOUND,"Adjacent OpenGlass setup is inaccessible");
        }
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
        const auto setup=workspace+L"\\OpenGlassSetup.Signed.exe";
        {File output;output.h=GlassReleaseClient::CreatePrivateFile(setup);
         Require(output.h!=INVALID_HANDLE_VALUE,"Cannot create OpenGlass setup staging file");
         created.push_back(setup);
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
        // Release the verification handle before deleting the named stream:
        // no sharing-mode dependency may turn unblocking into a hidden prompt.
        {
            File verified;Pin(verified,setup);
            if(!GlassReleaseClient::VerifyHash(verified.h,size,digest.substr(7),cancel,error))throw std::runtime_error("OpenGlass setup hash failed");
            (void)VerifyPublisher(setup,verified.h);
        }
        RemoveInternetZone(setup);
        // Re-pin and revalidate bytes/identity after the stream operation. The
        // caller never receives an executable validated only before a reopen.
        File pinned;Pin(pinned,setup);
        if(!GlassReleaseClient::VerifyHash(pinned.h,size,digest.substr(7),cancel,error))throw std::runtime_error("OpenGlass setup hash failed");
        const auto publisher=VerifyPublisher(setup,pinned.h);GlassInnoExtract::CheckCancel(cancelled);
        if(!extractPayload) {
            const auto actual=SetupFileVersion(setup);
            GlassPackageRules::Version expected{},found{};
            Require(GlassPackageRules::ParseVersion(version,expected)&&
                GlassPackageRules::ParseVersion(std::string(actual.begin(),actual.end()),found)&&
                GlassPackageRules::Compare(expected,found)==0,"OpenGlass setup/release version mismatch");
            const auto receipt=setup+L".metadata.json";created.push_back(receipt);
            Write(receipt,GlassInnoExtract::Bytes(metadata.begin(),metadata.end()));
            package.setup=setup;package.version=actual;return true;
        }
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
    package={};return false;
}
bool Prepare(const std::wstring& workspace,unsigned windowsBuild,const std::atomic<bool>& cancel,Package& package,std::wstring& error) noexcept {
    return PrepareImpl(workspace,windowsBuild,cancel,package,error,true);
}
bool PrepareInstaller(const std::wstring& workspace,unsigned windowsBuild,const std::atomic<bool>& cancel,Package& package,std::wstring& error) noexcept {
    return PrepareImpl(workspace,windowsBuild,cancel,package,error,false);
}
bool PrepareInstallerForCurrentSystem(const std::wstring& workspace,const std::atomic<bool>& cancel,Package& package,std::wstring& error) noexcept {
    using Query=LONG (WINAPI*)(PRTL_OSVERSIONINFOW);
    const auto module=GetModuleHandleW(L"ntdll.dll");const auto query=module?reinterpret_cast<Query>(GetProcAddress(module,"RtlGetVersion")):nullptr;
    RTL_OSVERSIONINFOW version{};version.dwOSVersionInfoSize=sizeof(version);SYSTEM_INFO system{};GetNativeSystemInfo(&system);
    if(!query||query(&version)!=0||version.dwMajorVersion!=10||version.dwBuildNumber<17763||system.wProcessorArchitecture!=PROCESSOR_ARCHITECTURE_AMD64) {
        package={};error=L"OpenGlass: unsupported or unknown native Windows version/architecture";return false;
    }
    return PrepareInstaller(workspace,version.dwBuildNumber,cancel,package,error);
}
bool RunInstaller(const Package& package,const std::wstring& directory,DWORD& exitCode,std::wstring& error) noexcept {
    exitCode=ERROR_INSTALL_FAILURE;error.clear();
    try {
        const auto slash=package.setup.find_last_of(L"\\/");
        Require(slash!=std::wstring::npos&&package.setup.substr(slash+1)==L"OpenGlassSetup.Signed.exe"&&
            GlassMaintenanceCache::TrustedDirectory(package.setup.substr(0,slash))&&
            GlassMaintenanceCache::TrustedPath(package.setup,false),"Untrusted OpenGlass installer destination");
        Require(GlassInstallUtil::IsSafeInstallPath(directory)&&directory.size()>3&&directory[1]==L':'&&directory[2]==L'\\'&&
            directory.find_first_of(L"\"\r\n")==std::wstring::npos&&directory.back()!=L'\\',"Unsafe OpenGlass install directory");
        {File verified;Pin(verified,package.setup);(void)VerifyPublisher(package.setup,verified.h);}
        RemoveInternetZone(package.setup);
        File pinned;Pin(pinned,package.setup);(void)VerifyPublisher(package.setup,pinned.h);
        Require(!package.version.empty()&&SetupFileVersion(package.setup)==package.version,"OpenGlass installer version changed");
        std::wstring command=L"\""+package.setup+L"\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NOICONS /MERGETASKS=\"!desktopicon\" /SP- /RESTARTEXITCODE=3010 /DIR=\""+directory+L"\" /LOG=\""+package.setup.substr(0,slash)+L"\\OpenGlass-install.log\"";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
        PROCESS_INFORMATION process{};
        Require(CreateProcessW(package.setup.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,
            package.setup.substr(0,slash).c_str(),&startup,&process)!=FALSE,"Cannot start official OpenGlass installer");
        struct ProcessHandles {PROCESS_INFORMATION& p;~ProcessHandles(){CloseHandle(p.hThread);CloseHandle(p.hProcess);}} handles{process};
        Require(WaitForSingleObject(process.hProcess,INFINITE)==WAIT_OBJECT_0,"Cannot wait for official OpenGlass installer");
        Require(GetExitCodeProcess(process.hProcess,&exitCode)!=FALSE,"Cannot read official OpenGlass installer result");
        return true;
    }catch(const std::exception& e){error=L"OpenGlass: "+GlassReleaseClient::Wide(e.what());}
     catch(...){error=L"OpenGlass installer execution failed";}
    return false;
}

}
