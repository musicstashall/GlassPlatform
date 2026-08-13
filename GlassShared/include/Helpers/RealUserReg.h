#pragma once

// Чтение DWORD из «настоящего» HKCU пользователя через HKEY_USERS\<SID>\...
// Нужно для AppContainer (SEH/SMEH/ShellHost): виртуализированный HKEY_CURRENT_USER
// не видит Software\StanislavKa\BlackWindowManager, записанный GlassSettings в user hive.

#include <Windows.h>
#include <sddl.h>

#pragma comment(lib, "Advapi32.lib")

namespace Shared::RealUserReg
{
    [[nodiscard]] inline bool CopyW(wchar_t* dst, DWORD cchDst, const wchar_t* src) noexcept
    {
        if (!dst || !src || cchDst == 0)
            return false;
        const size_t n = wcslen(src);
        if (n + 1 > static_cast<size_t>(cchDst))
            return false;
        memcpy(dst, src, (n + 1) * sizeof(wchar_t));
        return dst[0] != L'\0';
    }

    [[nodiscard]] inline bool TryGetUserSidString(wchar_t* out, DWORD cchOut) noexcept
    {
        if (!out || cchOut == 0)
            return false;
        out[0] = L'\0';

        HANDLE hToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
            return false;

        DWORD need = 0;
        (void)GetTokenInformation(hToken, TokenUser, nullptr, 0, &need);
        if (need == 0 || need > 4096)
        {
            CloseHandle(hToken);
            return false;
        }

        alignas(TOKEN_USER) BYTE buf[4096]{};
        if (!GetTokenInformation(hToken, TokenUser, buf, need, &need))
        {
            CloseHandle(hToken);
            return false;
        }
        CloseHandle(hToken);

        const auto* tu = reinterpret_cast<const TOKEN_USER*>(buf);
        LPWSTR sidStr = nullptr;
        if (!tu || !tu->User.Sid || !ConvertSidToStringSidW(tu->User.Sid, &sidStr) || !sidStr)
            return false;

        const bool ok = CopyW(out, cchOut, sidStr);
        LocalFree(sidStr);
        return ok;
    }

    // Сначала HKEY_CURRENT_USER, при неуспехе — HKEY_USERS\<SID>\<subKey>.
    // outSource: 0=hkcu, 1=users, 2=fail (для диагностики).
    [[nodiscard]] inline LSTATUS GetDword(
        const wchar_t* subKey,
        const wchar_t* valueName,
        DWORD& outValue,
        int* outSource = nullptr) noexcept
    {
        if (outSource)
            *outSource = 2;

        DWORD cb = sizeof(outValue);
        const LSTATUS lsHkcu = RegGetValueW(
            HKEY_CURRENT_USER,
            subKey,
            valueName,
            RRF_RT_REG_DWORD,
            nullptr,
            &outValue,
            &cb);
        if (lsHkcu == ERROR_SUCCESS)
        {
            if (outSource)
                *outSource = 0;
            return lsHkcu;
        }

        wchar_t sid[128]{};
        if (!TryGetUserSidString(sid, 128) || !subKey || !valueName)
            return lsHkcu;

        wchar_t usersPath[512]{};
        const size_t sidLen = wcslen(sid);
        const size_t subLen = wcslen(subKey);
        if (sidLen + 1 + subLen + 1 > 512)
            return lsHkcu;
        memcpy(usersPath, sid, sidLen * sizeof(wchar_t));
        usersPath[sidLen] = L'\\';
        memcpy(usersPath + sidLen + 1, subKey, (subLen + 1) * sizeof(wchar_t));

        cb = sizeof(outValue);
        const LSTATUS lsUsers = RegGetValueW(
            HKEY_USERS,
            usersPath,
            valueName,
            RRF_RT_REG_DWORD,
            nullptr,
            &outValue,
            &cb);
        if (lsUsers == ERROR_SUCCESS)
        {
            if (outSource)
                *outSource = 1;
        }
        return lsUsers;
    }
}
