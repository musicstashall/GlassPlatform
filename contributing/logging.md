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

### Цвета уровней в LogViewer

Константы уровней совпадают с `DBG_NORMAL` … `DBG_CRITICAL` в таблице выше. Цвет текста строки в списке логов и заливка квадратиков фильтра **N / I / W / E / C** на среднем тулбаре задаются одними и теми же `RGB` — единый источник: `LogViewer\LogViewerLevelColors.h` (отрисовка списка: `UiMainWindow.cpp`).
