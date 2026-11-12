#pragma once
#include <Windows.h>
#include <string>
#include "GlassInstallHost.h"
#include "GlassInstallRemoval.h"
#include "GlassInstallLog.h"

// Read-only service probe and a bounded stop, never service ownership transfer.
namespace GlassOpenGlassService {
inline DWORD Probe(bool& exists, bool& stopped) noexcept {
    exists=false;stopped=false;
    const SC_HANDLE scm=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);
    if(!scm)return GetLastError();
    const SC_HANDLE service=OpenServiceW(scm,L"OpenGlassHost",SERVICE_QUERY_STATUS);
    const DWORD opened=service?ERROR_SUCCESS:GetLastError();
    CloseServiceHandle(scm);
    if(!service) {
        if(opened==ERROR_SERVICE_DOES_NOT_EXIST)return ERROR_SUCCESS;
        if(opened==ERROR_SERVICE_MARKED_FOR_DELETE)
            GlassInstallLog::Line(L"OpenGlass: Windows service deletion pending (1072); close foreign handles or restart Windows");
        return opened;
    }
    exists=true;SERVICE_STATUS_PROCESS status{};DWORD needed=0;
    const BOOL ok=QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,
        reinterpret_cast<BYTE*>(&status),sizeof(status),&needed);
    const DWORD result=ok?ERROR_SUCCESS:GetLastError();
    CloseServiceHandle(service);
    if(result==ERROR_SERVICE_MARKED_FOR_DELETE)
        GlassInstallLog::Line(L"OpenGlass: Windows service deletion pending (1072); restart Windows before retrying");
    stopped=ok&&status.dwCurrentState==SERVICE_STOPPED;
    return result;
}
inline bool Check(bool requireStopped,std::wstring& error,DWORD& code) noexcept {
    bool exists=false,stopped=false;
    code=Probe(exists,stopped);
    if(code==ERROR_SUCCESS&&requireStopped&&exists&&!stopped)code=ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
    if(code==ERROR_SUCCESS)return true;
    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
    error=code==ERROR_SERVICE_MARKED_FOR_DELETE?
        (ru?L"OpenGlass: удаление службы Windows не завершено (1072). Закройте средства управления службами или перезагрузите Windows, затем повторите операцию.":
            L"OpenGlass: Windows service deletion is pending (1072). Close service management tools or restart Windows, then retry."):
        (ru?L"OpenGlass: не удалось подтвердить состояние службы. Операция не запущена. Код: ":
            L"OpenGlass: could not confirm service state. Operation was not started. Code: ")+std::to_wstring(code);
    return false;
}
inline bool PrepareRemoval(const std::wstring& directory,std::wstring& error,DWORD& code) noexcept {
    if(!Check(false,error,code))return false;
    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
    // Validate configured paths, running image and task actions before stopping.
    if(!GlassInstallRemoval::ValidateOpenGlassPaths(directory,nullptr,ru,error)) {
        code=ERROR_INSTALL_PACKAGE_REJECTED;return false;
    }
    if(!GlassInstallHost::StopOpenGlassHost(nullptr,nullptr)) {
        code=GetLastError();
        if(code==ERROR_SUCCESS)code=ERROR_INSTALL_FAILURE;
        // Keep the precise stop error, including timeout/access denied/1072.
        error=(ru?L"OpenGlass: остановка службы/задач не подтверждена; удалятор не запущен. Код: ":
            L"OpenGlass: service/task stop was not confirmed; uninstaller was not started. Code: ")+std::to_wstring(code);
        if(code==ERROR_SERVICE_MARKED_FOR_DELETE) {
            GlassInstallLog::Line(L"OpenGlass: stop failed with 1072; Windows service deletion pending");
            error+=ru?L" Перезагрузите Windows перед повторной попыткой.":L" Restart Windows before retrying.";
        }
        return false;
    }
    // StopOpenGlassHost also waits for legacy tasks. Confirm SERVICE_STOPPED
    // independently; all handles are closed before the caller launches Inno.
    return Check(true,error,code);
}
}
