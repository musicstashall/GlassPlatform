#include "GlassMaintenanceLaunch.h"

#include "GlassMaintenanceCache.h"
#include "GlassProductInspect.h"
#include "GlassInstallLog.h"
#include "GlassOpenGlassInstall.h"
#include "GlassOpenGlassFiles.h"

#include <GlassInstallerLaunch.h>

#include <shellapi.h>
#include <tlhelp32.h>
#include <shlwapi.h>

namespace {

[[nodiscard]] std::wstring LongPath(const std::wstring& path) noexcept
{
    if (path.empty())
        return {};
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD cch = GetLongPathNameW(path.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));
    if (cch == 0 || cch >= std::size(buffer))
        return path;
    return buffer;
}

[[nodiscard]] std::wstring ModulePath() noexcept
{
    wchar_t buffer[MAX_PATH * 2]{};
    if (GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer))) == 0)
        return {};
    return buffer;
}

[[nodiscard]] DWORD ParentProcessId(DWORD pid) noexcept
{
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD parent = 0;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (entry.th32ProcessID == pid) {
                parent = entry.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return parent;
}

[[nodiscard]] std::wstring ProcessBaseName(DWORD pid) noexcept
{
    if (!pid)
        return {};
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return {};
    wchar_t path[MAX_PATH * 2]{};
    DWORD cch = static_cast<DWORD>(std::size(path));
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &cch) != FALSE;
    CloseHandle(process);
    if (!ok)
        return {};
    const wchar_t* name = PathFindFileNameW(path);
    return name ? std::wstring(name) : std::wstring{};
}

[[nodiscard]] bool IsShellMaintenanceParent() noexcept
{
    static constexpr const wchar_t* kAllowed[] = {
        L"explorer.exe",
        L"SystemSettings.exe",
        L"ApplicationFrameHost.exe",
        L"ShellExperienceHost.exe",
        L"sihost.exe",
    };
    const std::wstring parent = ProcessBaseName(ParentProcessId(GetCurrentProcessId()));
    if (parent.empty())
        return false;
    for (const wchar_t* allowed : kAllowed) {
        if (_wcsicmp(parent.c_str(), allowed) == 0)
            return true;
    }
    return false;
}

[[nodiscard]] bool IsTrustedCachedSetupImage() noexcept
{
    const std::wstring self = LongPath(ModulePath());
    const std::wstring canonical = LongPath(GlassMaintenanceCache::CurrentFile(L"GlassSetup.exe"));
    if (self.empty() || canonical.empty())
        return false;
    if (!GlassMaintenanceCache::TrustedPath(self, false))
        return false;
    if(PathsEqualCanonical(self, canonical))return true;
    // An already-open Installed Apps window can retain an older ModifyPath.
    // Accept only our protected content-addressed entrypoint; signer and parent/
    // ticket authorization below remain mandatory, then normal handoff picks Active.
    const auto digest=GlassMaintenanceCache::ControllerDigest(self);
    if(digest.size()!=64)return false;
    const auto root=GlassMaintenanceCache::Root(),store=root+L"\\Installer";
    const auto folder=store+L"\\"+std::wstring(digest.begin(),digest.end());
    return GlassMaintenanceCache::TrustedDirectory(root)&&GlassMaintenanceCache::TrustedDirectory(store)&&
        GlassMaintenanceCache::TrustedDirectory(folder)&&PathsEqualCanonical(self,folder+L"\\GlassSetup.exe");
}

} // namespace

SetupCommandLine ParseSetupCommandLine() noexcept
{
    SetupCommandLine out;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return out;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--modify") == 0)
            out.modify = true;
        else if (_wcsicmp(argv[i], L"--product") == 0 && i + 1 < argc)
            out.productCode = argv[++i];
    }
    LocalFree(argv);
    return out;
}

