#pragma once
#include <string_view>

namespace GlassCompat::Symbols
{
    void Prepare();
    void Startup();
    void Shutdown();

    // Заглушка — всегда false. Реализация — в хост-процессе (не в инжектируемом GlassCore).
    bool TryResolve(
        std::wstring_view module,
        std::wstring_view symbol,
        unsigned long& outRva
    );
}
