#pragma once
#include <windows.h>
#include <cstddef>

namespace GlassCore::Hooks
{
    // Заглушка под будущий пул trampolines (near-code allocation).
    class TrampolinePool
    {
    public:
        TrampolinePool() = default;
        ~TrampolinePool() noexcept = default;

        TrampolinePool(const TrampolinePool&) = delete;
        TrampolinePool& operator=(const TrampolinePool&) = delete;

        // TODO: выделять исполняемую память рядом с targetModule.
        bool Initialize(HMODULE /*targetModule*/) { return true; }

        // TODO: возвращать адрес trampoline-буфера.
        void* Allocate(size_t /*bytes*/) { return nullptr; }

        void Reset() {}
    };
}
