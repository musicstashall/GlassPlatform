# GlassCompat

**Связанный код:** `GlassCompat/`

### GlassCompat — двойной режим работы

**Хост-режим** (в Rundll32.exe):
- `EntryRundll.cpp::Main()` — создаёт `InstanceMutex`, `StopEvent`, `GlassHostPid`
- Устанавливает глобальный `SetWinEventHook(WINEVENT_INCONTEXT)`
- Запускает `GlassLogServer` (сервер логов)
- Ожидает `/stop` или закрытия через `AttachConsole`

**Инжектированный режим** (в GUI-процессах):
- `DllMain` → `GlassCompat_WinEventProc` регистрируется как WinEvent-коллбэк ОС
- `DoInitialize()` → `GlassGuiThreadProc` в отдельном потоке
- Заблокированные процессы (из `ProcessBlockList`) получают "unload watcher" поток: ждёт StopEvent → `FreeLibraryAndExitThread`
- Незаблокированные процессы: `GlassGuiThread` загружает GlassCore + WndCompat, ждёт StopEvent, выгружает

### GlassCompat — на что обращать внимание

- `s_shouldSkip` — определяется один раз через `std::call_once`; `true` для процессов из `ProcessBlockList` или для хост-процесса
- Заблокированные процессы получают "unload watcher" поток с `GetModuleHandleExW` + `FreeLibraryAndExitThread`
- **Контракт выгрузки (не ломать):** [docs/GlassCompat-unload.md](docs/GlassCompat-unload.md) — path=1 без HostShutdown; path=2/3 ждут HostShutdown (H58)

## Документация

| Задача | Документ |
|--------|----------|
| Выгрузка (path=1 vs BlockList) | [docs/GlassCompat-unload.md](docs/GlassCompat-unload.md) |
| Архитектура платформы | [ARCHITECTURE.md](../ARCHITECTURE.md) |
| PlatformPresenceService | [GlassSettings/docs/PlatformPresenceService-architecture.md](../GlassSettings/docs/PlatformPresenceService-architecture.md) |
- `ReadHostPid()` / `WriteHostPid()` — работа с `Local\GlassHostPid`; `s_hostPidMapping` хранит handle всё время жизни хоста
- `GlassLogClient` — в Debug-сборках запускается в каждом инжектированном процессе; `Stop()` вызывается перед `FreeLibraryAndExitThread`; `EnsureConnected` проверяет `m_running` в retry-цикле
