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
- **Гейт загрузки (R7).** `DllMain` решает, грузиться ли модулю в процесс: встроенный deny-список (`ProcessBlockList` + приложения, падавшие в чужом стеке), затем `GateIsAlwaysTrusted` (хост, `explorer.exe`, shell-хосты), затем файл `%LOCALAPPDATA%\GlassPlatform\attach-gate.txt` с fail-closed. Отказ кэшируется на процесс мьютексом `GlassCompatGateDeny_<pid>`, поэтому повторные `LoadLibrary` от user32 не пересчитывают решение; слот «отвергнут» пишется один раз на процесс (`GlassCompatGateSlot_<pid>`), а `dll_attach`/`dll_detach` в лог не попадают (R12i)
- **Отвергнутые процессы.** Отказ гейта пишет слот карты инжекта с битом `ModuleCompat` (`deny_slot`), поэтому исключённые процессы видны в GlassSettings как «отвергнутые»

## Документация

| Задача | Документ |
|--------|----------|
| Выгрузка (path=1 vs BlockList) | [docs/GlassCompat-unload.md](docs/GlassCompat-unload.md) |
| Диагностика выгрузки (точки инструментации, события NDJSON) | [docs/unload-instrumentation.md](docs/unload-instrumentation.md) |
| Архитектура платформы | [ARCHITECTURE.md](../ARCHITECTURE.md) |
| PlatformPresenceService | [GlassSettings/docs/PlatformPresenceService-architecture.md](../GlassSettings/docs/PlatformPresenceService-architecture.md) |
- `ReadHostPid()` / `WriteHostPid()` — работа с `Local\GlassHostPid`; `s_hostPidMapping` хранит handle всё время жизни хоста
- `GlassLogClient` — в Debug-сборках запускается в каждом инжектированном процессе; `Stop()` вызывается перед `FreeLibraryAndExitThread`; `EnsureConnected` проверяет `m_running` в retry-цикле