bool PathsEqualCanonical(const std::wstring& left, const std::wstring& right) noexcept
{
    if (left.empty() || right.empty())
        return false;
    return CompareStringOrdinal(LongPath(left).c_str(), -1, LongPath(right).c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool MaintenanceProductAbsent(const std::wstring& code) noexcept {
    const wchar_t* known[]={GlassInstallMsi::kProductPlatformX64,GlassInstallMsi::kProductPlatformX86,
        GlassInstallMsi::kProductSettingsX64,GlassInstallMsi::kProductSettingsX86,GlassInstallMsi::kProductOpenGlass,
        GlassInstallMsi::kProductOpenGlassResourcesX64,
        GlassInstallMsi::kLegacyPlatformX64,GlassInstallMsi::kLegacyPlatformX86,GlassInstallMsi::kLegacyOpenGlass};
    bool recognized=false;for(const auto* item:known)if(!_wcsicmp(code.c_str(),item)){recognized=true;break;}
    if(!recognized)return false;
    if(!_wcsicmp(code.c_str(),GlassInstallMsi::kProductOpenGlass) ||
       !_wcsicmp(code.c_str(),GlassInstallMsi::kLegacyOpenGlass)) {
        GlassOpenGlassInstall::Record official;std::wstring error;
        // Inventory failure is not evidence of absence. Discovery will report it.
        if(!GlassOpenGlassInstall::Query(official,error)||official.installed)return false;
        GlassInstallUtil::InstallRoots roots;
        if(!GlassInstallUtil::ResolveInstallRoots(roots))return false;
        if(GlassOpenGlassFiles::HasReceipt(roots.openGlass))return false;
        // Retired managed MSI registrations are still separate presence facts.
    }
    wchar_t product[39]{};
    const UINT result=MsiEnumProductsExW(code.c_str(),L"S-1-1-0",MSIINSTALLCONTEXT_ALL,0,product,nullptr,nullptr,nullptr);
    return result==ERROR_NO_MORE_ITEMS||result==ERROR_UNKNOWN_PRODUCT;
}

bool AuthorizeMaintenanceModifyLaunch() noexcept
{
    if (!IsTrustedCachedSetupImage()) {
        GlassInstallLog::Line(L"Maintenance modify refused: setup image is not the trusted cache copy");
        return false;
    }
    const std::wstring self = ModulePath();
    if (!GlassProductInspect::FileHasSameSignerAsSetup(self)) {
        GlassInstallLog::Line(L"Maintenance modify refused: publisher mismatch");
        return false;
    }
    if(IsControllerChild()) {
        GlassInstallLog::Line(L"Maintenance modify authorized by validated controller handoff");
        return true;
    }
    if (Shared::GlassInstallerLaunch::TryConsumeMaintenanceTicket()) {
        GlassInstallLog::Line(L"Maintenance modify authorized via GlassSettings ticket");
        return true;
    }
    if (IsShellMaintenanceParent()) {
        GlassInstallLog::Line(L"Maintenance modify authorized via shell parent process");
        return true;
    }
    GlassInstallLog::Line(L"Maintenance modify refused: no ticket and parent is not shell");
    return false;
}

// Independent immutable controller publication + two-phase process handoff.
// No elevated code is launched from the download workspace.
#include "GlassSetupSourceLayout.h"
#include "../../res/resource.h"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <wintrust.h>
#include <softpub.h>

namespace {
struct ControllerHandle {
    HANDLE value=nullptr;
    ~ControllerHandle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
struct ControllerChildState {
    ControllerHandle parent,ready,commit;
    bool validated=false,modify=false,startupAckRequired=false;
    std::string record;
} controllerChild;
struct ControllerLock {
    ControllerHandle handle;
    bool acquired=false;
    ControllerLock(){
        PSECURITY_DESCRIPTOR sd=nullptr;
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;;GA;;;SY)(A;;GA;;;BA)",SDDL_REVISION_1,&sd,nullptr))return;
        SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE};
        handle.value=CreateMutexW(&sa,FALSE,L"Global\\GlassPlatform.Controller.V1");LocalFree(sd);
        if(handle.value){const auto wait=WaitForSingleObject(handle.value,0);acquired=wait==WAIT_OBJECT_0||wait==WAIT_ABANDONED;}
    }
    ~ControllerLock(){if(acquired)ReleaseMutex(handle.value);}
};
std::wstring ControllerToken(){
    GUID id{};wchar_t text[40]{};
    if(FAILED(CoCreateGuid(&id))||!StringFromGUID2(id,text,40))return {};
    return text;
}
bool ControllerTrusted(const std::wstring& path){
    DWORD binary=0;
    if(path.empty()||!GetBinaryTypeW(path.c_str(),&binary)||
       (binary!=SCS_32BIT_BINARY&&!(binary==SCS_64BIT_BINARY&&GlassInstallUtil::IsOs64Bit()))||
       !GlassProductInspect::FileHasSameSignerAsSetup(path))return false;
    WINTRUST_FILE_INFO file{};file.cbStruct=sizeof(file);file.pcwszFilePath=path.c_str();
    WINTRUST_DATA trust{};trust.cbStruct=sizeof(trust);trust.dwUIChoice=WTD_UI_NONE;
    trust.dwUnionChoice=WTD_CHOICE_FILE;trust.pFile=&file;trust.dwStateAction=WTD_STATEACTION_VERIFY;
    trust.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL|WTD_REVOCATION_CHECK_NONE;
    GUID policy=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status=WinVerifyTrust(nullptr,&policy,&trust);
    trust.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&policy,&trust);
    if(status!=ERROR_SUCCESS)GlassInstallLog::LineFormat(L"Controller Authenticode refused: status=0x%08lX path=%s",static_cast<unsigned long>(status),path.c_str());
    return status==ERROR_SUCCESS;
}
bool ControllerSupportsStartupAck(const std::wstring& path) {
    HMODULE module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if(!module)return false;
    const auto resource=FindResourceW(module,MAKEINTRESOURCEW(IDR_CONTROLLER_PROTOCOL),RT_RCDATA);
    const auto loaded=resource?LoadResource(module,resource):nullptr;
    const auto* bytes=loaded?static_cast<const BYTE*>(LockResource(loaded)):nullptr;
    const bool supported=bytes&&SizeofResource(module,resource)==2&&bytes[0]==3&&bytes[1]==0;
    FreeLibrary(module);return supported;
}
ULONGLONG ControllerVersion(const std::wstring& file){
    DWORD ignored=0;const DWORD size=GetFileVersionInfoSizeW(file.c_str(),&ignored);
    if(!size||size>1024*1024)return 0;
    std::vector<BYTE> bytes(size);VS_FIXEDFILEINFO* info=nullptr;UINT length=0;
    if(!GetFileVersionInfoW(file.c_str(),0,size,bytes.data())||
       !VerQueryValueW(bytes.data(),L"\\",reinterpret_cast<void**>(&info),&length)||length<sizeof(*info)||!info||info->dwSignature!=0xfeef04bd)return 0;
    return (static_cast<ULONGLONG>(info->dwFileVersionMS)<<32)|info->dwFileVersionLS;
}
bool WriteControllerRecord(const wchar_t* name,const std::string& record){
    using namespace GlassMaintenanceCache;
    if(!ControllerRecordValid(record))return false;
    const auto folder=Root()+L"\\Installer";
    if(!TrustedDirectory(folder))return false;
    const auto temporary=folder+L"\\Record-"+ControllerToken();
    ControllerHandle file;
    file.value=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file.value==INVALID_HANDLE_VALUE)return false;
    DWORD written=0;
    bool ok=WriteFile(file.value,record.data(),static_cast<DWORD>(record.size()),&written,nullptr)&&written==record.size()&&FlushFileBuffers(file.value);
    if(!CloseHandle(file.value))ok=false;file.value=nullptr;
    ok=ok&&HardenFile(temporary)&&MoveFileExW(temporary.c_str(),(folder+L"\\"+name).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    if(!ok)DeleteFileW(temporary.c_str());return ok;
}
bool StageController(const std::wstring& source,std::string& record){
    using namespace GlassMaintenanceCache;
    if(!ControllerTrusted(source)||!EnsureRoot())return false;
    const auto store=Root()+L"\\Installer";
    if(!SecureDirectory(store))return false;
    const auto setupHash=ControllerDigest(source);if(setupHash.size()!=64)return false;
    const auto stage=store+L"\\Stage-"+ControllerToken();
    if(!SecureDirectory(stage))return false;
    struct Cleanup {std::wstring path;~Cleanup(){if(!path.empty())GlassMaintenanceCache::RemoveFlat(path);}} cleanup{stage};
    const auto exe=stage+L"\\GlassSetup.exe",unins=stage+L"\\GlassUnins.exe";
    if(!CopyFileW(source.c_str(),exe.c_str(),TRUE)||ControllerDigest(exe)!=setupHash||
       !ControllerTrusted(exe))return false;
    // Data-file mapping does not execute the downloaded candidate's DllMain/code.
    HMODULE resources=LoadLibraryExW(exe.c_str(),nullptr,LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if(!resources)return false;
    const bool extracted=GlassInstallUtil::ExtractResourceToFile(resources,IDR_MAINTENANCE_UNINS,RT_RCDATA,unins);
    FreeLibrary(resources);
    if(!extracted||!ControllerTrusted(unins)||!HardenFile(exe)||!HardenFile(unins))return false;
    const auto uninsHash=ControllerDigest(unins);record=setupHash+uninsHash;
    if(!ControllerRecordValid(record))return false;
    const auto target=store+L"\\"+std::wstring(setupHash.begin(),setupHash.end());
    if(GetFileAttributesW(target.c_str())==INVALID_FILE_ATTRIBUTES){
        if(!MoveFileExW(stage.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH))return false;
        cleanup.path.clear();
    }
    return !ControllerFileForRecord(record,L"GlassSetup.exe").empty()&&!ControllerFileForRecord(record,L"GlassUnins.exe").empty();
}
// Called only under the controller mutex. Pending is intent, Confirmed is the
// durable startup-success boundary. Never execute a record merely because it is
// present in the journal.
bool RecoverControllerTransition() {
    using namespace GlassMaintenanceCache;
    const auto pending=ReadControllerRecord(L"Pending");
    if(pending.empty())return GetFileAttributesW((Root()+L"\\Installer\\Pending").c_str())==INVALID_FILE_ATTRIBUTES;
    const auto active=ReadControllerRecord(),confirmed=ReadControllerRecord(L"Confirmed");
    if(confirmed.empty()&&GetFileAttributesW((Root()+L"\\Installer\\Confirmed").c_str())!=INVALID_FILE_ATTRIBUTES)return false;
    if(active==pending&&confirmed!=pending&&!confirmed.empty()) {
        if(!ControllerTrusted(ControllerFileForRecord(confirmed,L"GlassSetup.exe"))||
           !ControllerTrusted(ControllerFileForRecord(confirmed,L"GlassUnins.exe")))return false;
        if(!WriteControllerRecord(L"Rejected",pending)||!WriteControllerRecord(L"Active",confirmed))return false;
        GlassInstallLog::Line(L"Controller recovery: interrupted unconfirmed startup rolled back");
    }
    // First publication has no rollback target: retain the verified immutable
    // candidate and retry its startup, never invent an older trusted controller.
    return DeleteFileW((Root()+L"\\Installer\\Pending").c_str())!=FALSE;
}
struct ControllerCatalogSnapshot {
    std::wstring path;
    ~ControllerCatalogSnapshot(){if(!path.empty())DeleteFileW(path.c_str());}
    bool Write() {
        using namespace GlassMaintenanceCache;
        const auto& text=GlassSetupSourceLayout::catalogText;
        if(text.size()>1024*1024)return false;
        path=Root()+L"\\Installer\\Catalog-"+ControllerToken();
        ControllerHandle file;
        file.value=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file.value==INVALID_HANDLE_VALUE){path.clear();return false;}
        DWORD written=0;
        const bool ok=WriteFile(file.value,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)&&
            written==text.size()&&FlushFileBuffers(file.value);
        CloseHandle(file.value);file.value=nullptr;
        return ok&&HardenFile(path);
    }
};
bool ReadControllerCatalog(const std::wstring& path) {
    using namespace GlassMaintenanceCache;
    const auto prefix=Root()+L"\\Installer\\Catalog-";
    if(path.rfind(prefix,0)!=0||path.substr(prefix.size()).find_first_of(L"\\/:")!=std::wstring::npos||
       !GlassInstallUtil::IsSafeInstallPath(path)||!TrustedPath(path,false))return false;
    ControllerHandle file;
    file.value=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file.value==INVALID_HANDLE_VALUE)return false;
    std::vector<char> bytes(1024*1024+1);DWORD read=0;
    if(!ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)||read>1024*1024)return false;
    GlassSetupSourceLayout::catalogText.assign(bytes.data(),read);
    GlassSetupSourceLayout::catalogInherited=true;return true;
}
// Quote only explicitly supported intent parameters; never forward an arbitrary
// original command line (which may contain old handoff switches/Apply commands).
std::wstring ControllerQuote(const std::wstring& value){
    if(value.find(L'"')!=std::wstring::npos||value.find(L'\r')!=std::wstring::npos||value.find(L'\n')!=std::wstring::npos)return {};
    std::wstring escaped=value;
    const auto last=escaped.find_last_not_of(L'\\');
    const auto trailing=last==std::wstring::npos?escaped.size():escaped.size()-last-1;
    escaped.append(trailing,L'\\');
    return L"\""+escaped+L"\"";
}
bool ParseControllerHandle(const std::wstring& text,HANDLE& result){
    if(text.empty()||text.size()>20)return false;
    for(wchar_t c:text)if(c<L'0'||c>L'9')return false;
    wchar_t* end=nullptr;const auto value=_wcstoui64(text.c_str(),&end,10);
    if(!value||!end||*end||value>static_cast<ULONGLONG>(UINTPTR_MAX))return false;
    result=reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
    DWORD flags=0;return GetHandleInformation(result,&flags)!=FALSE&&(flags&HANDLE_FLAG_INHERIT)!=0;
}
}

