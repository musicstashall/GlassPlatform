#pragma once

#include <windows.h>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <chrono>
#include <vector>
#include <cstdint>
#include <cstring>

#include "GlassLogProtocol.h"

namespace Shared::Log
{
    namespace
    {
        // LogViewer: не ставить строки в именованный канал — иначе рекурсия (Core логирует в тот же процесс).
        [[nodiscard]] inline bool IsCurrentExeLogViewerProcess() noexcept
        {
            static std::atomic<int> s_st{ -1 };
            const int v = s_st.load(std::memory_order_relaxed);
            if (v >= 0)
                return v != 0;
            wchar_t path[MAX_PATH]{};
            const DWORD n = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
            bool yes = false;
            if (n > 0)
            {
                const wchar_t* base = path;
                for (wchar_t* p = path; *p; ++p)
                    if (*p == L'/' || *p == L'\\')
                        base = p + 1;
                yes = (_wcsicmp(base, L"logviewer.exe") == 0);
            }
            s_st.store(yes ? 1 : 0, std::memory_order_relaxed);
            return yes;
        }
    }

    class GlassLogClient
    {
    public:
        static GlassLogClient& Instance()
        {
            static GlassLogClient g;
            return g;
        }

        // Возвращает true, если Stop() уже был вызван (необратимо)
        static bool IsStopped()
        {
            return Instance().m_stopped.load(std::memory_order_acquire);
        }

        void LogLine(const std::wstring& text, int level)
        {
#if !defined(_DEBUG) || !defined(GLASS_ENABLE_LOGGING)
            (void)text;
            (void)level;
            return;
#else
            if (text.empty())
                return;

            // Вывод в отладчик уже делает DebugLog.h (DebugLogImpl до SendLog и wstring-
            // перегрузка после). Повторный ODS здесь давал каждую строку дважды в Output.
            if (IsCurrentExeLogViewerProcess())
                return;

            // После необратимой остановки — только OutputDebugStringW, без pipe-потока
            if (m_stopped.load(std::memory_order_acquire))
                return;

            EnsureWorkerStarted();

            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_queue.push_back({ text, level, ::GetCurrentThreadId() });
            }
            m_queueCv.notify_one();
#endif
        }

        static void Write(const std::wstring& text, int level = 0)
        {
            Instance().LogLine(text, level);
        }

        // Явная остановка с GUI / watcher-потока: дождаться воркера с бюджетом
        // (~500 мс + отмена I/O), а не безграничным join().
        // true — воркер завершился, модуль можно выгружать;
        // false — воркер ещё жив: выгрузку отложить (см. IsWorkerAlive()).
        bool Stop()
        {
            return StopImpl(/*joinWorker*/ true);
        }

    private:
        GlassLogClient()
            : m_running(false)
            , m_pipe(INVALID_HANDLE_VALUE)
        {
        }

        ~GlassLogClient()
        {
            // join() из деструктора / CRT teardown / DllMain DETACH даёт deadlock
            // или std::terminate → abort()-диалог Debug CRT при reboot.
            // m_workerExited НЕ закрываем: отсоединённый воркер может
            // сигналировать позже — handle живёт до конца процесса.
            StopImpl(/*joinWorker*/ false);
        }

