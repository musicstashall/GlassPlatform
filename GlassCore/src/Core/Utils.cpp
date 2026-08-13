#include "CorePch.h"
#include "Core\Utils.h"
#include <vector>
#include <psapi.h>
#include <shlwapi.h>
#include <roapi.h>
#include <winstring.h>
#include <filesystem>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "psapi.lib")

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#ifndef WDA_NONE
#define WDA_NONE 0
#endif

namespace GlassCore::Utils
{
    std::wstring ToWString(const std::string& s)
    {
        if (s.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring ws(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &ws[0], len);
        return ws;
    }

    std::string ToString(const std::wstring& ws)
    {
        if (ws.empty()) return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
        std::string s(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &s[0], len, nullptr, nullptr);
        return s;
    }

    HMODULE GetModuleByName(const std::wstring& name)
    {
        std::vector<HMODULE> modules(1024);
        DWORD cbNeeded = 0;
        if (!EnumProcessModules(GetCurrentProcess(), modules.data(), (DWORD)(modules.size() * sizeof(HMODULE)), &cbNeeded))
            return nullptr;

        size_t count = cbNeeded / sizeof(HMODULE);
        for (size_t i = 0; i < count; ++i)
        {
            wchar_t modName[MAX_PATH] = {};
            if (GetModuleFileNameExW(GetCurrentProcess(), modules[i], modName, _countof(modName)))
            {
                std::wstring mn = modName;
                if (mn.find(name) != std::wstring::npos)
                    return modules[i];
            }
        }
        return nullptr;
    }

    // ---------------------------------------------------------
    // Получение версии модуля (major/minor)
    // ---------------------------------------------------------
    bool GetModuleVersion(HMODULE module, DWORD& major, DWORD& minor)
    {
        major = minor = 0;

        if (!module)
            return false;

        wchar_t path[MAX_PATH];
        if (!GetModuleFileNameW(module, path, MAX_PATH))
            return false;

        DWORD handle = 0;
        DWORD size = GetFileVersionInfoSizeW(path, &handle);
        if (size == 0)
            return false;

        std::vector<BYTE> buffer(size);
        if (!GetFileVersionInfoW(path, 0, size, buffer.data()))
            return false;

        VS_FIXEDFILEINFO* info = nullptr;
        UINT len = 0;
        if (!VerQueryValueW(buffer.data(), L"\\", (LPVOID*)&info, &len))
            return false;

        if (info)
        {
            major = HIWORD(info->dwFileVersionMS);
            minor = LOWORD(info->dwFileVersionMS);
            return true;
        }

        return false;
    }

    HWND SHCreateWorkerWindowW(
        _In_opt_ WNDPROC wndProc,
        _In_opt_ HWND hwndParent,
        _In_ DWORD dwExStyle,
        _In_ DWORD dwStyle,
        _In_opt_ HMENU hMenu,
        _In_opt_ void* lpParam
    )
    {
        // Используем CreateWindowExW с классом "WorkerW"
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = wndProc ? wndProc : DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"BlackWindowManagerWorkerW";
        RegisterClassExW(&wc);

        return CreateWindowExW(
            dwExStyle,
            wc.lpszClassName,
            nullptr,
            dwStyle,
            0, 0, 0, 0,
            hwndParent,
            hMenu,
            wc.hInstance,
            lpParam
        );
    }

    void CloakWindow(HWND hWnd, BOOL cloak)
    {
        if (!hWnd) return;
        DwmSetWindowAttribute(hWnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
        DwmTransitionOwnedWindow(hWnd, DWMTRANSITION_OWNEDWINDOW_REPOSITION);
    }

    std::wstring GetVersion(HMODULE module)
    {
        wchar_t path[MAX_PATH];
        if (!GetModuleFileNameW(module, path, MAX_PATH))
            return L"";

        DWORD handle = 0;
        DWORD size = GetFileVersionInfoSizeW(path, &handle);
        if (size == 0)
            return L"";

        std::vector<BYTE> buffer(size);
        if (!GetFileVersionInfoW(path, 0, size, buffer.data()))
            return L"";

        VS_FIXEDFILEINFO* info = nullptr;
        UINT len = 0;
        if (!VerQueryValueW(buffer.data(), L"\\", (LPVOID*)&info, &len) || !info)
            return L"";

        return std::format(L"{}.{}.{}.{}",
            HIWORD(info->dwFileVersionMS),
            LOWORD(info->dwFileVersionMS),
            HIWORD(info->dwFileVersionLS),
            LOWORD(info->dwFileVersionLS));
    }

    std::wstring MakeCurrentFolderFileStr(const std::wstring& fileName)
    {
        wchar_t tempPath[MAX_PATH];
        if (!GetTempPathW(MAX_PATH, tempPath))
            return L"";
        std::wstring fullPath = tempPath;
        fullPath += fileName;
        return fullPath;
    }

    RoInit::RoInit()
    {
        hr = RoInitialize(RO_INIT_MULTITHREADED);
    }

    RoInit::~RoInit()
    {
        if (SUCCEEDED(hr))
            RoUninitialize();
    }
}
