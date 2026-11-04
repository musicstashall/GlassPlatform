#pragma once
#include <Windows.h>

// Фоновые потоки UICompat, переживающие FreeLibrary.
//
// Дамп ShellExperienceHost 26100 (01.11.2026, uptime 17 с): retry-поток IXDE
// (ScheduleSehTallIxdeRetry) был создан через CreateThread + CloseHandle, спал в
// Sleep(3000) и проснулся уже после FreeLibrary(UICompat) из GlassCompat —
// c0000005 (execute) по RIP UICompat+0x3068 в выгруженном образе.
//
// Правило для UICompat: любой поток, чей код лежит в этой DLL, запускается только
// через ModuleThread::Start:
//   * до CreateThread берётся ссылка на модуль (GetModuleHandleEx FROM_ADDRESS),
//     поэтому FreeLibrary снаружи лишь уменьшает счётчик, образ остаётся;
//   * поток завершается через FreeLibraryAndExitThread и не возвращается в код
//     модуля после снятия своей ссылки;
//   * хэндл хранится до выхода потока: Stop ждёт потоки (JoinAll), а
//     UICompat_DebugComState сообщает GlassCompat о живых потоках, чтобы тот не
//     снимал «остаточную» ссылку (TryDropIxdeResidualModule), пока они не вышли.
namespace UICompat::ModuleThread
{
    using Proc = void (*)(void* param) noexcept;

    // Запуск потока. false — поток не создан (ресурс не захвачен, param не тронут).
    // tag — только для лога.
    [[nodiscard]] bool Start(Proc proc, void* param, const wchar_t* tag) noexcept;

    // Число потоков, запущенных через Start и ещё не завершившихся
    // (хэндл не сигнален — поток учитывается до полного выхода, включая
    // FreeLibraryAndExitThread).
    [[nodiscard]] long ActiveCount() noexcept;

    // Ожидание выхода всех потоков. Прокачиваются только sent-сообщения
    // (поток может сидеть в SendMessageTimeout к этому UI-потоку); posted не
    // трогаются. true — потоков не осталось; false — таймаут (в лог).
    bool JoinAll(DWORD timeoutMs, const wchar_t* phase) noexcept;
}
