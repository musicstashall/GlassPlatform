# Логирование

**Связанный код:** `GlassShared/include/DebugLog.h`, `LogViewer/`

## Логирование

Определено в `GlassShared/include/DebugLog.h`. В Release-сборке все вызовы — пустые inline-функции (zero-cost). Активируется в Debug через `#define GLASS_ENABLE_LOGGING`.

### Уровни сообщений

| Константа | Значение | Когда использовать |
|-----------------|----------|---------------------------------------------------------|
| `DBG_NORMAL` | 0 | Обычный информационный поток (вызовы, состояния) |
| `DBG_IMPORTANT` | 1 | Важные события: инициализация, shutdown, attach/detach |
| `DBG_WARNING` | 2 | Нештатные ситуации, не являющиеся ошибкой |
| `DBG_ERROR` | 3 | Ошибки, после которых работа продолжается |
| `DBG_CRITICAL` | 4 | Критические сбои, дальнейшая работа невозможна |

### Сигнатуры

```cpp
// С явным уровнем:
DebugLog(DBG_WARNING, L"[WndCompat][ComboLBox] неожиданный индекс: %d", index);

// Без уровня — подставляется DBG_NORMAL:
DebugLog(L"[GlassCore][IATRegistry] Register: slot=0x%p", slot);
```

### Маршрутизация

- **LogViewer.exe** (`GLASS_LOG_SERVER`): `GlassLogServer` принимает подключения → отображает в UI
- **Инжектированные модули**: `GlassLogClient` (фоновый поток, очередь) → именованный канал `\\.\pipe\GlassPlatform.Log` → LogViewer
- **После выгрузки модулей** — только `OutputDebugStringW` (сервер может быть уже остановлен):

```cpp
OutputDebugStringW(L"[WndCompat] диагностика после shutdown\n");
```

Формат тега: `[Модуль][Подсистема]` — например `[GlassCore][ComboLBox]`, `[WndCompat][Root]`.

### Остановка (`GlassLogClient::Stop`)

`Stop()` ждёт воркер **с бюджетом** (~500 мс, `CancelSynchronousIo` по хендлу
воркера между опросами), а не безграничным `join()`: воркер может сидеть в
`WriteFile`/`CreateFile` на канал и флаг `m_running` не читает. Возвращает:

- `true` — воркер завершился (обычно после `join()`), модуль можно выгружать;
- `false` — воркер ещё жив: `Stop()` сделал `detach()`, выгрузку нужно отложить.

Пока воркер жив, его `WorkerThreadProc` исполняется в образе модуля —
`FreeLibrary` снимет картину под ним (дамп `ShellExperienceHost.exe.55548.dmp`,
17.11.2026: c0000005 в `WndCompat!std::thread::join`). Поэтому:

- модули экспортируют `Xxx_IsLogWorkerAlive` (обёртка над `IsWorkerAlive()`);
- `GlassCompat::ExecuteShutdown` не делает `FreeLibrary`, пока экспорт
  отвечает `TRUE` (тот же приём, что для stale-колбэков H91);
- `FreeSelfAndExitThread` GlassCompat аналогично не выгружает себя;
- слепой `ForceUnloadGlassModules` (unlock/uninstall) откладывается, пока
  фаза shutdown в `InjectProcessMap` не `None` и не `CompatDone`.

Деструктор логгера и CRT-teardown по-прежнему делают только `detach()`
(join в `DllMain`/CRT — deadlock/abort). В Release `StopImpl` пустой:
воркера лога там нет.

### Цвета уровней в LogViewer

Константы уровней совпадают с `DBG_NORMAL` … `DBG_CRITICAL` в таблице выше. Цвет текста строки в списке логов и заливка квадратиков фильтра **N / I / W / E / C** на среднем тулбаре задаются одними и теми же `RGB` — единый источник: `LogViewer\LogViewerLevelColors.h` (отрисовка списка: `UiMainWindow.cpp`).
