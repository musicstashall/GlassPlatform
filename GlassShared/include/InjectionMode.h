#pragma once
#include <windows.h>

namespace Shared
{
    enum class InjectionMode : DWORD
    {
        Forbidden = 0,
        PopupOnly = 1,
        Full = 2
    };
}