#include "GlassOpenGlassPackage.h"
#include "GlassOpenGlassInstall.h"
#include "GlassOpenGlassTrust.h"
#include "GlassMaintenanceCache.h"
#include "GlassOpenGlassService.h"

namespace GlassOpenGlassPackage {
using namespace GlassOpenGlassTrust;
bool ValidateUninstaller(const GlassOpenGlassInstall::Record& record,std::wstring& error) noexcept {
    error.clear();
    try {
        GlassOpenGlassInstall::Record current;
        Require(GlassOpenGlassInstall::Query(current,error)&&record.installed&&GlassOpenGlassInstall::Same(record,current),
            "OpenGlass registration changed before uninstall");
        Require(GlassMaintenanceCache::TrustedDirectory(record.directory)&&
            GlassMaintenanceCache::TrustedPath(record.uninstaller,false)&&
            GlassMaintenanceCache::TrustedPath(record.directory+L"\\unins000.dat",false),
            "Untrusted official OpenGlass uninstaller/data");
        {
            File executable,data;Pin(executable,record.uninstaller);Pin(data,record.directory+L"\\unins000.dat");
            // Query/Same, trusted directory/path ACLs and pinned regular EXE/DAT
            // above are the trust boundary for a generated unsigned uninstaller.
            // Signed files still require the exact OG publisher and valid trust.
            (void)VerifyPublisher(record.uninstaller,executable.h,true);
            // Inno copies the uninstaller and deletes its original EXE/data.
            // Release pins only after verification; the directory must remain
            // protected from unprivileged replacement. Never unblock originals.
        }
        return true;
    }catch(const std::exception& e){error=L"OpenGlass: "+std::wstring(e.what(),e.what()+std::char_traits<char>::length(e.what()));}
     catch(...){error=L"OpenGlass uninstaller validation failed";}
    return false;
}
bool RunUninstaller(const GlassOpenGlassInstall::Record& record,DWORD& exitCode,std::wstring& error,bool deleteConfig) noexcept {
    exitCode=ERROR_INSTALL_FAILURE;error.clear();
    try {
        if(!ValidateUninstaller(record,error))return false;
        if(!GlassOpenGlassService::PrepareRemoval(record.directory,error,exitCode))return false;
        std::wstring command=L"\""+record.uninstaller+L"\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /RESTARTEXITCODE=3010 /DELETEPRESETS";
        if(!deleteConfig)command+=L" /NODELETECONFIG";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
        PROCESS_INFORMATION process{};
        Require(CreateProcessW(record.uninstaller.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,
            record.directory.c_str(),&startup,&process)!=FALSE,"Cannot start official OpenGlass uninstaller");
        struct ProcessHandles {PROCESS_INFORMATION& p;~ProcessHandles(){CloseHandle(p.hThread);CloseHandle(p.hProcess);}} handles{process};
        Require(WaitForSingleObject(process.hProcess,INFINITE)==WAIT_OBJECT_0,"Cannot wait for official OpenGlass uninstaller");
        Require(GetExitCodeProcess(process.hProcess,&exitCode)!=FALSE,"Cannot read official OpenGlass uninstaller result");
        if(exitCode==ERROR_SUCCESS) {
            bool exists=false,stopped=false;
            if(GlassOpenGlassService::Probe(exists,stopped)==ERROR_SERVICE_MARKED_FOR_DELETE)
                exitCode=ERROR_SUCCESS_REBOOT_REQUIRED;
        }
        return true;
    }catch(const std::exception& e){error=L"OpenGlass: "+std::wstring(e.what(),e.what()+std::char_traits<char>::length(e.what()));}
     catch(...){error=L"OpenGlass uninstaller execution failed";}
    return false;
}

}
