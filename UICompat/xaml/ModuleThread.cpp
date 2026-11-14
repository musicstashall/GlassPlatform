#include "UiPch.h"
#include "xaml/ModuleThread.hpp"

#include <DebugLog.h>
#include <new>

namespace
{
    // Одновременно живых потоков мало (IXDE-install, два retry, Advise + два
    // delayed CatchUp); 16 слотов — с запасом, без динамической памяти.
    constexpr int kMaxSlots = 16;

    SRWLOCK g_lock = SRWLOCK_INIT;
    HANDLE  g_slots[kMaxSlots]{};

    struct StartContext
    {
        UICompat::ModuleThread::Proc proc;
        void*                        param;
        HMODULE                      self;
    };

    [[nodiscard]] bool IsThreadFinished(HANDLE th) noexcept
    {
        return WaitForSingleObject(th, 0) == WAIT_OBJECT_0;
    }

    // Под exclusive-lock: закрыть хэндлы завершившихся потоков.
    void ReclaimFinishedLocked() noexcept
    {
        for (HANDLE& slot : g_slots)
        {
            if (slot && IsThreadFinished(slot))
            {
                CloseHandle(slot);
                slot = nullptr;
            }
        }
    }

    DWORD WINAPI ModuleThreadMain(LPVOID p) noexcept
    {
        auto* ctx = static_cast<StartContext*>(p);
        const HMODULE self = ctx->self;
        const auto    proc = ctx->proc;
        void* const   param = ctx->param;
        delete ctx;

        proc(param);

        // Снимаем свою ссылку и выходим, не возвращаясь в код модуля:
        // если ссылка была последней, образ выгружается прямо здесь.
        FreeLibraryAndExitThread(self, 0);
    }
}

namespace UICompat::ModuleThread
{
    bool Start(Proc proc, void* param, const wchar_t* tag) noexcept
    {
        if (!proc)
            return false;
        if (!tag)
            tag = L"";

        HMODULE self = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(&Start),
                &self)
            || !self)
        {
            DebugLog(DBG_ERROR, L"[UICompat][ModuleThread] %ls: GetModuleHandleEx failed GLE=%u",
                tag, static_cast<unsigned>(GetLastError()));
            return false;
        }

        auto* ctx = new (std::nothrow) StartContext{ proc, param, self };
        if (!ctx)
        {
            FreeLibrary(self);
            return false;
        }

        auto findFreeSlot = []() noexcept -> HANDLE* {
            for (HANDLE& s : g_slots)
            {
                if (!s)
                    return &s;
            }
            return nullptr;
        };

        AcquireSRWLockExclusive(&g_lock);
        HANDLE* slot = findFreeSlot();
        if (!slot)
        {
            // Хэндлы закрываем только при нехватке слотов: параллельный JoinAll
            // держит снимок хэндлов вне блокировки.
            ReclaimFinishedLocked();
            slot = findFreeSlot();
        }
        if (!slot)
        {
            ReleaseSRWLockExclusive(&g_lock);
            delete ctx;
            FreeLibrary(self);
            DebugLog(DBG_ERROR, L"[UICompat][ModuleThread] %ls: нет свободного слота (%d живых)",
                tag, kMaxSlots);
            return false;
        }

        // CREATE_SUSPENDED: хэндл попадает в слот до первой инструкции потока,
        // поэтому ActiveCount()/JoinAll() не могут «не заметить» только что
        // созданный поток.
        const HANDLE th = CreateThread(nullptr, 0, ModuleThreadMain, ctx, CREATE_SUSPENDED, nullptr);
        if (!th)
        {
            const DWORD gle = GetLastError();
            ReleaseSRWLockExclusive(&g_lock);
            delete ctx;
            FreeLibrary(self);
            DebugLog(DBG_ERROR, L"[UICompat][ModuleThread] %ls: CreateThread failed GLE=%u",
                tag, static_cast<unsigned>(gle));
            return false;
        }
        *slot = th;
        ReleaseSRWLockExclusive(&g_lock);

        if (ResumeThread(th) == static_cast<DWORD>(-1))
        {
            // Поток так и не стартует: ссылка на модуль и слот остаются занятыми
            // (утечка вместо use-after-unload). Практически недостижимо.
            DebugLog(DBG_ERROR, L"[UICompat][ModuleThread] %ls: ResumeThread failed GLE=%u",
                tag, static_cast<unsigned>(GetLastError()));
        }
        return true;
    }

    long ActiveCount() noexcept
    {
        long n = 0;
        AcquireSRWLockShared(&g_lock);
        for (const HANDLE slot : g_slots)
        {
            if (slot && !IsThreadFinished(slot))
                ++n;
        }
        ReleaseSRWLockShared(&g_lock);
        return n;
    }

    bool JoinAll(DWORD timeoutMs, const wchar_t* phase) noexcept
    {
        if (!phase)
            phase = L"";

        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        for (;;)
        {
            HANDLE pending[kMaxSlots]{};
            DWORD  count = 0;
            AcquireSRWLockShared(&g_lock);
            for (const HANDLE slot : g_slots)
            {
                if (slot && !IsThreadFinished(slot))
                    pending[count++] = slot;
            }
            ReleaseSRWLockShared(&g_lock);

            if (count == 0)
            {
                AcquireSRWLockExclusive(&g_lock);
                ReclaimFinishedLocked();
                ReleaseSRWLockExclusive(&g_lock);
                return true;
            }

            const ULONGLONG now = GetTickCount64();
            if (now >= deadline)
            {
                DebugLog(DBG_WARNING,
                    L"[UICompat][ModuleThread] %ls: %u поток(ов) не завершились за %u мс — "
                    L"образ держится их ссылками до выхода",
                    phase, static_cast<unsigned>(count), static_cast<unsigned>(timeoutMs));
                return false;
            }

            // Ждём выход потоков, просыпаясь на sent-сообщениях: retry-поток может
            // стоять в SendMessageTimeout к этому же потоку (RunFromWindowThread).
            const ULONGLONG left = deadline - now;
            const DWORD slice = left > 50 ? 50u : static_cast<DWORD>(left);
            const DWORD wr = MsgWaitForMultipleObjectsEx(
                count, pending, slice, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
            if (wr == WAIT_OBJECT_0 + count)
            {
                // PM_NOREMOVE: обрабатываются только межпоточные SendMessage,
                // posted-сообщения очереди не трогаем.
                MSG msg{};
                (void)PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
            }
            else if (wr == WAIT_FAILED)
            {
                // Хэндл мог закрыться между снимком и ожиданием — просто повторяем цикл.
                Sleep(10);
            }
        }
    }
}
