#pragma once
#include <windows.h>
#include <cstdint>
#include <optional>

namespace GlassCore::Hooks
{
    // Заглушка под будущую замену E8 call (rel32) на detour/trampoline.
    struct E8Patch
    {
        void* callSite{ nullptr };          // адрес байта E8
        std::int32_t originalRel32{ 0 };    // оригинальный rel32
        void* originalTarget{ nullptr };    // вычисленная цель
        void* trampolinePage = nullptr;
        bool applied{ false };
    };

    // Проверяет, что по адресу действительно E8 и вычисляет цель.
    std::optional<E8Patch> InspectE8Call(void* callSite);

    // Применяет патч: E8 <rel32(detour)>.
    bool ApplyE8CallPatch(E8Patch& patch, void* detourTarget);

    // Откатывает патч к originalRel32.
    bool RevertE8CallPatch(E8Patch& patch);
}
