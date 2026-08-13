#pragma once
#include <windows.h>
#include "ExplorerWindowInfo.hpp"

class XamlIslandUtils
{
public:
    // Зарезервировано: ранний путь через DWM на мост отключён (см. реализацию).
    static void FixBackground(const ExplorerWindowInfo& info);
};
