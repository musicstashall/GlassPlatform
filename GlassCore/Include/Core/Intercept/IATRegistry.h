// GlassCore/Include/Core/Intercept/IATRegistry.h
//
// IATRegistry — единый реестр IAT-слотов.
//
// ПРОБЛЕМА, которую решает:
//   Два диспетчера (например, MonthCalHooks и будущий DatePickerHooks)
//   хотят поставить хук на DrawThemeBackground в comctl32.dll.
//   Без реестра второй диспетчер перезапишет IAT-слот и первый потеряет
//   управление: его хук больше не вызывается.
//
// РЕШЕНИЕ — цепочка через трамплины с изменяемым указателем:
//
//   IAT-слот → detour_A
//                 │ (A вызывает GetOrg() → tramp_A)
//                 ▼
//              tramp_A: JMP [next_ptr_A] ──► detour_B
//                                                        │
//                                                        │ (B вызывает GetOrg() → original)
//                                                        ▼
//                                                    original DrawThemeBackground
//
// Трамплин 16 байт, next на offset=8 (выровнен для InterlockedExchangePointer):
//   x64: FF 25 02 00 00 00  ; JMP [RIP+2] → &next
//        00 00              ; pad
//        next (8 байт)
//   x86: FF 25 <abs &next>  ; JMP [absolute] — НЕ RIP-relative!
//        00 00              ; pad            (старый x64-stub давал JMP [0x2] → AV)
//        next (4 байта) + pad до 16
//
// Обновление next_ptr атомарно через InterlockedExchangePointer —
// не нужно приостанавливать потоки при снятии хука из середины цепочки.
//
// ПОДДЕРЖКА 2D и 3D диспетчеров:
//   Ключ реестра — абсолютный адрес void** iatSlot.
//   Диспетчер находит слот через GetIAT / GetDelayloadIAT и передаёт
//   его указатель в Register. 2D/3D — детали организации диспетчеров,
//   для реестра прозрачные.
//
// ИНТЕГРАЦИЯ с HookDispatcher:
//   В EnableHook вместо прямого WriteMemory вызывай:
//     IATRegistry::Instance().Register(iatFunc, detour, &hookTable[index].first)
//   В DisableAllHooks вместо восстановления через hookTable[i].first:
//     IATRegistry::Instance().Unregister(iatFunc, detour)
//
#pragma once

#include <windows.h>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <wil.h>

namespace GlassCore::Hooks
{
    class IATRegistry
    {
    public:
        // -----------------------------------------------------------------
        // Публичный API
        // -----------------------------------------------------------------

        // Регистрация хука в IAT-слоте.
        //
        // iatSlot     — адрес функционального указателя в IAT
        //               (то, что GetIAT / GetDelayloadIAT возвращает).
        // detour      — адрес нашей хук-функции.
        // outOriginal — [out] «эффективный оригинал», который диспетчер
        //               должен сохранить в hookTable[index].first и вызывать
        //               из своего detour:
        //                 • если слот был свободен — реальная original-функция;
        //                 • если слот уже занят    — адрес трамплина,
        //                   ведущего к предыдущей цепочке.
        //
        // Возвращает false если: iatSlot == nullptr, detour == nullptr,
        // или данный detour уже зарегистрирован в этом слоте.
        bool Register(void** iatSlot, void* detour, void** outOriginal);

        // Снятие хука из слота. Безопасно удаляет из любой позиции цепочки:
        //   • голова  — обновляет IAT-слот напрямую;
        //   • середина — атомарно обновляет next_ptr трамплина предшественника;
        //   • хвост   — просто удаляет звено.
        // Трамплин снятого звена освобождается (возвращается в пул).
        // Если цепочка опустела — запись удаляется из реестра.
        bool Unregister(void** iatSlot, void* detour);

        // Принудительное снятие всех хуков в слоте (при выгрузке модуля).
        // Восстанавливает original-функцию в IAT-слоте.
        void UnregisterAll(void** iatSlot);

