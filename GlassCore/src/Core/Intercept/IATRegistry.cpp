// GlassCore/src/Core/Intercept/IATRegistry.cpp
//
// Реализация IATRegistry.
// Подробное описание архитектуры — в IATRegistry.h.
//
#include "CorePch.h"
#include <Core/Intercept/IATRegistry.h>
#include <Core/Intercept/HookHelper.h>   // Hooks::WriteMemory

#include <algorithm>
#include <cassert>
#include <cstdio>

namespace GlassCore::Hooks
{
    // =========================================================================
    // Синглтон
    // =========================================================================

    IATRegistry& IATRegistry::Instance() noexcept
    {
        static IATRegistry s_instance;
        return s_instance;
    }

    // =========================================================================
    // Пул трамплинов — bump-allocator из RWX-страниц
    // =========================================================================

    uint8_t* IATRegistry::AllocRaw(size_t bytes, size_t align)
    {
        auto lock = m_pageLock.lock_exclusive();

        for (auto& page : m_pages)
        {
            // Выравниваем смещение
            size_t offset = (page.used + align - 1) & ~(align - 1);
            if (offset + bytes <= Page::kCapacity)
            {
                page.used = offset + bytes;
                return page.base + offset;
            }
        }

        // Нужна новая страница RWX
        auto* raw = static_cast<uint8_t*>(VirtualAlloc(
            nullptr,
            Page::kCapacity,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE
        ));
        if (!raw)
        {
            //DebugLog(L"[IATRegistry] VirtualAlloc failed for trampoline page");
            return nullptr;
        }

        auto& page = m_pages.emplace_back();
        page.base = raw;

        size_t offset = 0; // новая страница всегда выровнена по kCapacity
        page.used = offset + bytes;
        return raw + offset;
    }

    void IATRegistry::PreallocatePages(size_t pageCount)
    {
        auto lock = m_pageLock.lock_exclusive();

        size_t ready = 0;
        for (auto& page : m_pages)
        {
            if (page.used < Page::kCapacity && ++ready >= pageCount)
                return;
        }
        while (ready < pageCount)
        {
            auto* raw = static_cast<uint8_t*>(VirtualAlloc(
                nullptr,
                Page::kCapacity,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_EXECUTE_READWRITE
            ));
            if (!raw)
                return;
            auto& page = m_pages.emplace_back();
            page.base = raw;
            page.used = 0;
            ++ready;
        }
    }

    IATRegistry::Trampoline* IATRegistry::AllocTrampoline(void* initialNext)
    {
        // FIX owner-draw (dmdskmgr): сначала reuse из free-листа. Кодировку
        // обновлять не нужно: адрес next неизменен (x86) либо RIP-relative (x64).
        // Публикация next — через Interlocked, как в UpdateTrampolineNext.
        {
            auto freeLock = m_pageLock.lock_exclusive();
            if (!m_freeTramps.empty())
            {
                Trampoline* t = m_freeTramps.back();
                m_freeTramps.pop_back();
                UpdateTrampolineNext(t, initialNext);
                return t;
            }
        }
        // Аллоцируем с выравниванием 16 байт, чтобы поле next (offset=8)
        // оказалось выровнено → InterlockedExchangePointer атомарен.
        auto* mem = AllocRaw(sizeof(Trampoline), alignof(Trampoline));
        if (!mem)
            return nullptr;

        auto* t = new(mem) Trampoline{};
        t->next = initialNext;
        // x86: вписать абсолютный &next в FF 25 <addr> (x64 Finalize — no-op).
        t->FinalizeEncoding();
        return t;
    }

    // =========================================================================
    // Атомарное обновление next_ptr трамплина
    // =========================================================================

    // static
    void IATRegistry::UpdateTrampolineNext(Trampoline* t, void* newNext) noexcept
    {
        // Поле `next` лежит на 8-байтовой границе (см. static_assert).
        // InterlockedExchangePointer — атомарная операция; не нужно
        // приостанавливать потоки, которые в данный момент исполняют
        // трамплин. Процессор x64 гарантирует атомарность 64-битной
        // aligned-записи, Interlocked добавляет memory barrier.
        InterlockedExchangePointer(&t->next, newNext);
    }

    // =========================================================================
    // Register
    // =========================================================================

