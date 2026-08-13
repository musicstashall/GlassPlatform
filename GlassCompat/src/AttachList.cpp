#include "CompatPch.h"
#include "AttachList.h"
#include <DebugLog.h>
#include <algorithm>

std::unordered_map<std::wstring, DWORD> AttachList::m_allowList;

void AttachList::Initialize()
{
    m_allowList.clear();

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\StanislavKa\\BlackWindowManager\\AttachList",
        0, KEY_READ, &hKey) != ERROR_SUCCESS)
    {
        return;
    }

    DWORD index = 0;
    wchar_t valueName[256];
    DWORD valueNameSize;
    DWORD valueType;
    DWORD valueData;
    DWORD valueDataSize;

    while (true)
    {
        valueNameSize = _countof(valueName);
        valueDataSize = sizeof(valueData);

        LONG rc = RegEnumValueW(
            hKey, index++,
            valueName, &valueNameSize,
            nullptr,
            &valueType,
            reinterpret_cast<BYTE*>(&valueData),
            &valueDataSize);

        if (rc != ERROR_SUCCESS)
            break;

        if (valueType == REG_DWORD)
        {
            std::wstring exe = Normalize(valueName);
            m_allowList.emplace(exe, valueData);
        }
    }

    RegCloseKey(hKey);
}

void AttachList::LogAll()
{
    for (const auto& [name, rawValue] : m_allowList)
    {
        const wchar_t* modeText = L"Forbidden";
        if (rawValue >= 2)
            modeText = L"Full";
        else if (rawValue == 1)
            modeText = L"PopupOnly";

        DebugLog(DBG_IMPORTANT,
            L"[GlassCompat][AttachList] %s = %u (%s)",
            name.c_str(), rawValue, modeText);
    }
}

Shared::InjectionMode AttachList::GetProcessMode(const std::wstring& exeName)
{
    std::wstring key = Normalize(exeName);
    auto it = m_allowList.find(key);
    if (it == m_allowList.end())
    {
        if (key == L"logviewer.exe")
        {
            DebugLog(DBG_IMPORTANT,
                L"[GlassCompat][AttachList] %s -> Full (пилот: Tab/Toolbar/ListView в LogViewer)",
                key.c_str());
            return Shared::InjectionMode::Full;
        }
        // UICompat CoreWindow hosts: нужен Full (Attach Special), не PopupOnly по умолчанию.
        if (key == L"shellexperiencehost.exe"
            || key == L"shellhost.exe"
            || key == L"startmenuexperiencehost.exe"
            || key == L"searchhost.exe"
            || key == L"searchapp.exe")
        {
            DebugLog(DBG_IMPORTANT,
                L"[GlassCompat][AttachList] %s -> Full (UICompat CoreWindow host)",
                key.c_str());
            return Shared::InjectionMode::Full;
        }
        DebugLog(DBG_NORMAL,
            L"[GlassCompat][AttachList] %s -> default PopupOnly",
            key.c_str());
        return Shared::InjectionMode::PopupOnly;
    }

    DWORD raw = it->second;
    Shared::InjectionMode mode =
        (raw >= 2) ? Shared::InjectionMode::Full :
        (raw == 1) ? Shared::InjectionMode::PopupOnly :
                     Shared::InjectionMode::Forbidden;

    DebugLog(DBG_IMPORTANT,
        L"[GlassCompat][AttachList] %s -> mode=%u (raw=%u)",
        key.c_str(), static_cast<DWORD>(mode), raw);

    return mode;
}

bool AttachList::IsProcessAllowed(const std::wstring& exeName)
{
    return GetProcessMode(exeName) != Shared::InjectionMode::Forbidden;
}

std::wstring AttachList::Normalize(const std::wstring& exeName)
{
    std::wstring result = exeName;
    std::transform(result.begin(), result.end(), result.begin(), ::towlower);
    return result;
}
