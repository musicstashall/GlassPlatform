#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "LogPacket.h"
#include "Ui/Model/LogViewerLogModel.h"

void StartLogClientThread(HWND hwndTarget);

/// Остановить вспомогательные потоки (разрешение имён PID); вызывать из GUI перед уничтожением окна.
void StopLogClientSupportThreads() noexcept;

// Лимиты обработки за один тик таймера дренажа.
inline constexpr std::size_t kMaxLogEntriesPerUiTick = 400;
// Бюджет времени на один тик (мс); после исчерпания — остаток батча вернётся в очередь.
inline constexpr double kMaxLogUiSliceMs = 16.0;
// Потолок очереди LogEntry между пайп-потоком и GUI; старые удаляются, счётчик — ConsumeLogQueueDroppedCount.
inline constexpr std::size_t kMaxPendingLogEntries = 50'000;

// Забирает до maxEntries записей с головы очереди (FIFO). Остальное остаётся.
// Вызывать только из GUI-потока.
[[nodiscard]] std::vector<LogEntry> DrainLogEntryQueueUpTo(std::size_t maxEntries);

// Вернуть необработанные записи в начало очереди (порядок сохраняется).
// Вызывать только из GUI-потока.
void RequeueLogEntriesFront(std::vector<LogEntry>&& entries);

// Сколько записей было вытеснено из-за переполнения очереди (и сбросить счётчик).
[[nodiscard]] std::uint64_t ConsumeLogQueueDroppedCount() noexcept;

// Есть ли ещё записи в очереди приёма.
[[nodiscard]] bool HasPendingLogEntries() noexcept;

// Поставить в очередь разрешение имени процесса (без OpenProcess на GUI-потоке).
void EnqueuePidForLabelResolution(uint32_t processId) noexcept;

// Забрать результаты разрешения PID для слияния в m_pidLabels (вызывать из WM_APP_PID_LABELS_FLUSH).
void TakePendingPidLabelResults(std::vector<std::pair<uint32_t, std::wstring>>& out);

#define WM_LOG_PACKET    (WM_USER + 1)  // устарело: дренаж через IDT_LOG_DRAIN; оставлено для совместимости
#define WM_LOG_CONNECTED (WM_USER + 2)  // wParam = 1 (подключён) / 0 (разрыв)