bool IsControllerChild() noexcept {return controllerChild.validated;}

bool BeginControllerChild(std::wstring& error) noexcept {
    try {
        int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
        if(!argv)return false;
        std::vector<std::wstring> args;for(int i=1;i<argc;++i)args.emplace_back(argv[i]);LocalFree(argv);
        bool requested=false;
        std::wstring parent,ready,commit,record,layout,catalog;
        for(size_t i=0;i<args.size();++i){
            if(args[i]==L"--controller-child")requested=true;
            else if(args[i]==L"--controller-startup-ack")controllerChild.startupAckRequired=true;
            else if(i+1<args.size()&&args[i]==L"--controller-parent")parent=args[++i];
            else if(i+1<args.size()&&args[i]==L"--controller-ready")ready=args[++i];
            else if(i+1<args.size()&&args[i]==L"--controller-commit")commit=args[++i];
            else if(i+1<args.size()&&args[i]==L"--controller-record")record=args[++i];
            else if(i+1<args.size()&&args[i]==L"--source-layout")layout=args[++i];
            else if(i+1<args.size()&&args[i]==L"--controller-catalog")catalog=args[++i];
            else if(args[i]==L"--modify")controllerChild.modify=true;
        }
        if(!requested)return true;
        error=L"Invalid or expired controller handoff";
        if(!ParseControllerHandle(parent,controllerChild.parent.value)||!ParseControllerHandle(ready,controllerChild.ready.value)||
           !ParseControllerHandle(commit,controllerChild.commit.value))return false;
        const DWORD pid=GetProcessId(controllerChild.parent.value);
        if(!pid||pid!=ParentProcessId(GetCurrentProcessId())||WaitForSingleObject(controllerChild.parent.value,0)!=WAIT_TIMEOUT)return false;
        wchar_t image[32768]{};DWORD count=32768;
        if(!QueryFullProcessImageNameW(controllerChild.parent.value,0,image,&count)||!ControllerTrusted(image))return false;
        if(record.size()!=128)return false;
        for(wchar_t c:record)if(c>127)return false;
        controllerChild.record.assign(record.begin(),record.end());
        const auto self=GlassMaintenanceCache::ControllerFileForRecord(controllerChild.record,L"GlassSetup.exe");
        if(self.empty()||!PathsEqualCanonical(self,ModulePath()))return false;
        // Only the three explicit inherited handles belong to this protocol.
        SetHandleInformation(controllerChild.parent.value,HANDLE_FLAG_INHERIT,0);
        SetHandleInformation(controllerChild.ready.value,HANDLE_FLAG_INHERIT,0);
        SetHandleInformation(controllerChild.commit.value,HANDLE_FLAG_INHERIT,0);
        if(!catalog.empty()&&!ReadControllerCatalog(catalog))return false;
        GlassSetupSourceLayout::directory=layout;
        controllerChild.validated=true;error.clear();return true;
    }catch(...){error=L"Cannot validate controller handoff";return false;}
}

