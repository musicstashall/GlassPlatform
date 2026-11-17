#include "NativeHost.h"

#include <DefaultProcessListsSeed.h>

// Те же умолчания, что пишет установщик: пустой AttachList/BlockList текущего пользователя.
// Уже заполненные списки не меняются. Код: 0 ничего, 1 Attach, 2 Block, 3 оба, -1 сбой.
extern "C" __declspec(dllexport) int WINAPI GlassSettings_SeedDefaultProcessLists() noexcept
{
    GlassShared::DefaultProcessLists::SeedResult result{};
    if (!GlassShared::DefaultProcessLists::SeedMissing(result))
        return -1;
    int code = 0;
    if (result.attachCreated)
        code |= 1;
    if (result.blockCreated)
        code |= 2;
    return code;
}