    bool IATRegistry::Register(void** iatSlot, void* detour, void** outOriginal)
    {
        if (!iatSlot || !detour || !outOriginal)
            return false;

        auto key = reinterpret_cast<uintptr_t>(iatSlot);
        auto lock = m_slotLock.lock_exclusive();

        auto it = m_slots.find(key);

        // -----------------------------------------------------------------
        // Первый регистрант в этом слоте
        // -----------------------------------------------------------------
        if (it == m_slots.end())
        {
            void* originalFunc = *iatSlot;

            Hooks::WriteMemory(iatSlot, [&]
                {
                    *iatSlot = detour;
                });

            SlotRecord rec;
            rec.iatSlot = iatSlot;
            rec.original = originalFunc;
            rec.chain.push_back({ detour, nullptr }); // хвостовое звено, трамплина нет

            m_slots.emplace(key, std::move(rec));

            *outOriginal = originalFunc;

            /*DebugLog(
                L"[IATRegistry] Register (first): slot=%p detour=%p orig=%p",
                iatSlot, detour, originalFunc
            );*/
            return true;
        }

        // -----------------------------------------------------------------
        // Слот уже занят — строим цепочку
        // -----------------------------------------------------------------
        auto& rec = it->second;

        // Дубликаты запрещены
        for (auto& link : rec.chain)
        {
            if (link.detour == detour)
            {
                //DebugLog(L"[IATRegistry] Register: duplicate detour=%p, ignored", detour);
                return false;
            }
        }

        // Текущее значение в IAT-слоте — внешний detour предыдущей цепочки.
        // Аллоцируем трамплин, указывающий на него.
        void* currentIATValue = *iatSlot;
        auto* tramp = AllocTrampoline(currentIATValue);
        if (!tramp)
        {
            //DebugLog(L"[IATRegistry] Register: trampoline alloc failed");
            return false;
        }

        // Записываем новый detour в IAT-слот
        Hooks::WriteMemory(iatSlot, [&]
            {
                *iatSlot = detour;
            });

        // Вставляем в голову цепочки
        // chain[0] = новое внешнее звено, его трамплин ведёт к бывшей голове
        rec.chain.insert(rec.chain.begin(), { detour, tramp });

        // Новый диспетчер вызывает трамплин как свой «оригинал»
        *outOriginal = tramp->Address();

        /*DebugLog(
            L"[IATRegistry] Register (chain): slot=%p detour=%p tramp=%p -> %p",
            iatSlot, detour, tramp->Address(), currentIATValue
        );*/
        return true;
    }

    // =========================================================================
    // Unregister
    // =========================================================================

