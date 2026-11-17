#pragma once

#include <windows.h>

#include <string>

// Запись списков из DefaultProcessLists.h в HKCU текущего пользователя.
// Общий путь для установщика и GlassSettings: ключ без именованных значений
// заполняется умолчаниями, уже заполненный список не меняется.
namespace GlassShared::DefaultProcessLists {

struct SeedResult
{
    bool attachCreated = false; // список был пуст и записан этим вызовом
    bool blockCreated = false;
    unsigned attachWritten = 0;
    unsigned blockWritten = 0;
    LSTATUS error = ERROR_SUCCESS;
    std::wstring detail; // причина сбоя, по-английски, для журнала
};

// false — запись в реестр не удалась. Уже записанное остаётся и описано в result.
bool SeedMissing(SeedResult& result) noexcept;

// Удаляет только списки, которые SeedMissing создал в этом результате.
void UndoSeed(const SeedResult& result) noexcept;

} // namespace GlassShared::DefaultProcessLists
