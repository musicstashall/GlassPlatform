#include "GlassSetupSourceLayout.h"
#include "GlassPackageAcquisition.h"
#include "GlassReleaseClient.h"
#include "GlassMaintenanceCache.h"
#include "GlassProductInspect.h"
#include "../InstallLog.h"
#include <wincrypt.h>
#include <msidefs.h>
#include <objbase.h>
#include <array>
#include <wintrust.h>
#include <softpub.h>

namespace GlassPackageAcquisition {
namespace {
using namespace GlassPackageRules;
using GlassReleaseClient::Wide;
struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~Handle(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
};
struct Msi {
    MSIHANDLE h = 0;
    ~Msi(){if(h)MsiCloseHandle(h);}
};
struct Spec {const wchar_t* code;const wchar_t* upgrade;const wchar_t* machine;};
const Spec specs[] = {
    {GlassInstallMsi::kProductPlatformX64,L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5010}",L"x64"},
    {GlassInstallMsi::kProductPlatformX86,L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5020}",L"Intel"},
    {GlassInstallMsi::kProductSettingsX64,L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5040}",L"x64"},
    {GlassInstallMsi::kProductSettingsX86,L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5050}",L"Intel"},
    {GlassInstallMsi::kProductOpenGlass,L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5030}",L"x64"},
    {GlassInstallMsi::kProductOpenGlassResourcesX64,L"{A5571574-00E7-43AD-81DF-B65D1AE07699}",L"x64"}
};
static_assert(sizeof(specs)/sizeof(specs[0])==GlassPackageCatalog::kCount);
bool NativeProductSupported(GlassPackageCatalog::Product product) {
    if(product!=GlassPackageCatalog::Product::OpenGlassResources)return true;
    SYSTEM_INFO system{};GetNativeSystemInfo(&system);
    return GlassPackageCatalog::ResourcesSupported(system.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_AMD64);
}
bool Fail(std::wstring& error,const std::wstring& text) {error=text;return false;}
std::string Narrow(const std::wstring& text) {
    std::string out;
    for(wchar_t c:text){if(c>127)return {};out.push_back(static_cast<char>(c));}
    return out;
}
bool Property(MSIHANDLE db,const wchar_t* name,std::wstring& value) {
    const auto sql=L"SELECT `Value` FROM `Property` WHERE `Property`='"+std::wstring(name)+L"'";
    Msi view,row;
    if(MsiDatabaseOpenViewW(db,sql.c_str(),&view.h)||MsiViewExecute(view.h,0)||MsiViewFetch(view.h,&row.h))return false;
    DWORD size=0;wchar_t empty=0;
    if(MsiRecordGetStringW(row.h,1,&empty,&size)!=ERROR_MORE_DATA||size>32767)return false;
    std::vector<wchar_t> data(size+1);++size;
    if(MsiRecordGetStringW(row.h,1,data.data(),&size))return false;
    value.assign(data.data(),size);return true;
}
bool Metadata(const std::wstring& path,const Spec& spec,Version& version,std::wstring& code,std::wstring& display) {
    Msi db,summary;
    if(MsiOpenDatabaseW(path.c_str(),MSIDBOPEN_READONLY,&db.h))return false;
    std::wstring upgrade;
    if(!Property(db.h,L"ProductCode",code)||!Property(db.h,L"UpgradeCode",upgrade)||
       !Property(db.h,L"ProductVersion",display)||_wcsicmp(upgrade.c_str(),spec.upgrade)||
       !ParseVersion(Narrow(display),version))return false;
    // Current package family uses fixed product codes. Do not run a foreign MSI
    // just because it copied our display name or UpgradeCode.
    if(_wcsicmp(code.c_str(),spec.code))return false;
    if(MsiGetSummaryInformationW(db.h,nullptr,0,&summary.h))return false;
    UINT type=0;INT number=0;FILETIME time{};wchar_t text[256]{};DWORD size=256;
    if(MsiSummaryInfoGetPropertyW(summary.h,PID_TEMPLATE,&type,&number,&time,text,&size))return false;
    std::wstring machine=text;machine.resize(machine.find(L';')==std::wstring::npos?machine.size():machine.find(L';'));
    return !_wcsicmp(machine.c_str(),spec.machine);
}
bool SameMsiPublisher(const std::wstring& path) {
    const auto expected=GlassProductInspect::QuerySelfSignerSha1();
    if(expected.empty()) {
        InstallLog::LineFormat(L"Local MSI publisher unavailable: running Setup has no identifiable signer: %s",path.c_str());
        return false; // Never implicitly trust unsigned adjacent MSI.
    }
    // Validate the MSI SIP signature, not merely the embedded certificate. A
    // fresh machine need not trust our self-signed author certificate globally:
    // the exact leaf is pinned to the running Setup instead. No store mutation.
    WINTRUST_FILE_INFO info{};info.cbStruct=sizeof(info);info.pcwszFilePath=path.c_str();
    WINTRUST_DATA trust{};trust.cbStruct=sizeof(trust);trust.dwUIChoice=WTD_UI_NONE;
    trust.fdwRevocationChecks=WTD_REVOKE_NONE;trust.dwUnionChoice=WTD_CHOICE_FILE;
    trust.pFile=&info;trust.dwStateAction=WTD_STATEACTION_VERIFY;
    trust.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL|WTD_REVOCATION_CHECK_NONE;
    GUID policy=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status=WinVerifyTrust(nullptr,&policy,&trust);
    struct CloseTrust {
        WINTRUST_DATA& data;GUID& policy;
        ~CloseTrust(){data.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&policy,&data);}
    } closeTrust{trust,policy};
    bool ok=false;
    // Do not accept generic trust failures, expiry, wrong usage, missing chains,
    // absent signatures or a bad digest just because a certificate was extracted.
    if(status==ERROR_SUCCESS||status==static_cast<LONG>(CERT_E_UNTRUSTEDROOT)) {
        auto* provider=WTHelperProvDataFromStateData(trust.hWVTStateData);
        auto* signer=provider?WTHelperGetProvSignerFromChain(provider,0,FALSE,0):nullptr;
        PCCERT_CONTEXT cert=signer&&signer->csCertChain&&signer->pasCertChain?signer->pasCertChain[0].pCert:nullptr;
        BYTE digest[20]{};DWORD bytes=sizeof(digest);
        if(cert&&CertGetCertificateContextProperty(cert,CERT_SHA1_HASH_PROP_ID,digest,&bytes)&&bytes==20) {
            std::wstring value;constexpr wchar_t digits[]=L"0123456789ABCDEF";
            for(BYTE byte:digest){value+=digits[byte>>4];value+=digits[byte&15];}
            ok=!_wcsicmp(value.c_str(),expected.c_str());
        }
    }
    InstallLog::LineFormat(L"Local MSI Authenticode: status=0x%08lX, Setup publisher match=%u: %s",
        static_cast<unsigned long>(status),ok?1u:0u,path.c_str());
    return ok;
}

bool CopyLocked(HANDLE source,const std::wstring& destination,const std::atomic<bool>& cancel) {
    Handle out;out.h=GlassReleaseClient::CreatePrivateFile(destination);
    if(out.h==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER zero{};
    if(!SetFilePointerEx(source,zero,nullptr,FILE_BEGIN))return false;
    std::array<BYTE,32768> buffer{};
    while(!cancel.load(std::memory_order_acquire)){
        DWORD read=0,written=0;
        if(!ReadFile(source,buffer.data(),static_cast<DWORD>(buffer.size()),&read,nullptr))return false;
        if(!read)return FlushFileBuffers(out.h)!=FALSE;
        if(!WriteFile(out.h,buffer.data(),read,&written,nullptr)||written!=read)return false;
    }
    return false;
}
}
bool CreateWorkspace(std::wstring& directory) noexcept {
    directory.clear();
    try {
        if(!GlassMaintenanceCache::EnsureRoot())return false;
        GUID id{};wchar_t text[40]{};
        if(FAILED(CoCreateGuid(&id))||!StringFromGUID2(id,text,40))return false;
        const auto path=GlassMaintenanceCache::Root()+L"\\Work-"+text;
        if(!GlassMaintenanceCache::SecureDirectory(path))return false;
        directory=path;return true;
    }catch(...){return false;}
}
bool PrepareSetup(const GlassPackageCatalog::Asset& asset,const std::wstring& workspace,
    const std::atomic<bool>& cancel,std::wstring& prepared,std::wstring& error) noexcept {
    prepared.clear();error.clear();
    const auto part=workspace+L"\\GlassSetup.exe.part",file=workspace+L"\\GlassSetup.exe";
    try {
        if(!asset.downloadable||asset.file!="GlassSetup.exe"||
           !GlassPackageCatalog::ReleaseUrl(asset.url,asset.file)||!GlassPackageCatalog::Digest(asset.sha256)||
           asset.size<512||asset.size>1024ull*1024*1024||!GlassMaintenanceCache::TrustedDirectory(workspace)||cancel.load())
            return Fail(error,L"Setup download metadata/workspace unavailable");
        bool ok=false;
        {
            Handle out;out.h=GlassReleaseClient::CreatePrivateFile(part);
            if(out.h==INVALID_HANDLE_VALUE)return Fail(error,L"Cannot create private Setup staging file");
            ok=GlassReleaseClient::Get(Wide(asset.url),out.h,nullptr,asset.size,cancel,error)&&FlushFileBuffers(out.h);
        }
        if(ok){
            Handle locked;locked.h=CreateFileW(part.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            ok=locked.h!=INVALID_HANDLE_VALUE&&GlassReleaseClient::VerifyHash(locked.h,asset.size,asset.sha256,cancel,error);
            if(ok){
                WINTRUST_FILE_INFO info{};info.cbStruct=sizeof(info);info.pcwszFilePath=part.c_str();info.hFile=locked.h;
                WINTRUST_DATA trust{};trust.cbStruct=sizeof(trust);trust.dwUIChoice=WTD_UI_NONE;
                trust.dwUnionChoice=WTD_CHOICE_FILE;trust.pFile=&info;trust.dwStateAction=WTD_STATEACTION_VERIFY;
                trust.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL|WTD_REVOCATION_CHECK_NONE;
                GUID policy=WINTRUST_ACTION_GENERIC_VERIFY_V2;
                const LONG status=WinVerifyTrust(nullptr,&policy,&trust);
                trust.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&policy,&trust);
                ok=status==ERROR_SUCCESS&&GlassProductInspect::FileHasSameSignerAsSetup(part);
                if(!ok)error=L"Downloaded Setup failed Authenticode/expected publisher verification";
            }
        }
        if(ok&&!cancel.load()&&MoveFileExW(part.c_str(),file.c_str(),MOVEFILE_WRITE_THROUGH)){
            prepared=file;return true;
        }
    }catch(...){error=L"Cannot prepare new Setup";}
    DeleteFileW(part.c_str()); // Only this freshly-created workspace's .part.
    if(error.empty())error=L"Setup preparation failed or cancelled";
    return false;
}
bool FetchCatalog(const std::atomic<bool>& cancel,GlassPackageCatalog::Catalog& catalog,std::wstring& diagnostic) noexcept {
    catalog={};
    try {
        std::lock_guard<std::mutex> lock(GlassSetupSourceLayout::catalogMutex);
        std::string text,error;
        const bool reuse=GlassSetupSourceLayout::catalogInherited||GlassSetupSourceLayout::catalogChecked;
        if(reuse) {
            text=GlassSetupSourceLayout::catalogText;
            if(text.empty())return Fail(diagnostic,L"Parent catalog check was unavailable; continuing offline without a duplicate request");
        } else {
            GlassSetupSourceLayout::catalogChecked=true;
            GlassSetupSourceLayout::catalogText.clear();
            if(!GlassReleaseClient::Get(L"https://github.com/musicstashall/GlassPlatform/releases/latest/download/version.json",
                INVALID_HANDLE_VALUE,&text,1024*1024,cancel,diagnostic))return false;
        }
        if(!GlassPackageCatalog::Parse(text,catalog,error))return Fail(diagnostic,Wide(error));
        if(!reuse)GlassSetupSourceLayout::catalogText=text;
        diagnostic.clear();return true;
    }catch(...){return Fail(diagnostic,L"Cannot read release package catalog");}
}
// Shared read-only discovery: identical identity/trust policy for UI and Apply.
// Holds each file against write/delete while checking metadata and its trust.
static bool Discover(GlassPackageCatalog::Product product,const GlassPackageCatalog::Catalog& catalog,
    const std::atomic<bool>& cancel,std::vector<Candidate>& candidates,
    std::vector<std::wstring>& sources,std::wstring& diagnostics,std::wstring& error,
    std::vector<std::wstring>* labels=nullptr) {
        const auto i=static_cast<std::size_t>(product);
        if(i>=GlassPackageCatalog::kCount||cancel.load()||!NativeProductSupported(product))return false;
        const auto& spec=specs[i];const auto& remote=catalog.products[i];
        const auto name=Wide(GlassPackageCatalog::kFiles[i]);
        wchar_t exe[32768]{};const DWORD length=GetModuleFileNameW(nullptr,exe,32768);
        if(!length||length>=32768)return Fail(error,L"Cannot locate installer directory");
        std::wstring parent(exe,length);parent=parent.substr(0,parent.find_last_of(L"\\/"));
        if(!GlassSetupSourceLayout::directory.empty())parent=GlassSetupSourceLayout::directory;
        const std::array<std::wstring,4> paths={GlassMaintenanceCache::CurrentFile(name.c_str()),
            parent+L"\\packages\\"+name,parent+L"\\"+name,GlassSetupSourceLayout::Prepared(GlassPackageCatalog::PreparedIndex(product))};
        const auto rejected=[&](const std::wstring& path,const std::wstring& reason) {
            InstallLog::LineFormat(L"Local MSI rejected: %s: %s",path.c_str(),reason.c_str());
            diagnostics+=L"\n"+path+L": "+reason;
        };
        for(std::size_t n=0;n<paths.size();++n) {
            if(paths[n].empty())continue;
            Handle file;file.h=CreateFileW(paths[n].c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            if(file.h==INVALID_HANDLE_VALUE){rejected(paths[n],L"cannot open file (Win32 "+std::to_wstring(GetLastError())+L")");continue;}
            BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
            if(!GetFileInformationByHandle(file.h,&info)||!GetFileSizeEx(file.h,&size)||
                (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||info.nNumberOfLinks!=1||
                size.QuadPart<512||size.QuadPart>1024ll*1024*1024){rejected(paths[n],L"unsafe file attributes, links or size");continue;}
            Version v;std::wstring code,display;
            if(!Metadata(paths[n],spec,v,code,display)){rejected(paths[n],L"MSI identity, architecture or version mismatch (or unreadable MSI)");continue;}
            bool trusted=n==0&&GlassMaintenanceCache::MatchesInstalled(paths[n],code,display);
            if(!trusted&&remote&&remote->downloadable&&Compare(v,remote->version)==0)
                trusted=GlassReleaseClient::VerifyHash(file.h,remote->size,remote->sha256,cancel,error);
            if(!trusted)trusted=SameMsiPublisher(paths[n]);
            if(!trusted) {
                rejected(paths[n],L"signature/publisher or release hash verification failed (see log); unsigned local MSI is not trusted");
                continue;
            }
            candidates.push_back({(n==3?Source::Prepared:static_cast<Source>(n)),v,true});sources.push_back(paths[n]);
            if(labels)labels->push_back(display);
        }
        if(remote&&remote->downloadable){
            candidates.push_back({Source::Release,remote->version,true});sources.push_back({});
            if(labels){const auto& v=remote->version.parts;labels->push_back(std::to_wstring(v[0])+L"."+
                std::to_wstring(v[1])+L"."+std::to_wstring(v[2])+L"."+std::to_wstring(v[3]));}
        }
        return !cancel.load();
}

bool Describe(GlassPackageCatalog::Product product,const GlassPackageCatalog::Catalog& catalog,
    const std::atomic<bool>& cancel,std::wstring& version,bool& verifiedLocal,std::wstring* localPath,std::wstring* attachedVersion,std::wstring* attachedPath) noexcept {
    if(localPath)localPath->clear();
    if(attachedVersion)attachedVersion->clear();
    if(attachedPath)attachedPath->clear();
    version.clear();verifiedLocal=false;
    try {
        std::vector<Candidate> candidates;std::vector<std::wstring> sources,labels;
        std::wstring diagnostics,error;
        if(!Discover(product,catalog,cancel,candidates,sources,diagnostics,error,&labels))return false;
        std::optional<std::size_t> attached;
        for(std::size_t i=0;i<candidates.size();++i) {
            const auto& c=candidates[i];
            if(c.source!=Source::LocalPackages&&c.source!=Source::LocalAdjacent)continue;
            if(!attached||Compare(c.version,candidates[*attached].version)>0)attached=i;
        }
        if(attached) {
            if(attachedVersion)*attachedVersion=labels[*attached];
            if(attachedPath)*attachedPath=sources[*attached];
        }
        Request request;const auto& spec=specs[static_cast<std::size_t>(product)];
        if(GlassInstallMsi::IsProductInstalled(spec.code)) {
            Version installed;
            if(!ParseVersion(Narrow(GlassInstallMsi::QueryDisplayVersion(spec.code)),installed))return false;
            request.installed=installed;
            bool newer=false;
            for(const auto& c:candidates)if(c.source!=Source::Current&&Compare(c.version,installed)>0)newer=true;
            if(!newer)request.target=installed;
        }
        const auto choice=Select(request,candidates);
        if(!choice.index||!choice.target)return false;
        version=labels[*choice.index];
        verifiedLocal=candidates[*choice.index].source!=Source::Release;
        if(localPath&&verifiedLocal)*localPath=sources[*choice.index];
        return true;
    }catch(...){version.clear();verifiedLocal=false;return false;}
}

bool Prepare(GlassPackageCatalog::Product product,const GlassPackageCatalog::Catalog& catalog,
    const std::wstring& workspace,const std::atomic<bool>& cancel,std::wstring& error) noexcept {
    std::wstring staged,part;
    try {
        const auto i=static_cast<std::size_t>(product);
        if(i>=GlassPackageCatalog::kCount||!NativeProductSupported(product)||!GlassMaintenanceCache::TrustedDirectory(workspace))
            return Fail(error,L"Invalid package workspace/product");
        if(cancel.load())return Fail(error,L"Package acquisition cancelled");
        const auto& spec=specs[i];const auto& remote=catalog.products[i];
        const auto name=Wide(GlassPackageCatalog::kFiles[i]);
        staged=workspace+L"\\"+name;part=staged+L".part";
        Request request;
        Version installed;
        if(GlassInstallMsi::IsProductInstalled(spec.code)) {
            if(!ParseVersion(Narrow(GlassInstallMsi::QueryDisplayVersion(spec.code)),installed))
                return Fail(error,L"Installed product version is unknown");
            request.installed=installed;
        }
        std::vector<Candidate> candidates;std::vector<std::wstring> sources;
        std::wstring diagnostics;
        if(!Discover(product,catalog,cancel,candidates,sources,diagnostics,error))return false;
        // Repair fallback to Current is valid only if there is no higher target.
        bool newer=false;
        if(request.installed) {
            for(const auto& c:candidates)if(c.source!=Source::Current&&Compare(c.version,*request.installed)>0)newer=true;
            if(!newer)request.target=request.installed;
        }
        const auto choice=Select(request,candidates);
        if(!choice.index)return Fail(error,choice.failure==Failure::Downgrade?
            L"Refusing package downgrade: "+name:L"No verified package in Current, packages, installer directory or release: "+name+diagnostics+
                (remote&&remote->downloadable?L"":L"\nRelease catalog: no downloadable asset for this product."));
        const auto index=*choice.index;
        error.clear();
        if(candidates[index].source==Source::Release) {
            {Handle download;download.h=GlassReleaseClient::CreatePrivateFile(part);
             if(download.h==INVALID_HANDLE_VALUE||!GlassReleaseClient::Get(Wide(remote->url),download.h,nullptr,remote->size,cancel,error)||
                !FlushFileBuffers(download.h))throw std::runtime_error("Package download failed");}
            {Handle file;file.h=CreateFileW(part.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
             if(file.h==INVALID_HANDLE_VALUE||!GlassReleaseClient::VerifyHash(file.h,remote->size,remote->sha256,cancel,error))
                 throw std::runtime_error("Downloaded package failed verification");
             if(!CopyLocked(file.h,staged,cancel))throw std::runtime_error("Cannot snapshot downloaded package");}
        } else {
            Handle file;file.h=CreateFileW(sources[index].c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            if(file.h==INVALID_HANDLE_VALUE||!CopyLocked(file.h,staged,cancel))throw std::runtime_error("Cannot snapshot local package");
            // Revalidate the immutable private copy, not only a previous path query.
            const bool current=candidates[index].source==Source::Current&&request.installed&&
                GlassMaintenanceCache::MatchesInstalled(sources[index],spec.code,GlassInstallMsi::QueryDisplayVersion(spec.code))&&
                GlassMaintenanceCache::ControllerDigest(staged)==GlassMaintenanceCache::ControllerDigest(sources[index]);
            const bool digest=remote&&remote->downloadable&&Compare(candidates[index].version,remote->version)==0&&
                GlassReleaseClient::VerifyHash(file.h,remote->size,remote->sha256,cancel,error);
            if(!current&&!digest&&!SameMsiPublisher(staged))throw std::runtime_error("Local package changed or is not trusted");
        }
        Version actual;std::wstring code,display;
        if(cancel.load()||!Metadata(staged,spec,actual,code,display)||!choice.target||Compare(actual,*choice.target)!=0)
            throw std::runtime_error("Acquired MSI identity/version does not match target");
        DeleteFileW(part.c_str());error.clear();
        InstallLog::LineFormat(L"Acquired %s version=%s source=%u path=%s",name.c_str(),display.c_str(),
            static_cast<unsigned>(candidates[index].source),staged.c_str());
        return true;
    }catch(const std::exception& e){if(error.empty())error=Wide(e.what());}
    catch(...){error=L"Cannot acquire MSI package";}
    if(!part.empty())DeleteFileW(part.c_str());if(!staged.empty())DeleteFileW(staged.c_str());return false;
}
}