    // Схема для цепочки [A, B, C] (A — голова, C — хвост → original):
    //
    //   IAT → detour_A
    //              │ (A.GetOrg() = tramp_A.Address())
    //              ▼
    //           tramp_A: JMP [next_ptr_A = detour_B]
    //              │
    //              ▼
    //           detour_B
    //              │ (B.GetOrg() = tramp_B.Address())
    //              ▼
    //           tramp_B: JMP [next_ptr_B = detour_C]
    //              │
    //              ▼
    //           detour_C (хвост, trampoline=nullptr)
    //              │ (C.GetOrg() = original, сохранён при Register)
    //              ▼
    //           original
    //
    // Удаление B (idx=1):
    //   nextAddr = chain[2].detour = detour_C
    //   idx > 0  → обновляем tramp_A.next_ptr = detour_C
    //   Освобождаем tramp_B
    //   chain = [A(tramp_A→detour_C), C(nullptr)]
    //
    // Удаление A (idx=0):
    //   nextAddr = chain[1].detour = detour_B  (или tramp ведущий к B)
    //   idx == 0 → пишем nextAddr в IAT-слот
    //   Освобождаем tramp_A
    //   chain = [B(tramp_B→detour_C), C(nullptr)]
    //
    bool IATRegistry::Unregister(void** iatSlot, void* detour)
    {
        if (!iatSlot || !detour)
            return false;

        auto key = reinterpret_cast<uintptr_t>(iatSlot);
        auto lock = m_slotLock.lock_exclusive();

        auto it = m_slots.find(key);
        if (it == m_slots.end())
            return false;

        auto& rec = it->second;
        auto& chain = rec.chain;

        // Ищем позицию detour в цепочке
        auto pos = std::find_if(chain.begin(), chain.end(),
            [detour](const ChainLink& l) { return l.detour == detour; });

        if (pos == chain.end())
        {
            //DebugLog(L"[IATRegistry] Unregister: detour=%p not found in slot=%p", detour, iatSlot);
            return false;
        }

        const size_t idx = static_cast<size_t>(pos - chain.begin());

        // Определяем адрес, на который должны смотреть те, кто ссылался
        // на текущее звено: следующий detour или original (если это хвост).
        void* nextAddr = (idx + 1 < chain.size())
            ? chain[idx + 1].detour
            : rec.original;

        if (idx == 0)
        {
            // Голова цепочки — обновляем IAT-слот напрямую
            Hooks::WriteMemory(iatSlot, [&]
                {
                    *iatSlot = nextAddr;
                });

            /*DebugLog(
                L"[IATRegistry] Unregister (head): slot=%p detour=%p next=%p",
                iatSlot, detour, nextAddr
            );*/
        }
        else
        {
            // Середина или хвост — обновляем трамплин предшественника.
            // chain[idx-1].trampoline → его next_ptr указывал на chain[idx].detour,
            // теперь должен указывать на nextAddr.
            Trampoline* predTramp = chain[idx - 1].trampoline;
            if (predTramp)
            {
                UpdateTrampolineNext(predTramp, nextAddr);

                /*DebugLog(
                    L"[IATRegistry] Unregister (mid): slot=%p detour=%p predTramp=%p next=%p",
                    iatSlot, detour, predTramp->Address(), nextAddr
                );*/
            }
            else
            {
                // predTramp == nullptr означало бы, что у предшественника
                // нет трамплина — это был бы хвост, но тогда idx-1 == last.
                // Такого быть не должно: хвост не имеет трамплина только если
                // он единственный. При наличии idx > 0 у pred всегда есть tramp.
                /*DebugLog(L"[IATRegistry] Unregister: unexpected null predTramp at idx=%zu", idx);
                assert(false && "IATRegistry: predecessor must have a trampoline");*/
            }
        }

        // Трамплин удаляемого звена больше не нужен.
        // Реальный free опасен — трамплин мог быть кэширован в branch predictor.
        // Безопасная альтернатива: записать в его next_ptr адрес оригинала,
        // чтобы случайный вызов (из кэша предиктора) не вёл в мусор,
        // и вернуть в reuse-пул (FIX owner-draw: шторм Arm/Disarm больше
        // не растит RWX-страницы; порядок локов — сначала m_slotLock, затем
        // m_pageLock, как в Register → AllocTrampoline).
        if (pos->trampoline)
        {
            UpdateTrampolineNext(pos->trampoline, rec.original);
            auto freeLock = m_pageLock.lock_exclusive();
            m_freeTramps.push_back(pos->trampoline);
        }

        chain.erase(pos);

        if (chain.empty())
            m_slots.erase(it);

        return true;
    }

    // =========================================================================
    // UnregisterAll
    // =========================================================================

    void IATRegistry::UnregisterAll(void** iatSlot)
    {
        if (!iatSlot)
            return;

        auto key = reinterpret_cast<uintptr_t>(iatSlot);
        auto lock = m_slotLock.lock_exclusive();

        auto it = m_slots.find(key);
        if (it == m_slots.end())
            return;

        auto& rec = it->second;

        // Восстанавливаем оригинал
        Hooks::WriteMemory(iatSlot, [&]
            {
                *iatSlot = rec.original;
            });

        // Инвалидируем трамплины (на случай кэшей предиктора, см. Unregister)
        // и возвращаем их в reuse-пул (FIX owner-draw).
        {
            auto freeLock = m_pageLock.lock_exclusive();
            for (auto& link : rec.chain)
            {
                if (link.trampoline)
                {
                    UpdateTrampolineNext(link.trampoline, rec.original);
                    m_freeTramps.push_back(link.trampoline);
                }
            }
        }

        //DebugLog(L"[IATRegistry] UnregisterAll: slot=%p restored orig=%p", iatSlot, rec.original);

        m_slots.erase(it);
    }

    // =========================================================================
    // Shutdown
    // =========================================================================

    void IATRegistry::Shutdown() noexcept
    {
        {
            auto lock = m_slotLock.lock_exclusive();
            // Снимаем все оставшиеся хуки (защита от забытого Unregister)
            for (auto& [key, rec] : m_slots)
            {
                Hooks::WriteMemory(rec.iatSlot, [&]
                    {
                        *rec.iatSlot = rec.original;
                    });
            }
            m_slots.clear();
        }

        {
            auto lock = m_pageLock.lock_exclusive();
            for (auto& page : m_pages)
            {
                if (page.base)
                    VirtualFree(page.base, 0, MEM_RELEASE);
            }
            m_pages.clear();
            m_freeTramps.clear();
        }
    }

} // namespace GlassCore::Hooks