bool CompleteControllerChild(bool windowReady) noexcept {
    if(!controllerChild.validated)return true;
    if(!windowReady||!SetEvent(controllerChild.ready.value))return false;
    HANDLE waits[]={controllerChild.commit.value,controllerChild.parent.value};
    if(WaitForMultipleObjects(2,waits,FALSE,30000)!=WAIT_OBJECT_0)return false;
    // Recheck the atomic active pointer after the parent has committed it.
    return GlassMaintenanceCache::ReadControllerRecord()==controllerChild.record&&
        (!controllerChild.startupAckRequired||ResetEvent(controllerChild.commit.value));
}

bool ConfirmControllerStartup(bool ready) noexcept {
    if(!IsControllerChild()||!controllerChild.startupAckRequired)return ready; // Validated v1 parent compatibility.
    if(!ready||!SetEvent(controllerChild.ready.value))return false;
    HANDLE waits[]={controllerChild.commit.value,controllerChild.parent.value};
    return WaitForMultipleObjects(2,waits,FALSE,30000)==WAIT_OBJECT_0&&
        GlassMaintenanceCache::ReadControllerRecord(L"Confirmed")==controllerChild.record&&
        GlassMaintenanceCache::ReadControllerRecord()==controllerChild.record;
}

ControllerTransition EnsureCurrentController(const std::wstring& downloaded,bool modify,const std::wstring& product,std::wstring& error) noexcept {
    try {
        using namespace GlassMaintenanceCache;
        error.clear();
        if(IsControllerChild())return ControllerTransition::Continue; // One transition per launch chain.
        ControllerLock lock;
        if(!lock.acquired){error=L"Another controller update is in progress";return ControllerTransition::Failed;}
        if(!RecoverControllerTransition()){error=L"Controller journal recovery failed";return ControllerTransition::Failed;}
        const auto self=ModulePath();const auto selfVersion=ControllerVersion(self);
        if(!selfVersion){error=L"Cannot identify running controller version";return ControllerTransition::Failed;}
        const auto rejected=ReadControllerRecord(L"Rejected");
        const bool rejectedSelf=!rejected.empty()&&!ControllerFile(L"GlassSetup.exe").empty()&&ControllerDigest(self)==rejected.substr(0,64);
        auto source=rejectedSelf?std::wstring{}:self;auto version=rejectedSelf?0:selfVersion;
        if(!downloaded.empty()&&(!ControllerVersion(downloaded)||!ControllerTrusted(downloaded))) {
            error=L"Downloaded controller verification failed before publication";return ControllerTransition::Failed;
        }
        const auto active=ControllerFile(L"GlassSetup.exe");
        if(GetFileAttributesW((Root()+L"\\Installer\\Active").c_str())!=INVALID_FILE_ATTRIBUTES&&active.empty()) {
            error=L"The active controller record or code is invalid; automatic downgrade was refused";
            return ControllerTransition::Failed;
        }
        if(!active.empty()&&(!ControllerVersion(active)||!ControllerTrusted(active))) {
            error=L"The installed controller publisher or Authenticode verification failed";return ControllerTransition::Failed;
        }
        if(!active.empty()&&ControllerVersion(active)>=version&&ControllerTrusted(active)&&ControllerSupportsStartupAck(active)){
            source=active;version=ControllerVersion(active);
        }
        if(!downloaded.empty()&&ControllerVersion(downloaded)>version&&ControllerTrusted(downloaded)&&
           (active.empty()||rejected.empty()||ControllerDigest(downloaded)!=rejected.substr(0,64))){
            source=downloaded;version=ControllerVersion(downloaded);
        }
        if(source.empty()){error=L"No controller with a successful or new startup candidate is available";return ControllerTransition::Failed;}
        std::string record;
        if(!active.empty()&&PathsEqualCanonical(source,active)) {
            record=ReadControllerRecord();
            if(!ControllerTrusted(ControllerFileForRecord(record,L"GlassUnins.exe"))) {
                error=L"The installed controller uninstaller could not be verified";return ControllerTransition::Failed;
            }
        } else if(!StageController(source,record)){
            error=L"Cannot stage the verified controller";return ControllerTransition::Failed;
        }
        const auto target=ControllerFileForRecord(record,L"GlassSetup.exe");
        if((!active.empty()&&version<ControllerVersion(active))||ControllerVersion(target)!=version||!ControllerSupportsStartupAck(target)) {
            error=L"Staged controller version changed during publication";return ControllerTransition::Failed;
        }
        const auto previous=ReadControllerRecord();
        if(PathsEqualCanonical(self,target)){
            // A manually launched immutable candidate has completed startup too,
            // but still needs the same durable intent/confirmation boundary.
            if(previous!=record&&(!WriteControllerRecord(L"Pending",record)||
               (!previous.empty()&&!WriteControllerRecord(L"Previous",previous))||!WriteControllerRecord(L"Active",record))){
                error=L"Cannot journal and activate controller";return ControllerTransition::Failed;
            }
            if(!WriteControllerRecord(L"Confirmed",record)){
                error=L"Cannot confirm controller";return ControllerTransition::Failed;
            }
            if(GetFileAttributesW((Root()+L"\\Installer\\Pending").c_str())!=INVALID_FILE_ATTRIBUTES)
                DeleteFileW((Root()+L"\\Installer\\Pending").c_str());
            return ControllerTransition::Continue;
        }
        // Durable pending record: a crash before Active replacement leaves the
        // previous active controller intact. A crash after replacement leaves a
        // fully verified, already-started candidate, not a half-written EXE.
        if(!WriteControllerRecord(L"Pending",record)){error=L"Cannot journal controller transition";return ControllerTransition::Failed;}
        ControllerCatalogSnapshot catalogSnapshot;
        if(!catalogSnapshot.Write()){error=L"Cannot snapshot the controller catalog";return ControllerTransition::Failed;}
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
        ControllerHandle ready,commit,parent;
        ready.value=CreateEventW(&sa,TRUE,FALSE,nullptr);commit.value=CreateEventW(&sa,TRUE,FALSE,nullptr);
        if(!ready.value||!commit.value||!DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&parent.value,
            SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,TRUE,0)){error=L"Cannot prepare controller handshake";return ControllerTransition::Failed;}
        STARTUPINFOEXW start{};start.StartupInfo.cb=sizeof(start);
        SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,1,0,&size);
        std::vector<BYTE> attributes(size);start.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if(!InitializeProcThreadAttributeList(start.lpAttributeList,1,0,&size)){error=L"Cannot initialize handle allowlist";return ControllerTransition::Failed;}
        struct DeleteAttributes {LPPROC_THREAD_ATTRIBUTE_LIST list;~DeleteAttributes(){DeleteProcThreadAttributeList(list);}} deleteAttributes{start.lpAttributeList};
        HANDLE inherited[]={parent.value,ready.value,commit.value};
        if(!UpdateProcThreadAttribute(start.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr)){
            error=L"Cannot restrict inherited handles";return ControllerTransition::Failed;}
        auto number=[](HANDLE h){return std::to_wstring(reinterpret_cast<uintptr_t>(h));};
        auto layout=GlassSetupSourceLayout::directory;
        if(layout.empty())layout=self.substr(0,self.find_last_of(L"\\/"));
        std::wstring command=ControllerQuote(target)+L" --controller-child --controller-startup-ack --no-self-update --controller-parent "+number(parent.value)+
            L" --controller-ready "+number(ready.value)+L" --controller-commit "+number(commit.value)+
            L" --controller-record "+std::wstring(record.begin(),record.end())+L" --source-layout "+ControllerQuote(layout)+
            L" --controller-catalog "+ControllerQuote(catalogSnapshot.path);
        if(modify){command+=L" --modify";if(!product.empty())command+=L" --product "+ControllerQuote(product);}
        PROCESS_INFORMATION process{};
        const auto folder=target.substr(0,target.find_last_of(L'\\'));
        if(!CreateProcessW(target.c_str(),command.data(),nullptr,nullptr,TRUE,EXTENDED_STARTUPINFO_PRESENT,nullptr,folder.c_str(),&start.StartupInfo,&process)){
            error=L"Cannot launch staged controller";return ControllerTransition::Failed;}
        ControllerHandle child;child.value=process.hProcess;CloseHandle(process.hThread);
        HANDLE waits[]={ready.value,child.value};
        if(WaitForMultipleObjects(2,waits,FALSE,30000)!=WAIT_OBJECT_0){error=L"New controller did not acknowledge readiness";return ControllerTransition::Failed;}
        if((!previous.empty()&&!WriteControllerRecord(L"Previous",previous))||!WriteControllerRecord(L"Active",record)){
            error=L"Cannot commit active controller";return ControllerTransition::Failed;}
        if(!ResetEvent(ready.value)||!SetEvent(commit.value)){
            if(!previous.empty())(void)WriteControllerRecord(L"Active",previous);
            else DeleteFileW((Root()+L"\\Installer\\Active").c_str());
            error=L"Cannot acknowledge controller commit";return ControllerTransition::Failed;
        }
        // READY was reset before COMMIT; a second signal now means the child
        // completed startup, not merely window creation. It still gates product UI.
        if(WaitForMultipleObjects(2,waits,FALSE,30000)!=WAIT_OBJECT_0||
           !WriteControllerRecord(L"Confirmed",record)) {
            (void)WriteControllerRecord(L"Rejected",record);
            if(!previous.empty())(void)WriteControllerRecord(L"Active",previous);
            else DeleteFileW((Root()+L"\\Installer\\Active").c_str());
            error=L"Controller startup confirmation failed; journal retained for recovery";
            return ControllerTransition::Failed;
        }
        if(!DeleteFileW((Root()+L"\\Installer\\Pending").c_str())||!SetEvent(commit.value)) {
            error=L"Controller startup acceptance failed";return ControllerTransition::Failed;
        }
        GlassInstallLog::Line(L"Controller handoff startup confirmed; product inspection is permitted");
        return ControllerTransition::Transferred;
    }catch(...){error=L"Controller transition failed";return ControllerTransition::Failed;}
}

