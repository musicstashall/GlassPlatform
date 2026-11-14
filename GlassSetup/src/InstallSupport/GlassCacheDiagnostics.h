#pragma once
#include <windows.h>
#include <sddl.h>
#include <string>
#include <cwchar>
#include "GlassInstallLog.h"

namespace GlassMaintenanceCache::Diagnostics {
struct Failure {
    std::wstring stage,path,operation,domain,reason;
    DWORD code=ERROR_SUCCESS;
};
struct Context {
    std::wstring stage,path;
    Failure first;
    bool failed=false;
};
inline thread_local Context* active=nullptr;
inline std::wstring CodeText(DWORD code) {
    wchar_t text[48]{};swprintf_s(text,48,L"0x%08lX (%lu)",code,code);return text;
}
inline std::wstring SystemMessage(DWORD code) {
    LPWSTR message=nullptr;
    const DWORD count=FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,code,0,reinterpret_cast<LPWSTR>(&message),0,nullptr);
    std::wstring text=count&&message?std::wstring(message,count):std::wstring();
    if(message)LocalFree(message);
    while(!text.empty()&&(text.back()==L'\r'||text.back()==L'\n'||text.back()==L' '))text.pop_back();
    return text;
}
inline std::wstring Describe(const Failure& f) {
    auto text=L"stage="+f.stage+L"; path="+f.path+L"; operation="+f.operation+
        L"; "+f.domain+L"="+CodeText(f.code);
    if(!f.reason.empty())text+=L"; "+f.reason;
    if(f.domain!=L"Policy") {const auto system=SystemMessage(f.code);if(!system.empty())text+=L"; "+system;}
    return text;
}
inline void Step(const wchar_t* stage,const std::wstring& path) {
    if(!active)return;
    active->stage=stage;active->path=path;
    GlassInstallLog::LineFormat(L"Maintenance cache: stage=%s path=%s",stage,path.c_str());
}
inline bool Fail(const wchar_t* operation,const std::wstring& path,DWORD code,
    const std::wstring& reason={},const wchar_t* domain=L"Win32") {
    if(active) {
        Failure failure{active->stage,path,operation,domain,reason,code};
        if(!active->failed){active->first=failure;active->failed=true;}
        const auto detail=Describe(failure);
        GlassInstallLog::LineFormat(L"Maintenance cache: FAILED %s",detail.c_str());
    }
    SetLastError(code);return false;
}
inline std::wstring SidText(PSID sid) {
    if(!sid||!IsValidSid(sid))return L"<missing/invalid>";
    LPWSTR text=nullptr;if(!ConvertSidToStringSidW(sid,&text))return L"<unavailable>";
    std::wstring result=text;LocalFree(text);return result;
}
// Scope preserves the first failure even if later cleanup/recovery also fails.
// Policy refusals have an explicit domain: their codes are not OS API results.
struct Publication {
    Context context;
    Context* previous=nullptr;
    std::wstring* detail=nullptr;
    bool succeeded=false;
    explicit Publication(std::wstring* out):previous(active),detail(out) {
        if(detail)detail->clear();active=&context;
        Step(L"begin",L"GlassPlatform maintenance bundle");
    }
    ~Publication() {
        if(!succeeded&&!context.failed)
            Fail(L"Publish",context.path,ERROR_GEN_FAILURE,L"Stage returned failure without a more specific diagnostic",L"Policy");
        if(!succeeded&&detail)*detail=Describe(context.first);
        GlassInstallLog::Line(succeeded?L"Maintenance cache: publication completed":L"Maintenance cache: publication incomplete (first failure above)");
        active=previous;
    }
};
}