        // Остановка воркера.
        // joinWorker=true (Shutdown() на GUI/watcher-потоке): раньше здесь был
        // безграничный join() — воркер, сидящий в WriteFile/CreateFile на канал
        // LogViewer, мешал вернуться из Shutdown → GlassCompat не доходил до
        // FreeLibrary, а слепой FreeLibrary снаружи снимал образ под уже
        // выполняющимся join (дамп ShellExperienceHost.exe 17.11.2026:
        // c0000005 в WndCompat!std::thread::join+0x81).
        // Поэтому: бюджет + CancelSynchronousIo, join только если воркер вышел;
        // иначе detach и false — вызывающий ОБЯЗАН оставить свой модуль
        // загруженным (гейт Xxx_IsLogWorkerAlive перед FreeLibrary).
        // joinWorker=false (деструктор/CRT detach): без ожидания, loader lock.
        bool StopImpl(bool joinWorker)
        {
#if defined(_DEBUG) && defined(GLASS_ENABLE_LOGGING)
            m_stopped.store(true, std::memory_order_release);

            bool expected = true;
            if (m_running.compare_exchange_strong(expected, false))
                m_queueCv.notify_all();

            if (!joinWorker)
            {
                if (m_worker.joinable())
                    m_worker.detach();
                return !IsWorkerAlive();
            }

            // CancelSynchronousIo рвёт ожидающий WriteFile/CreateFile воркера;
            // повтор цикла нужен, т.к. отмена до входа потока в syscall —
            // no-op (ERROR_NOT_FOUND), и воркер может зайти в I/O уже после
            // отмены. Между итерациями — опрос события выхода (50 мс).
            constexpr ULONGLONG kStopBudgetMs = 500;
            const ULONGLONG t0 = ::GetTickCount64();
            while (IsWorkerAlive())
            {
                if (::GetTickCount64() - t0 >= kStopBudgetMs)
                    break;
                if (m_worker.joinable())
                {
                    // MSVC: native_handle_type = void* (HANDLE). reinterpret_cast
                    // работает и для целочесного native_handle других STL.
                    (void)::CancelSynchronousIo(
                        reinterpret_cast<HANDLE>(m_worker.native_handle()));
                }
                (void)::WaitForSingleObject(m_workerExited, 50);
            }

            if (!IsWorkerAlive())
            {
                // join() после события добирает фактический выход потока
                // (эпилог/CRT) — без него остаётся окно, когда событие уже
                // сигнальное, а код модуля ещё исполняется.
                if (m_worker.joinable())
                    m_worker.join();
                ClosePipe();
                return true;
            }

            // Бюджет исчерпан — detach и «ещё жив». Пайп НЕ закрываем: его
            // владелец — воркер (CloseHandle под ожидающим I/O не отменяет его
            // и гоняет handle-значение); воркер закроет сам на выходе.
            if (m_worker.joinable())
                m_worker.detach();
            return false;
#else
            (void)joinWorker;
            return true;
#endif
        }

    public:
        // true — воркер лога ещё исполняет код своего модуля. Такой модуль
        // выгружать нельзя: WorkerThreadProc находится в его образе.
        // false — воркера не было, он завершился, или сборка без логгера.
        // (Отслеживается по событию выхода воркера, а не по joinable():
        // после detach joinable() уже false, а поток может быть жив.)
        bool IsWorkerAlive() const noexcept
        {
#if defined(_DEBUG) && defined(GLASS_ENABLE_LOGGING)
            if (!m_workerExited)
                return false; // воркер не запускался (или не смог стартовать)
            return ::WaitForSingleObject(m_workerExited, 0) != WAIT_OBJECT_0;
#else
            return false;
#endif
        }

    private:

        struct PendingItem
        {
            std::wstring text;
            int          level;
            DWORD        sourceThreadId; // поток, вызвавший DebugLog (не воркер очереди)
        };

        void EnsureWorkerStarted()
        {
            // Проверяем необратимый флаг остановки первым — до m_running
            if (m_stopped.load(std::memory_order_acquire))
                return;

            bool expected = false;
            if (!m_running.compare_exchange_strong(expected, true))
                return;

            // Событие выхода воркера (создаётся один раз, unsignaled = жив):
            // SetEvent в конце WorkerThreadProc; по нему StopImpl() ждёт без
            // join и модули спрашивают IsWorkerAlive() перед FreeLibrary.
            // Без события воркер не поднимаем — его выход было бы нечем
            // отследить.
            if (!m_workerExited)
            {
                m_workerExited = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (!m_workerExited)
                {
                    m_running.store(false, std::memory_order_release);
                    return;
                }
            }

            try
            {
                m_worker = std::thread(&GlassLogClient::WorkerThreadProc, this);
            }
            catch (...)
            {
                // Иначе m_running останется true навсегда, а воркера нет —
                // StopImpl() ждал бы бюджет впустую, IsWorkerAlive соврал бы.
                ::SetEvent(m_workerExited);
                m_running.store(false, std::memory_order_release);
            }
        }

