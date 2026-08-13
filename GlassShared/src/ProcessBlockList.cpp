#include "SharedPch.h"
#include "ProcessBlockList.h"
#include <windows.h>
#include <string>
#include <algorithm>
#include <unordered_set>
#include <DebugLog.h>

namespace
{
    std::unordered_set<std::wstring> g_blockList;

    std::wstring NormalizeName(const std::wstring& name)
    {
        std::wstring n = name;
        std::transform(n.begin(), n.end(), n.begin(),
            [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        return n;
    }

    void LoadFromRegistry()
    {
        HKEY hKey;
        if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\StanislavKa\\BlackWindowManager\\BlockList",
            0, KEY_READ, &hKey) != ERROR_SUCCESS)
        {
            return;
        }

        wchar_t name[256];
        DWORD nameSize = 256;
        DWORD type = 0;
        DWORD value = 0;
        DWORD valueSize = sizeof(DWORD);

        for (DWORD index = 0;; index++)
        {
            nameSize = 256;
            valueSize = sizeof(DWORD);

            LONG res = RegEnumValueW(
                hKey, index,
                name, &nameSize,
                nullptr,
                &type,
                reinterpret_cast<BYTE*>(&value),
                &valueSize
            );

            if (res == ERROR_NO_MORE_ITEMS)
                break;

            if (res == ERROR_SUCCESS && type == REG_DWORD && value != 0)
            {
                g_blockList.insert(NormalizeName(name));
            }
        }

        RegCloseKey(hKey);
    }
}

namespace ProcessBlockList
{
    void Initialize()
    {
        // Статический список процессов
        static const wchar_t* kStaticBlockList[] =
        {
            // Системные процессы Windows
            L"ApplicationFrameHost.exe",
            L"backgroundTaskHost.exe",
            L"CoreWidgetProvider.exe",
            L"dwm.exe",
            L"FileCoAuth.exe",
            L"GameBar.exe",
            L"GameBarFTServer.exe",
            L"mblctr.exe",
            L"Microsoft.SharePoint.exe",
            L"mobsync.exe",
            L"msedge.exe",
            L"msedgewebview2.exe",
            L"OneDriveStandaloneUpdater.exe",
            L"PerfWatson2.exe",
            L"RuntimeBroker.exe",
            L"SDXHelper.exe",
            L"ServiceHub.ThreadedWaitDialog.exe",
            L"ServiceHub.VSDetouredHost.exe",
            // ShellExperienceHost / ShellHost / StartMenuExperienceHost / SearchHost / SearchApp —
            // UICompat CoreWindow hosts (см. docs/guides/UICompat-CoreWindow.md). Не в BlockList.
            L"sihost.exe",
            L"smartscreen.exe",
            L"splwow64.exe",
            L"spoolsv.exe",
            L"svchost.exe",
            L"taskhostw.exe",
            L"WerFault.exe",
            L"WidgetService.exe",
            L"Widgets.exe",
            L"WindowsPackageManagerServer.exe",
            L"WkStartCpl64.exe",
            L"WkStartCpl86.exe",
            L"WSHost.exe",

            // Антивирус и безопасность
            L"avpui.exe",

            // Среды разработки
            L"clangd.exe",
            L"devenv.exe",
            L"msvsmon.exe",
            L"vsdebugger.exe",
            L"ServiceHub.Host.dotnet.arm64.exe",
            L"ServiceHub.Host.dotnet.x64.exe",

            // Инструменты отладки и мониторинга
            L"apimonitor-x64.exe",
            L"apimonitor-x86.exe",
            L"dbgview.exe",
            L"dbgview64.exe",
            L"procexp.exe",
            L"procexp64.exe",
            L"procmon.exe",
            L"procmon64.exe",
            L"windbg.exe",
            L"x32dbg.exe",
            L"x64dbg.exe",

			//Собственные инструменты
			L"LogViewer.exe",
			L"GlassSettings.exe",
        };

        for (auto* exe : kStaticBlockList)
            g_blockList.insert(NormalizeName(exe));

        // Загружаем записи из реестра
        LoadFromRegistry();
    }

    bool IsBlocked(const std::wstring& exeName)
    {
        const std::wstring norm = NormalizeName(exeName);
        const bool blocked = g_blockList.contains(norm);

#ifdef _DEBUG
        if (blocked)
        {
            DebugLog(DBG_IMPORTANT,
                L"[ProcessBlockList] Blocked: %s",
                norm.c_str());
        }
#endif

        return blocked;
    }

    std::wstring Normalize(const std::wstring& exeName)
    {
        return NormalizeName(exeName);
    }

    bool IsCurrentProcessGlassToolHost() noexcept
    {
        static const bool s_isToolHost = []() noexcept -> bool
        {
            wchar_t path[MAX_PATH]{};
            const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
            if (len == 0 || len >= MAX_PATH)
                return false;

            const wchar_t* fileName = path;
            for (DWORD i = len; i > 0; --i)
            {
                if (path[i - 1] == L'\\' || path[i - 1] == L'/')
                {
                    fileName = path + i;
                    break;
                }
            }

            const std::wstring norm = NormalizeName(fileName);
            return norm == NormalizeName(L"GlassSettings.exe");
        }();
        return s_isToolHost;
    }

    bool IsCurrentProcessLogViewerHost() noexcept
    {
        static const bool s_isLogViewer = []() noexcept -> bool
        {
            wchar_t path[MAX_PATH]{};
            const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
            if (len == 0 || len >= MAX_PATH)
                return false;

            const wchar_t* fileName = path;
            for (DWORD i = len; i > 0; --i)
            {
                if (path[i - 1] == L'\\' || path[i - 1] == L'/')
                {
                    fileName = path + i;
                    break;
                }
            }

            const std::wstring norm = NormalizeName(fileName);
            return norm == NormalizeName(L"LogViewer.exe");
        }();
        return s_isLogViewer;
    }

    void AddToRegistry(const std::wstring& exeName) noexcept
    {
        std::wstring normalized = NormalizeName(exeName);

        // Сразу вносим в оперативный список — IsBlocked() начнёт возвращать true немедленно
        g_blockList.insert(normalized);

        // Создаём ключ реестра (или открываем существующий) и записываем значение
        HKEY hKey = nullptr;
        DWORD disp = 0;
        LONG rc = RegCreateKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\StanislavKa\\BlackWindowManager\\BlockList",
            0, nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            nullptr,
            &hKey,
            &disp);

        if (rc != ERROR_SUCCESS)
        {
            // DebugLog не используем — при shutdown сервер логов может быть остановлен
            wchar_t msg[256];
            swprintf_s(msg,
                L"[ProcessBlockList] AddToRegistry: RegCreateKeyExW failed '%s' err=%ld\n",
                exeName.c_str(), rc);
            OutputDebugStringW(msg);
            return;
        }

        // Значение 1 = заблокировано (LoadFromRegistry проверяет value != 0)
        DWORD value = 1;
        rc = RegSetValueExW(
            hKey,
            normalized.c_str(),
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&value),
            sizeof(value));

        RegCloseKey(hKey);

        if (rc != ERROR_SUCCESS)
        {
            wchar_t msg[256];
            swprintf_s(msg,
                L"[ProcessBlockList] AddToRegistry: RegSetValueExW failed '%s' err=%ld\n",
                normalized.c_str(), rc);
            OutputDebugStringW(msg);
        }
    }
}
