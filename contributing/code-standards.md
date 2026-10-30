# Стандарты кода

## Стандарты кода

**Язык:** C++20  
**Комментарии:** русский язык  
**Кодировка файлов в репозитории:** UTF-8 без BOM; после создания нового файла — сразу проверить (см. [file-encoding.md](file-encoding.md)).

### Обязательные правила

- **Запрет исключений** — весь код `noexcept` там, где это применимо; никаких `throw`; SEH только в строго обозначенных местах
- **RAII и WIL везде** — ресурсы освобождаются через RAII-обёртки (`wil::unique_handle`, `wil::scope_exit` и т.д.), не через явные `CloseHandle`/`free` в коде
- **Без naked new/delete** — использовать `std::make_unique`, `std::make_shared` или WIL-обёртки
- **`[[nodiscard]]`** — обязателен для функций, возвращающих HRESULT, BOOL, handle, указатель на ресурс

### Стиль именования (как в существующем коде)

- Глобальные переменные: `g_` префикс (`g_insidePaint`, `g_comctl32Module`)
- Thread-local: `thread_local` + `g_` или `s_` (`s_lastHotItem`)
- Статические локальные: `s_` префикс
- Функции-хуки: `Overall_My*`, `My*`
- Индексы хуков: `OHK_*` enum

### Паттерны, которые нужно соблюдать

```cpp
// Shutdown guard — первая строка каждого SubclassProc и Begin-коллбэка
if (WndCompat::IsShuttingDown())
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);

// Atomic shutdown guard — в Begin-коллбэках GlassCore
if (g_shutdown.load(std::memory_order_acquire)) return;

// Scope exit для ресурсов
auto releaseDC = wil::scope_exit([&] { ReleaseDC(hWnd, hdc); });

// Кросс-поточное снятие субкласса (из DetachAll):
DWORD_PTR result = 0;
SendMessageTimeoutW(hWnd, WM_GLASS_DETACH_*, 0, 0, SMTO_ABORTIFHUNG, 1000, &result);

// Безопасная выгрузка DLL (в GlassGuiThreadProc и unload watcher):
FreeLibraryAndExitThread(hModuleRef, 0);

// thread_local для per-paint состояния (GUI-поток)
// std::atomic для cross-thread состояния (анимации, scroll)
```

### Отрисовка и альфа: обычный GDI, без GDI+

Платформа **не использует GDI+** (`Gdiplus`) для корректной непрозрачности поверх «стекла». Зависимость от GDI+ в общих утилитах снята (см. комментарий в `GlassShared/include/BitmapUtils.h`). В **BWM** часто встречаются `GdipCreateBitmapAlpha` и другие вызовы GDI+ — при переносе в Glass их **не** воспроизводить; нужна эквивалентная схема на **GDI + DIB**.

**Опорные реализации в репозитории:**

- **`Shared::WinApiUtils::CreateSolidColorBrushWithAlpha`** — [`GlassShared/include/Helpers/WinApiUtils.h`](GlassShared/include/Helpers/WinApiUtils.h), [`GlassShared/src/WinApiUtils.cpp`](GlassShared/src/WinApiUtils.cpp): однопиксельный 32bpp DIB (premultiplied BGRA) и `CreatePatternBrush` — кисть с заданной альфой для заливок на прозрачном фоне.
- **`Shared::BitmapUtils`** — [`GlassShared/include/BitmapUtils.h`](GlassShared/include/BitmapUtils.h), [`GlassShared/src/BitmapUtils.cpp`](GlassShared/src/BitmapUtils.cpp): `Promise32BPP` (конвертация в 32bpp DIBSECTION с сохранением альфы), `HasAlphaChannel`, `SpriteEffect` / правка пикселей на месте для масок и «вырезания» фона.
- **Вывод с учётом альфа-канала битмапа** — типично `GdiAlphaBlend` (`msimg32.lib`, `BLENDFUNCTION`), см. поясняющие комментарии в [`GlassCore/src/Hooks/ListViewHooks.cpp`](GlassCore/src/Hooks/ListViewHooks.cpp) (там же зафиксировано отличие от BWM: после конвертации нельзя полагаться на обычный `BitBlt` для премультиплированной альфы).

При добавлении новых хуков ищи **существующий** паттерн в ближайшем `*Hooks.cpp` или в GlassShared, а не новый стек на GDI+.

## Чего не делать

## Чего НЕ делать

- Не подключать **GDI+** (`Gdiplus`) и не переносить из BWM GDI+-зависимую отрисовку (`GdipCreateBitmapAlpha` и аналоги) — использовать **GDI**, `BitmapUtils` и `WinApiUtils::CreateSolidColorBrushWithAlpha` (см. раздел «Отрисовка и альфа» выше)
- Не писать в IAT-слот напрямую, минуя IATRegistry
- Не вызывать `RemoveWindowSubclass` с чужого потока — использовать `WM_GLASS_DETACH_*` + `SendMessageTimeoutW`
- Не обращаться к провайдерам GlassCore после `IsShuttingDown() == true`
- Не создавать новые 2D-диспетчеры (g_comboDispatcher и т.п.) — только через g_overallMap
- Не использовать `GetParent()` для определения стиля ComboLBox (у него `WS_POPUP`, нет родителя) — проверять стили на самом окне
- Не использовать `PostMessageW` для доставки сообщений, требующих обратного подтверждения до уничтожения объекта — только `SendMessageTimeoutW`
- Не закрывать `hMap` в `WriteHostPid` — handle хранить в `s_hostPidMapping` до `ClearHostPid`
- **Не объявлять `WM_APP`-сообщения локально** в отдельных файлах — все кастомные оконные сообщения (на базе `WM_APP`) регистрируются исключительно в `GlassShared\include\GlassMessages.h` и нигде больше
- **Не нарушать изоляцию списков провайдеров PaintDispatcher** — каждый хук обращается строго к своему списку (`HookEndPaint` → `g_paintProviders`, `HookReleaseDC` → `g_getDCProviders`, `HookDeleteDC` → `g_compatDCProviders`; post-BeginPaint — только `g_postPaintProviders` из `HookBeginPaint`). Button/Static регистрировать **только** через `RegisterPaintProvider`: смешение списков приводит к тому, что внутренние `DeleteDC`/`ReleaseDC` от uxtheme отключают IAT-хуки в середине animation-фрейма
- **Не смешивать хуки контролов между файлами** — у ListView / TreeView / Toolbar / … свои `*Hooks.cpp` и свои `HookDispatcher`; не вызывать API одного контрольного модуля из другого, не экспортировать «мосты» между модулями для обхода порядка IAT, не проверять чужие `thread_local` стеки сессий. Один IAT у стороннего модуля (например `sysmon.ocx`) на несколько контролов — **отдельные** тела хуков + стек **IATRegistry** (occupant — первый активный, вложенные сессии не флипают; см. «BWM и Glass», п.4)
- **Не линковать peer-модули друг с другом** — единственная допустимая статическая линковка между проектами solution для peer/host — **GlassShared**; межмодульные вызовы только runtime (`GetProcAddress` / `LoadOptionalModule`). Запрещены `ProjectReference` peer→peer с `LinkLibraryDependencies=true` и import lib соседних DLL. Подробности: [ARCHITECTURE.md](../ARCHITECTURE.md) («Зависимости модулей»)