        void WorkerThreadProc()
        {
            for (;;)
            {
                PendingItem item;

                {
                    std::unique_lock<std::mutex> lock(m_queueMutex);
                    m_queueCv.wait(lock, [this]() {
                        return !m_running.load(std::memory_order_acquire) || !m_queue.empty();
                        });

                    if (!m_running.load(std::memory_order_acquire) && m_queue.empty())
                        break;

                    if (m_queue.empty())
                        continue;

                    item = std::move(m_queue.front());
                    m_queue.pop_front();
                }

                if (!EnsureConnected())
                {
                    // При Stop() EnsureConnected сразу возвращает false. Если снова положить
                    // элемент в очередь, условие выхода (!m_running && empty) никогда не выполнится —
                    // join() в Stop() зависнет (без LogViewer канал не открывается).
                    if (m_running.load(std::memory_order_acquire))
                    {
                        {
                            std::lock_guard<std::mutex> lock(m_queueMutex);
                            m_queue.push_front(std::move(item));
                        }
                        m_queueCv.notify_one();
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    continue;
                }

                std::wstring payload = item.text;
                if (payload.empty() || payload.back() != L'\0')
                    payload.push_back(L'\0');

                MessageHeader header{};
                header.size = static_cast<std::uint32_t>(
                    sizeof(MessageHeader) + payload.size() * sizeof(wchar_t)
                    );
                header.type = static_cast<std::uint16_t>(MessageType::LogLine);
                header.flags = static_cast<std::uint16_t>(
                    LogFlag_WideChar | EncodeLevel(item.level)
                    );
                header.threadId = item.sourceThreadId;

                const size_t totalSize = sizeof(MessageHeader) + payload.size() * sizeof(wchar_t);

                std::vector<std::uint8_t> buffer(totalSize);
                std::memcpy(buffer.data(), &header, sizeof(header));
                std::memcpy(buffer.data() + sizeof(header),
                    payload.data(),
                    payload.size() * sizeof(wchar_t));

                DWORD written = 0;
                BOOL ok = ::WriteFile(
                    m_pipe,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &written,
                    nullptr
                );

                if (!ok || written != buffer.size())
                {
                    ClosePipe();
                    if (m_running.load(std::memory_order_acquire))
                    {
                        {
                            std::lock_guard<std::mutex> lock(m_queueMutex);
                            m_queue.push_front(std::move(item));
                        }
                        m_queueCv.notify_one();
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                }
            }

            ClosePipe();

            // Сигнал «воркер вышел» — последним шагом (после ClosePipe).
            // По нему StopImpl() завершает бюджет ожидания, а модули
            // решают перед FreeLibrary (см. IsWorkerAlive).
            if (m_workerExited)
                ::SetEvent(m_workerExited);
        }

        bool EnsureConnected()
        {
            if (m_pipe != INVALID_HANDLE_VALUE)
                return true;

            constexpr int   kMaxAttempts = 50;
            constexpr DWORD kDelayMs = 100;
            constexpr DWORD kSeriesPauseMs = 500;

            for (;;)
            {
                if (!m_running.load(std::memory_order_acquire))
                    return false;

                bool connected = false;

                for (int i = 0; i < kMaxAttempts; ++i)
                {
                    // Проверяем флаг остановки до попытки подключения,
                    // чтобы не блокировать выгрузку DLL на ~5 секунд.
                    if (!m_running.load(std::memory_order_acquire))
                        return false;

                    // Ограниченное ожидание инстанса канала: без сервера
                    // WaitNamedPipe возвращает ERROR_FILE_NOT_FOUND сразу
                    // (дальше обычный Sleep), занятые инстансы ждём не дольше
                    // kDelayMs. m_running проверяем и после — WaitNamedPipe
                    // мог ждать.
                    (void)::WaitNamedPipeW(kLogPipeName, kDelayMs);

                    if (!m_running.load(std::memory_order_acquire))
                        return false;

                    HANDLE h = ::CreateFileW(
                        kLogPipeName,
                        GENERIC_WRITE,
                        0,
                        nullptr,
                        OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL,
                        nullptr
                    );

                    if (h != INVALID_HANDLE_VALUE)
                    {
                        m_pipe = h;
                        connected = true;
                        break;
                    }

                    DWORD err = ::GetLastError();
                    if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND)
                        break;

                    ::Sleep(kDelayMs);
                }

                if (connected)
                    return true;

                ::Sleep(kSeriesPauseMs);

                if (!m_running.load(std::memory_order_acquire))
                    return false;
            }
        }

        void ClosePipe()
        {
            if (m_pipe != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(m_pipe);
                m_pipe = INVALID_HANDLE_VALUE;
            }
        }

    private:
        std::atomic<bool>        m_stopped{false}; // необратимый флаг: true после Stop()
        std::atomic<bool>        m_running;
        std::thread              m_worker;

        // Manual-reset, создаётся при старте воркера (unsignaled = жив),
        // сигналится в конце WorkerThreadProc. Не закрывается в деструкторе:
        // отсоединённый воркер может сигналировать позже — handle живёт до
        // конца процесса.
        HANDLE                   m_workerExited{ nullptr };

        HANDLE                   m_pipe;

        std::mutex               m_queueMutex;
        std::condition_variable  m_queueCv;
        std::deque<PendingItem>  m_queue;
    };

    inline void SendLog(const std::wstring& text, int level)
    {
        if (GlassLogClient::IsStopped())
            return;
        GlassLogClient::Instance().LogLine(text, level);
    }
}
