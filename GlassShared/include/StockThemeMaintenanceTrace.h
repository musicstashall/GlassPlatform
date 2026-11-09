#pragma once
#include <windows.h>
#include <string>

// Optional, same-user diagnostic transport. Never authorizes removal: only the
// helper exit code and the existing maintenance checks do that. Fixed-width
// payload is identical for x86 Setup and x64 Native; no pointers cross processes.
namespace StockThemeMaintenanceTrace {
struct Data {
    DWORD completed=0,active=0;
    HRESULT hr=E_PENDING,comHr=E_PENDING,forceHr=E_PENDING,fontHr=E_PENDING,readHr=E_PENDING;
    DWORD verified=0;
};
static_assert(sizeof(Data)==32,"Cross-bitness maintenance trace ABI");
inline bool ValidName(const wchar_t* name) {
    const std::wstring prefix=L"Local\\GlassStockTheme-";
    if(!name)return false;
    const std::wstring value=name;
    if(value.size()<=prefix.size()||value.size()>128||value.compare(0,prefix.size(),prefix))return false;
    for(size_t i=prefix.size();i<value.size();++i)
        if((value[i]<L'0'||value[i]>L'9')&&value[i]!=L'-')return false;
    return true;
}
class Writer {
    HANDLE handle_=nullptr;
    Data* data_=nullptr;
public:
    explicit Writer(const wchar_t* name) {
        if(!ValidName(name))return;
        handle_=OpenFileMappingW(FILE_MAP_WRITE,FALSE,name);
        if(handle_)data_=static_cast<Data*>(MapViewOfFile(handle_,FILE_MAP_WRITE,0,0,sizeof(Data)));
    }
    bool Ready() const {return data_!=nullptr;}
    void Complete(const Data& result) {
        if(data_){*data_=result;data_->completed=1;}
    }
    ~Writer(){if(data_)UnmapViewOfFile(data_);if(handle_)CloseHandle(handle_);}
};
class Reader {
    HANDLE handle_=nullptr;
    Data* data_=nullptr;
    std::wstring name_;
public:
    Reader() {
        name_=L"Local\\GlassStockTheme-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
        handle_=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Data),name_.c_str());
        if(handle_&&GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(handle_);handle_=nullptr;}
        if(handle_)data_=static_cast<Data*>(MapViewOfFile(handle_,FILE_MAP_ALL_ACCESS,0,0,sizeof(Data)));
        if(data_)*data_=Data{};
    }
    const wchar_t* Name() const {return data_?name_.c_str():L"";}
    const Data* Result() const {return data_&&data_->completed==1?data_:nullptr;}
    ~Reader(){if(data_)UnmapViewOfFile(data_);if(handle_)CloseHandle(handle_);}
};
}
