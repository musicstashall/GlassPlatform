#include "CompatPch.h"
#include "SymbolResolver.h"

namespace GlassCompat::Symbols
{
    void Prepare() {}
    void Startup() {}
    void Shutdown() {}

    bool TryResolve(
        std::wstring_view,
        std::wstring_view,
        unsigned long&)
    {
        return false;
    }
}
