#pragma once
#include <windows.h>
#include <sddl.h>
namespace GlassMaintenanceReboot {
inline bool pinRequested=false;
// Volatile HKLM key survives process exit, but not a system reboot. It prevents
// source/code GC after MSI 3010 without relying on wall-clock/uptime heuristics.
inline bool Pinned(bool pin=false) {
    constexpr auto path=L"SOFTWARE\\GlassPlatform\\MaintenanceRebootPins";
    HKEY key=nullptr;
    if(pin) {
        pinRequested=true; // Even registry I/O failure pins this process's cleanup.
        PSECURITY_DESCRIPTOR sd=nullptr;
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;;GA;;;SY)(A;;GA;;;BA)",SDDL_REVISION_1,&sd,nullptr))return true;
        SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE};
        const auto result=RegCreateKeyExW(HKEY_LOCAL_MACHINE,path,0,nullptr,REG_OPTION_VOLATILE,
            KEY_READ|KEY_WOW64_64KEY,&sa,&key,nullptr);
        LocalFree(sd);if(result==ERROR_SUCCESS)RegCloseKey(key);
        return true; // Failure must not cause immediate GC either.
    }
    if(pinRequested)return true;
    const auto result=RegOpenKeyExW(HKEY_LOCAL_MACHINE,path,0,KEY_READ|KEY_WOW64_64KEY,&key);
    if(result==ERROR_SUCCESS)RegCloseKey(key);
    return result!=ERROR_FILE_NOT_FOUND&&result!=ERROR_PATH_NOT_FOUND;
}
}
