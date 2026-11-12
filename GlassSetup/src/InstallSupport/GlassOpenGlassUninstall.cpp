#include "GlassOpenGlassPackage.h"
#include "GlassOpenGlassInstall.h"
#include "GlassOpenGlassTrust.h"
#include "GlassMaintenanceCache.h"

namespace GlassOpenGlassPackage {
using namespace GlassOpenGlassTrust;
bool RunUninstaller(const GlassOpenGlassInstall::Record& record,DWORD& exitCode,std::wstring& error) noexcept {
    exitCode=ERROR_INSTALL_FAILURE;error.clear();
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
            (void)VerifyPublisher(record.uninstaller,executable.h);
            // Inno copies the uninstaller and deletes its original EXE/data.
            // Release pins only after verification; the directory must remain
            // protected from unprivileged replacement. Never unblock originals.
        }
        std::wstring command=L"\""+record.uninstaller+L"\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /RESTARTEXITCODE=3010 /NODELETECONFIG";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
        PROCESS_INFORMATION process{};
        Require(CreateProcessW(record.uninstaller.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,
            record.directory.c_str(),&startup,&process)!=FALSE,"Cannot start official OpenGlass uninstaller");
        struct ProcessHandles {PROCESS_INFORMATION& p;~ProcessHandles(){CloseHandle(p.hThread);CloseHandle(p.hProcess);}} handles{process};
        Require(WaitForSingleObject(process.hProcess,INFINITE)==WAIT_OBJECT_0,"Cannot wait for official OpenGlass uninstaller");
        Require(GetExitCodeProcess(process.hProcess,&exitCode)!=FALSE,"Cannot read official OpenGlass uninstaller result");
        return true;
    }catch(const std::exception& e){error=L"OpenGlass: "+std::wstring(e.what(),e.what()+std::char_traits<char>::length(e.what()));}
     catch(...){error=L"OpenGlass uninstaller execution failed";}
    return false;
}

}
