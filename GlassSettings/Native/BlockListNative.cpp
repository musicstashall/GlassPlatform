#include <Windows.h>
#include "NativeHost.h"
#include <ProcessBlockList.h>
#include <cstring>

extern "C"
{
    __declspec(dllexport) DWORD WINAPI GlassSettings_GetBuiltInBlockListCount() noexcept
    {
        return static_cast<DWORD>(ProcessBlockList::BuiltInCount());
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_CopyBuiltInBlockListAt(
        DWORD index, wchar_t* buffer, DWORD cch) noexcept
    {
        if (!buffer || cch == 0)
            return FALSE;
        const wchar_t* name = ProcessBlockList::BuiltInAt(index);
        if (!name)
            return FALSE;
        const size_t len = wcslen(name);
        if (len + 1 > cch)
            return FALSE;
        wcsncpy_s(buffer, cch, name, _TRUNCATE);
        return TRUE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_IsBuiltInBlockList(const wchar_t* exeName) noexcept
    {
        if (!exeName || !exeName[0])
            return FALSE;
        return ProcessBlockList::IsBuiltIn(exeName) ? TRUE : FALSE;
    }
}