        // Синглтон. Инициализируется при первом обращении,
        // живёт до конца процесса.
        static IATRegistry& Instance() noexcept;

        // Освобождение всех страниц трамплинов (вызывать из Hooks::Uninstall).
        void Shutdown() noexcept;

        // Предпрогрев исполняемых страниц (только коммит, без записей).
        // Убирает первый VirtualAlloc из видимого paint-кадра; вызывать из Engine::Startup.
        void PreallocatePages(size_t pageCount);

    private:
        // -----------------------------------------------------------------
        // Трамплин (16 байт)
        // -----------------------------------------------------------------
        //
        //  offset 8: void* next — изменяемый целевой адрес (aligned)
        //
#if defined(_M_X64) || defined(__x86_64__)
        //  offset 0: FF 25 02 00 00 00   JMP [RIP+2]
        //  offset 6: 00 00               padding
        struct alignas(16) Trampoline
        {
            uint8_t code[6]{ 0xFF, 0x25, 0x02, 0x00, 0x00, 0x00 };
            uint8_t pad[2]{};
            void* next{ nullptr };

            void* Address() noexcept { return this; }
            void FinalizeEncoding() noexcept {}
        };
#else
        // Тот же каркас 6+2+next@8, но disp — абсолютный &next (не RIP+2).
        // Нельзя держать uint32_t-поле сразу после jmpOp[2]: MSVC выровняет его на 4
        // и next съедет с offset 8.
        struct alignas(16) Trampoline
        {
            uint8_t code[6]{};
            uint8_t pad[2]{};
            void* next{ nullptr };

            void* Address() noexcept { return this; }
            void FinalizeEncoding() noexcept
            {
                code[0] = 0xFF;
                code[1] = 0x25;
                const auto abs = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&next));
                std::memcpy(code + 2, &abs, sizeof(abs));
            }
        };
#endif
        static_assert(sizeof(Trampoline) == 16, "Trampoline layout broken");
        static_assert(offsetof(Trampoline, next) == 8, "next must be at offset 8");

        // -----------------------------------------------------------------
        // Звено цепочки
        // -----------------------------------------------------------------

        struct ChainLink
        {
            void* detour{};      // адрес хук-функции этого звена
            Trampoline* trampoline{};  // трамплин, через который это звено
            // достигается из предыдущего
            // (nullptr у хвостового звена —
            //  оно вызывает original напрямую)
        };

        // -----------------------------------------------------------------
        // Запись об одном IAT-слоте
        // -----------------------------------------------------------------

        struct SlotRecord
        {
            void** iatSlot{};   // адрес IAT-слота
            void* original{};  // значение до первого хука
            std::vector<ChainLink> chain{};     // [0]=внешний (записан в IAT)
        };

        // -----------------------------------------------------------------
        // Пул исполняемых страниц для трамплинов (bump-allocator)
        // -----------------------------------------------------------------

        struct Page
        {
            uint8_t* base{};
            size_t   used{};
            static constexpr size_t kCapacity = 4096;
        };

        Trampoline* AllocTrampoline(void* initialNext);

        // Атомарное обновление next_ptr трамплина (thread-safe без
        // приостановки потоков, т.к. поле 8-байтово выровнено).
        static void UpdateTrampolineNext(Trampoline* t, void* newNext) noexcept;

        // -----------------------------------------------------------------
        // Внутреннее состояние
        // -----------------------------------------------------------------

        mutable wil::srwlock                         m_slotLock;
        std::unordered_map<uintptr_t, SlotRecord>    m_slots;

        wil::srwlock       m_pageLock;
        std::vector<Page>  m_pages;
        // FIX owner-draw (dmdskmgr): reuse снятых трамплинов вместо вечного
        // bump-роста — сессионный шторм Arm/Disarm выделял новый трамплин
        // на каждый цикл. Доступ только под m_pageLock.
        std::vector<Trampoline*> m_freeTramps;

        uint8_t* AllocRaw(size_t bytes, size_t align);
    };

} // namespace GlassCore::Hooks