bool FinalizeControllerStartup(bool ready) noexcept {
    if(ready&&!ConfirmControllerStartup(true))return false;
    try {
        using namespace GlassMaintenanceCache;
        const auto self=ModulePath();const auto active=ReadControllerRecord();
        if(active.empty()||!PathsEqualCanonical(self,ControllerFileForRecord(active,L"GlassSetup.exe"))||
           !ControllerTrusted(self)||!ControllerTrusted(ControllerFileForRecord(active,L"GlassUnins.exe")))return true;
        ControllerLock lock;
        if(!lock.acquired&&lock.handle.value) {
            const auto wait=WaitForSingleObject(lock.handle.value,5000);
            lock.acquired=wait==WAIT_OBJECT_0||wait==WAIT_ABANDONED;
        }
        if(!lock.acquired)return true; // No unsafe cleanup if another transition owns the lock.
        if(!ready){
            (void)WriteControllerRecord(L"Rejected",active);
            auto previous=ReadControllerRecord(L"Confirmed");
            if(previous.empty())previous=ReadControllerRecord(L"Previous");
            if(ControllerTrusted(ControllerFileForRecord(previous,L"GlassSetup.exe"))&&
               ControllerTrusted(ControllerFileForRecord(previous,L"GlassUnins.exe")))
                (void)WriteControllerRecord(L"Active",previous);
            GlassInstallLog::Line(L"Controller startup failed; previous verified pointer restored when available");
            return false;
        }
        if(!WriteControllerRecord(L"Confirmed",active))return false;
        const auto store=Root()+L"\\Installer";
        DeleteFileW((store+L"\\Previous").c_str());
        DeleteFileW((store+L"\\Pending").c_str());
        // Installed Apps caches command strings while its window remains open.
        // A broadcast cannot guarantee it rereads ModifyPath/UninstallString.
        // Keep published digest folders until explicit unused-controller retirement;
        // even an unlocked old EXE may still be a live cached entrypoint.
        // Only abandoned staging/snapshot objects are collected here.
        WIN32_FIND_DATAW data{};HANDLE find=FindFirstFileW((store+L"\\*").c_str(),&data);
        if(find!=INVALID_HANDLE_VALUE){
            do {
                const std::wstring name=data.cFileName;
                const std::string ascii(name.begin(),name.end());
                const bool stageFolder=name.rfind(L"Stage-",0)==0;
                if(stageFolder&&(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&
                   !(data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&ascii!=active.substr(0,64)) {
                    if(!RemoveFlat(store+L"\\"+name))
                        GlassInstallLog::LineFormat(L"Controller CleanupPending: %s",name.c_str());
                } else if(!(data.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&
                          (name.rfind(L"Catalog-",0)==0||name.rfind(L"Record-",0)==0)) {
                    const auto stale=store+L"\\"+name;
                    if(TrustedPath(stale,false)&&!DeleteFileW(stale.c_str()))
                        GlassInstallLog::LineFormat(L"Controller snapshot CleanupPending: %s",name.c_str());
                }
            }while(FindNextFileW(find,&data));
            FindClose(find);
        }
        GlassInstallUtil::InstallRoots roots;
        if(!GlassInstallUtil::ResolveInstallRoots(roots)||!GlassInstallMsi::RefreshManagedArp(roots.x64,roots.x86,roots.openGlass)) {
            GlassInstallLog::Line(L"Controller ready; maintenance entry refresh pending; legacy entrypoints retained");
        }
        // Legacy entrypoints can also be cached by an open Settings window.
        // Do not delete them merely because current registry writes succeeded.
        return true;
    }catch(...){GlassInstallLog::Line(L"Controller startup finalization pending");return false;}
}
