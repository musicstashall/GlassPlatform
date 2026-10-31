# План инструментации выгрузки модулей (refcount-audit)

**Тип документа:** карта точек инструментации для Cursor AI (локально на ПК).
Способ инструментации — на усмотрение Cursor (его встроенный механизм). Здесь только:
**где** снимать данные и **что** в этих точках должно быть видно.

Ничего в контракте выгрузки не меняет: [GlassCompat-unload.md](GlassCompat-unload.md) остаётся
источником истины по `path=1` / `path=2/3` / BlockList. Инструментация — только наблюдение:
порядок фаз, таймауты и семантика путей не меняются.

**Порядок чтения — только в одну сторону:** сначала контракт (`GlassCompat-unload.md`), затем этот план.
План не требует, чтобы его прочитали, чтобы понять, что читать: единственная внешняя зависимость —
контракт выгрузки, перечисленный ниже.

**Ветка работы:** `arena/627639b4-glassplatform` (опубликована в origin).
**Актуальность привязок `file:line`:** коммит `8a5a491`. Номера строк плывут — ищите по имени функции.

---

## 0. Промпт для Cursor (копировать целиком)

Промпт самодостаточен: он не просит читать документ, внутри которого написан. Единственная внешняя
зависимость — контракт выгрузки; перечень точек — таблица R1 (раздел 5) ниже.

> Контракт выгрузки: `GlassCompat/docs/GlassCompat-unload.md` — прочитай его, менять нельзя
> (`path=1` vs BlockList, порядок фаз, таймауты).
> Задача — только наблюдение: расставь инструментацию своим штатным механизмом в точках этапа **R1**
> (таблица в разделе 5 этой задачи, ниже). В каждой точке пиши ровно то, что в колонке
> «Что должно быть в записи».
> Логику не менять: порядок фаз, таймауты, набор `UnhookWinEvent` и семантику `path=1` vs BlockList.
> Вставки помечай комментарием `GLASS-ULTRACE`, чтобы потом снять одним проходом.
> Сборка x64 Debug из корня solution. После сборки — отчёт: список точек (файл:функция), формат записей,
> куда пишутся логи.

---

## 1. Цель и наблюдаемая проблема

**Цель:** быстрая и стабильная выгрузка **всех** модулей (`GlassCompat`, `WndCompat`, `GlassCore`,
`ExplorerCompat`, `TaskbarCompat`, `UICompat`) из всех процессов, без крэшей и без остатков.

**Наблюдение:** модули выгружаются нестабильно, `GlassCompat.dll` часто остаётся загруженной.
Подозрение: `GlassCompat` получает **один лишний refcount**, поэтому `FreeLibrary` /
`FreeLibraryAndExitThread` не доводят счётчик до 0. При этом Unload из GlassSettings
(страница «Процессы») снимает модуль через `FreeLibrary` (Unlocker-стиль) «без краша»,
а прочие зависшие процессы просто жёстко завершаются.

Данные должны ответить на три вопроса:

1. Сколько **фактически** ссылок на `GlassCompat.dll` (и peer-модули) в процессе перед `FreeLibrary`/`FreeLibraryAndExitThread`?
2. Какая часть остатка — **наши пины** (`GetModuleHandleEx`, компенсация remote-load), а какая — **ссылки ОС** от INCONTEXT-хуков, снимаемые асинхронно?
3. Кто и когда снимает/не снимает ссылку: наш код, хост при `UnhookWinEvent`, или remote `FreeLibrary` из GlassSettings.

> Два разных остатка нельзя путать:
> **(1)** наш пин — детерминированный, считается кодом; **(2)** пин ОС от `SetWinEventHook(..., WINEVENT_INCONTEXT)` —
> их столько, сколько хуков привязано к процессу, и снимаются они **deferred** (`UnhookWinEvent` → `FreeLibrary`
> на потоке-получателе). Второй механизм сам по себе даёт «то выгрузилось, то нет» без всякой утечки.

---

## 2. Карта этапов выгрузки (с точки зрения refcount)

### Этап 0 — загрузка и пины

| Механизм | Где | Что делает со счётчиком |
|---|---|---|
| Глобальный INCONTEXT `CREATE..SHOW` | `EntryRundll.cpp:819` | +1 в каждом процессе, куда ОС его доставит |
| INCONTEXT `EVENT_SYSTEM_FOREGROUND` | `EntryRundll.cpp:828` | +1 |
| INCONTEXT `EVENT_OBJECT_UNCLOAKED` | `EntryRundll.cpp:837` | +1 |
| Per-PID INCONTEXT: `CREATE..SHOW`, `UNCLOAKED`, `LOCATIONCHANGE` | `CoreWindowHostInject.cpp:974-1035` | до **+3** в конкретном CW-процессе |
| `TryRemoteLoadLibrary` (`CreateRemoteThread(LoadLibraryW)`) | `CoreWindowHostInject.cpp:546-601` | **+1**, владелец — наш код в цели |
| `TryRemoteBootstrap` → `GlassCompat_RemoteBootstrap` | `:604`, `GlassCompat.cpp:3432-3450` | пин не берёт (`UNCHANGED_REFCOUNT`), ставит флаг `g_remoteLoadLibraryPin` |
| GUI-поток `GetModuleHandleEx(FROM_ADDRESS)` | `GlassCompat.cpp:2133` | **+1** на поколение GUI |
| CW-unload-watcher `GetModuleHandleEx` | `GlassCompat.cpp:2909` | **+1** в CW-процессах |
| Minimal-watcher (`path=2/3`) `GetModuleHandleEx` | `GlassCompat.cpp:3135` | **+1** в BlockList/Forbidden |
| `TryLoadAndInitUICompat` → `GetModuleHandleExW(0, L"UICompat.dll")` | `GlassCompat.cpp:1647` | **+1** к UICompat (не к Compat) |

Вывод: число ссылок на `GlassCompat.dll` в конкретном процессе заранее неизвестно — оно зависит от
того, сколько INCONTEXT-хуков ОС привязала, плюс наши пины. Поэтому «2 → decrement → 0» — неверная модель.

### Этап 1 — триггеры остановки

| Триггер | Кто | Где |
|---|---|---|
| `StopEvent` | хост по `/stop` | `GlassCompat.cpp:2158`, `:2690` |
| `Local\GlassCompat.UnloadRequest.<pid>` (soft unload) | GlassSettings | приём — **только** Minimal-watcher, `GlassCompat.cpp:3200` |
| `GlassCompat_CwForceUnloadFlag` (WPM) + `Local\GlassCompat.CwForce.<pid>` | хост | `CoreWindowHostInject.cpp:331`, `:127`; poll — `GlassCompat.cpp:2295`, `:2935` |
| `WM_QUIT` в GUI-поток | хост / watcher | `GlassCompat.cpp:2233`, `:2966` |
| `HostShutdown` | хост после `UnhookWinEvent` | `EntryRundll.cpp:970` |
| `HostAlive` (abandoned mutex = хост мёртв) | инжект | `GlassCompat.cpp:2209`, `:3238` |
| `TerminateProcess` | Планировщик / пользователь | только `HostAlive` |

### Этап 2 — `ExecuteShutdown` (модули)

`GlassCompat.cpp:882` → UICompat → (explorer) Taskbar → (explorer) Explorer → GlassCore → WndCompat →
`FreeLibrary` Core → `FreeLibrary` Explorer/Taskbar/UICompat/Wnd (под SRWLOCK) → `ReportUnloadResult`.
Дожимы: `CoFreeUnusedLibraries` + `TryDropIxdeResidualModule` (`:229-260`) — **дополнительный** `FreeLibrary`
сверх парного к `LoadLibrary`.

### Этап 3 — `FreeSelfAndExitThread` (`GlassCompat.cpp:2088-2108`)

1. `TryClearPid` — карта инжекта чистится **до** выгрузки (UI врёт «процесса нет», модуль ещё жив).
2. `g_remoteLoadLibraryPin.exchange(false)` → компенсирующий `FreeLibrary(hSelfRef)`.
3. `DrainBeforeFreeSelf()` — 8–24 раунда wake + `SleepEx(50)`.
4. `FreeLibraryAndExitThread(hSelfRef)` — снятие нашего GUI-пина.

Модуль уходит ⇔ к шагу 4 все OS-ссылки уже сняты deferred-вызовами. Это и есть точка нестабильности.

### Этап 4 — `done:` хоста (`EntryRundll.cpp:942-980`)

`UnhookWinEvent` ×3 → `CoreWindowHostInject::Shutdown()` (`:1574`) → broadcast `WM_NULL` + `Sleep(300)` →
`SignalHostShutdownComplete()` → `ForceUnloadResiduals()` (`CoreWindowHostInject.cpp:1640`).
Порядок важен: клиентам разрешают FreeSelf **до** запуска добивки остатков.

### Этап 5 — выгрузка из GlassSettings

- `InjectRuntimeMonitor.RequestProcessUnload` (`InjectRuntimeMonitor.cs:214-243`): Residual / платформа off → Force, иначе soft.
- `GlassSettings_RequestProcessUnload` (`InjectProcessMapNative.cpp:624`): `SignalUnloadRequest`, при неудаче → remote `FreeLibrary`.
- `Shared::ForceUnloadGlassModules::ForceUnload` (`GlassShared/src/ForceUnloadGlassModules.cpp`): до **4 проходов**,
  порядок `WndCompat → GlassCore → GlassCompat`, ожидание потока **1500 мс**, без контроля остатка и состояния хуков.

---

## 3. Сводный леджер: кто и что должен снимать

| Модуль | Кто загружает (+1) | Кто обязан снять (−1) | Подозрительные пути |
|---|---|---|---|
| `GlassCompat` | ОС (INCONTEXT ×N), remote `LoadLibrary`, GUI-пин, watcher-пины | ОС после `UnhookWinEvent`; компенсация remote; FLAET; `FreeLibrary(hPin)` в watcher | H80, H82, H87, H89 |
| `GlassCore` | `DoInitialize` (`LoadOptionalModule`) | `ExecuteShutdown` (до 4 попыток) | HWiNFO-zombie (H60/H61) — скипается намеренно |
| `WndCompat` | `DoInitialize` | `ExecuteShutdown` | `FreeLibrary(savedWnd)` без проверки результата |
| `ExplorerCompat` | lazy `Ensure*` + IXDE `LoadLibrary` | `FreeLibrary` + `TryDropIxdeResidualModule` | H86 |
| `TaskbarCompat` | lazy `Ensure*`; **`GetModuleHandleW` без +1** (`GlassCompat.cpp:1404`) | `FreeLibrary(savedTaskbar)` | H86: ручка без +1, а `FreeLibrary` по ней — лишний −1 |
| `UICompat` | lazy `Ensure*`; remote inject; `GetModuleHandleExW(0, ...)` (+1) | `FreeLibrary(savedUICompat)` + `TryDrop` | H83 |

---

## 4. Гипотезы, которые должны закрыть данные

| # | Гипотеза | Что должно быть видно |
|---|---|---|
| **H80** | `g_remoteLoadLibraryPin` — **bool**, а не счётчик, и ставится в `GlassCompat_RemoteBootstrap` (`:3435`) **безусловно**, даже если модуль грузился не через `CreateRemoteThread(LoadLibraryW)`. Итог: либо лишний −1 в `FreeSelfAndExitThread` (`:2101-2103`) → преждевременный unmap, либо при >1 remote-load компенсация одна → **+1 остаток** | значение флага до/после bootstrap, факт и результат компенсирующего `FreeLibrary`, refcount вокруг него |
| **H81** | Пин ОС кратен числу INCONTEXT-хуков; часть хуков не снимается / снимается не в тот PID | по каждому pid: установлено vs снято, список handle |
| **H82** | Deferred `FreeLibrary` от ОС приходит позже нашего FLAET, а в idle/AppContainer/без pump процесса — не приходит вовсе | refcount перед FLAET > наших пинов; позднее событие выгрузки модуля |
| **H83** | `GetModuleHandleExW(0, L"UICompat.dll")` (`:1647`) даёт +1 к UICompat, загруженной remote-инжектом; в shutdown снимается один `FreeLibrary` | refcount UICompat до/после free |
| **H84** | `TryRemoteDropShellHostResidualPin` (`CoreWindowHostInject.cpp:795-875`) делает **ровно один** remote `FreeLibrary` — при остатке >1 не снимает | состояние модуля до/после, guiTid |
| **H85** | `ForceUnload` — слепые 4 прохода с таймаутом 1500 мс, результат `FreeLibrary` (BOOL) теряется, состояние хуков не сверяется | по проходам: модуль, wait, что осталось после |
| **H86** | `g_hTaskbar = GetModuleHandleW(...)` (`:1404`, `:1518`) — ручка без +1, но позже по ней `FreeLibrary` → лишний −1 по чужой ссылке | refcount до/после, признак «владения» ручкой |
| **H87** | Watcher-пины (`:2909`, `:3135`) освобождаются в разных ветках; `call_once` не перевзводится | по каждой ветке выхода watcher'а: был ли снят пин |
| **H88** | `SignalHostShutdownComplete` (`:970`) раньше `ForceUnloadResiduals` (`:979`) → гонка «наш FLAET vs remote FreeLibrary» | таймлайн трёх событий |
| **H89** | Новая генерация GUI (`StartGuiThreadIfNeeded`, `:3303`) берёт новый пин; если предыдущая не сделала FLAET — утечка +1 за цикл | номер поколения GUI и парность пин/снятие |
| **H90** | Soft unload (`UnloadRequest`) для path=1 процесса никем не слушается → GS считает «запрошено», реакции нет, дальше Force | факт запроса в процессе vs отсутствие триггера |

---

## 5. Точки инструментации

Три раунда: **R1** — сейчас (закрывает H80/H82/H85/H87/H88), **R2** — детализация модулей, **R3** — если R1/R2 не хватило.

### R1 — обязательные точки

| # | Файл / функция | Событие | Что должно быть в записи |
|---|---|---|---|
| 1 | `GlassCompat/DllMain.cpp` `DllMain` ATTACH / DETACH | `dll_attach` / `dll_detach` | base, pid, tid; для DETACH — сам факт выгрузки модуля (это эталон «модуль ушёл») |
| 2 | `GlassCompat.cpp:2133` `GlassGuiThreadProc` | `pin_acq` | номер поколения GUI, handle, ok/GLE, refcount модуля до/после |
| 3 | `GlassCompat.cpp:3432` `GlassCompat_RemoteBootstrap` | `bootstrap` | tid, `remote_flag` до/после, refcount до/после, код выхода |
| 4 | `GlassCompat.cpp:2101` `FreeSelfAndExitThread` (вход) | `freeself_begin` | путь, handle, `remote_flag`, refcount, число in-flight WinEvent-колбэков |
| 5 | там же, компенсация remote (`:2102-2103`) | `freeself_remote_comp` | вызывался ли, результат, refcount до/после |
| 6 | `GlassCompat.cpp:225` `DrainBeforeFreeSelf` | `drain_begin` / `drain_round` / `drain_end` | число раундов (факт/максимум), сколько потоков разбужено, in-flight, длительность, refcount |
| 7 | `GlassCompat.cpp:2108` перед `FLAET` | `freeself_flaet` | refcount, наши пины, handle |
| 8 | `GlassCompat.cpp:882` `ExecuteShutdown` | `shutdown_begin` / `shutdown_end` | причина запуска, длительность, итоговая маска результата, фаза |
| 9 | все точки выхода из GUI-цикла и вызовы `ExecuteShutdown` (`:2861`, `:3041`) | `stop_trigger` | что именно разбудило: stopEvent / hostLiveness / unloadRequest / cwForceFlag / wm_quit / hostAlive |
| 10 | `ExecuteShutdown` по модулям: UICompat `:914`, Taskbar `:948`, Explorer `:978`, Core `:996`, Wnd `:1035` | `module_shutdown` | модуль, результат вызова, флаг `IsShutdownComplete`, сколько ждали |
| 11 | каждый `FreeLibrary` peer-модуля: `:1098`, `:1186`, `:1225`, `:1277`, `:1360` | `module_free` | модуль, base, результат, refcount до/после, GetModuleHandle, Toolhelp, «свой/чужой» пин |
| 12 | `GlassCompat.cpp:229` `TryDropIxdeResidualModule` | `module_residual_drop` | модуль, COM-состояние (objectCount/locks/shutdownDone), результат, refcount |
| 13 | `GlassCompat.cpp:2909` `StartCwUnloadWatcher` + все `return`/`FLAET` в нём (`:2940-3083`) | `watcher_start` / `watcher_exit` | handle-пин, ветка выхода (метка), shutdownStarted/complete, guiTid, загружен ли модуль, снят ли пин |
| 14 | `GlassCompat.cpp:3135` `StartMinimalUnloadWatcher` | `watcher_start` / `watcher_wake` / `watcher_exit` | path=2/3, триггер (stop/unload/hostAlive/wait_failed), результат H58-ожидания, снят ли пин |
| 15 | `EntryRundll.cpp:819/828/837` | `host_hook_install` | вид хука (global/foreground/uncloak), handle, ok |
| 16 | `EntryRundll.cpp:947-961` | `host_hook_unhook` | вид, handle, результат, сколько всего снято |
| 17 | `EntryRundll.cpp:966-970` | `host_broadcast` / `host_shutdown_signal` | время (для склейки таймлайна с процессами) |
| 18 | `EntryRundll.cpp:979` → `CoreWindowHostInject.cpp:1640` `ForceUnloadResiduals` | `cw_residual_begin` / `cw_residual_pid` / `cw_residual_end` | список pid, exe, есть ли Compat/UICompat, длительность, сколько осталось |
| 19 | `CoreWindowHostInject.cpp:546` `TryRemoteLoadLibrary` | `cw_remote_load` | pid, ok, был ли уже загружен до/после |
| 20 | `CoreWindowHostInject.cpp:604` `TryRemoteBootstrap` | `cw_bootstrap` | pid, ok, код выхода потока (= guiTid) |
| 21 | `CoreWindowHostInject.cpp:974` `InstallProcessHook` | `cw_hook_install` | pid, три handle (create/uncloak/location), сколько хуков у pid теперь |
| 22 | `CoreWindowHostInject.cpp:1574` `Shutdown` | `cw_hook_unhook` | pid, сколько снято, всего хуков, длительность |
| 23 | `CoreWindowHostInject.cpp:795` `TryRemoteDropShellHostResidualPin` | `cw_residual_drop` | pid, стадия (wake / free), загружен ли до/после, guiTid |
| 24 | `CoreWindowHostInject.cpp:878` `TryRemoteRequestShutdown` | `cw_remote_shutdown` | pid, стадия, guiTid, загружен ли модуль |
| 25 | `GlassShared/src/ForceUnloadGlassModules.cpp` `ForceUnload` | `fu_begin` / `fu_try` / `fu_end` | pid, host/wow64, проход, модуль, base, результат и wait, что осталось, длительность, итог `left=` |
| 26 | `GlassSettings/Native/InjectProcessMapNative.cpp:624` | `gs_request` | pid, soft-сигнал прошёл или нет, результат force, какие модули остались |
| 27 | Верификация извне (хост и GS native) | `verify` | через 0 / 2 / 10 с после Stop: pid, exe, набор Glass*-модулей процесса |

**Дополнительно к R1 (важнее остального):** в точках 2, 4, 7, 11, 25 нужен **фактический refcount модуля**
(число ссылок загрузчика), а не только `GetModuleHandle`. Если штатный механизм не даёт это из коробки —
достаточно того, что даёт, плюс `GetModuleHandle`/Toolhelp-присутствие; тогда «наши vs чужие» ссылки
различаем по леджеру пинов из точек 2–3, 13–14, 15–16.

### R2 — детализация модулей (второй прогон)

- `WndCompat_BeginShutdown` / `WndCompat_IsShutdownComplete` + число снятых субклассов.
- `ExplorerCompat_Shutdown` / `UICompat_Shutdown` / `TaskbarCompat_Shutdown` + `*_DebugComState` до/после `CoFreeUnusedLibraries`.
- `AuditGlassMessageWindowsBeforeFree` (`GlassCompat.cpp:813`) — сколько наших окон ещё живо перед `FreeLibrary`.
- `g_winEventCallbacksInFlight` в каждой ветке `ExecuteShutdown`/drain.
- Учёт пинов peer-модулей: `g_hCore`, `g_hWndCompat`, `g_hExplorer`, `g_hTaskbar`, `g_hUICompat` (кто +1, кто −1).
- Сверка карты инжекта (`InjectProcessMap`) с реальным Toolhelp по pid.

### R3 — крайний случай

- Аудит всех `GetModuleHandleEx` внутри `GlassCore`/`WndCompat` (есть ли места без `UNCHANGED_REFCOUNT`).
- Loader-события извне (ETW / Process Monitor), если внутрипроцессных данных не хватит.
- Дамп (скилл `crashdump`) при крэше + сопоставление с `freeself_flaet` / `dll_detach`.

### Приёмка R1 (проверяется до первого прогона)

Инструментация R1 считается принятой, только если выполнено всё:

| # | Требование | Почему важно |
|---|---|---|
| A1 | Код R1 **в ветке** `arena/627639b4-glassplatform` (`git log origin/...` содержит коммит), а не только локально | логи без кода нельзя ни проверить, ни исправить |
| A2 | Хелпер лежит в `GlassShared/include/` (или `.cpp` в `GlassShared`), **не** в `.cursor/` и не в `_backups/` — оба каталога в `.gitignore` | код в игнорируемом каталоге не попадёт в ветку никогда |
| A3 | Нечитаемый LoadCount отдаётся **отдельным sentinel** (например `-1`), а не `0` | иначе «модуля нет» и «не смог прочитать» неразличимы, а на этом строятся выводы по H80/H82 |
| A4 | Калибровочная строка с офсетами/hex-дампом `LDR_DATA_TABLE_ENTRY` → `LDR_DDAG_NODE` и проверкой `LoadCount ∈ 1..64` | `DdagNode`/`LoadCount` версионно-зависимы; при неверном офсете `rc` — мусор, и все выводы ложные |
| A5 | Путь лога не хардкод `d:\...`: fallback `%LOCALAPPDATA%` → `%TEMP%` → ODS, фактический путь пишется в шапке сессии | AppContainer/SEH-процессы (самые проблемные) часто не имеют доступа к каталогу проекта и молча выпадут из выборки |
| A6 | Много процессов в один файл: открытие с `FILE_APPEND_DATA \| FILE_SHARE_READ \| FILE_SHARE_WRITE`, **одна** запись = один `WriteFile` (буфер строки с `\n` целиком) | иначе строки перемешиваются и NDJSON не парсится |
| A7 | На `path=2/3` (BlockList/Forbidden) **нет** новых потоков, CRT, `CreateFile` в watcher'е: verify там не планируется, только события | в этих процессах `DebugLog`-воркер уже приводил к `abort()` при reboot; инструментация не должна вернуть эту проблему |
| A8 | Фильтр по exe (переменная окружения или список) и выключение одним макросом | иначе Stop пишет сотни файлов и меняет тайминги гонки |
| A9 | `verify` существует и **из другого процесса** (хост/GS native): через 0/2/10 с, с битовой маской модулей | внутрипроцессный verify исчезает ровно тогда, когда модуль ушёл/процесс в AppContainer |

## 6. Протокол тестирования

Для каждого прогона приложить: время Stop (UTC), список процессов (PID + exe) до Stop,
собранные логи целиком, состояние страницы «Процессы» сразу после Stop и через 30 с.

| # | Сценарий | Что проверяет |
|---|---|---|
| S1 | Штатный Stop платформы при ~10 процессах с модулями | базовый таймлайн, H81/H82 |
| S2 | Stop + сразу Force Unload остатков из «Процессов» | H85, H88 |
| S3 | Unload одной строки из «Процессов» при **живом** хосте | H90, soft-путь |
| S4 | Открыть Пуск/меню → Ensure/re-inject → снова Stop | H80, H89 |
| S5 | `TerminateProcess` хоста (rundll32) → затем Stop/GS | путь `HostAlive` |
| S6 | Stop при `taskhostw` / `msedgewebview2` (BlockList) | path=2, H87 |
| S7 | Stop при CW-хосте, который ни разу не показывался (PLM freeze) | «GUI нет»: пины и восстановление |
| S8 | **5 циклов** Start→Stop подряд без перезагрузки | накопление +1 — главный тест гипотезы |

**Критерий приёмки:** после S8 через 5 с — ни одного процесса с `Glass*.dll`; ни одного крэша/дампа;
Stop укладывается в бюджет (baseline длительностей фиксируем в первом прогоне; цель ≤ 3 с на процесс,
хост `done:` ≤ 5 с).

---

## 7. Как читать логи: «признак → вывод → фикс»

| Признак | Вывод | Что делать |
|---|---|---|
| refcount перед FLAET == нашим пинам | лишних наших пинов нет, остаток — OS/deferred | H82: заняться доставкой deferred-free, а не «искать утечку» |
| refcount > наших пинов | есть неучтённая/чужая ссылка | R2: леджер пинов, искать источник (H80/H83/H86/H89) |
| компенсация remote вызвана, хотя remote-load не было | H80: ложная компенсация (лишний −1) | считать remote-pins счётчиком, флаг ставить только при реальной remote-загрузке |
| пин взят без парного снятия | H87/H89: утечка | закрыть ветку выхода, нумеровать поколения |
| установлено хуков > снято по pid | H81: часть хуков не снята | реестр хуков со счётчиком и сверкой |
| `cw_residual_drop` — модуль остался после free | H84: одного `FreeLibrary` не хватает | цикл до исчезновения, отказ при живом GUI |
| `fu_try` — wait/timeout, результат free не определён | H85: remote `FreeLibrary` не отработал или остатку >1 | возвращать результат, останавливать проходы, финальный `left=` |
| `dll_detach` раньше `host_hook_unhook` | преждевременный unmap (риск крэша) | разбирать порядок FLAET/компенсации |
| `dll_detach` через десятки секунд после Stop | H82: «поздняя» доставка | нормализовать прогрев/дренаж |
| `gs_request` soft прошёл, реакции в процессе нет | H90: path=1 не слушает `UnloadRequest` | добавить обработчик в GUI-цикл (после подтверждения) |

---

## Приложение A. Замеченные особенности (без правок; кандидаты в фиксы после данных)

1. `FreeSelfAndExitThread` компенсирует remote-load **одним** `FreeLibrary` при флаге-bool (`:2101-2103`),
   хотя флаг ставится на каждом bootstrap (`:3435`) — учёт неверен в обе стороны (H80).
2. `TryClearPid` вызывается **до** выгрузки (`:2097`) → диагностическая карта «врёт», GS может считать
   процесс выгруженным, пока DLL жива.
3. `ExecuteShutdown` освобождает `savedWnd` без проверки результата (`:1360`) — единственный модуль без контроля.
4. `g_hTaskbar`/`g_hExplorer` могут быть получены `GetModuleHandleW` (без +1), но освобождаться `FreeLibrary` (H86).
5. `ForceUnload` не различает «отработал» и «счётчик не дошёл до нуля»: `RemoteFreeLibrary` возвращает только
   факт завершения потока (`ForceUnloadGlassModules.cpp:70-90`), BOOL от `FreeLibrary` теряется.
6. `StartCwUnloadWatcher` / `StartMinimalUnloadWatcher` — `call_once`: после раннего выхода watcher'а
   второго шанса нет (H87).
7. `SignalHostShutdownComplete` → `ForceUnloadResiduals` (`EntryRundll.cpp:970,979`) допускает гонку FLAET vs remote free (H88).
8. `UnloadRequest` слушает только Minimal-watcher → path=1 процесс «не отвечает» на soft unload (H90).

---

## Приложение B. Правки-кандидаты (только после подтверждения данными)

| Гипотеза | Правка-кандидат | Риск |
|---|---|---|
| H80 | `g_remoteLoadLibraryPin` → счётчик remote-загрузок; `−1` ровно столько раз, сколько реально было remote-load | средний: меняет баланс |
| H81 | Реестр хуков: счётчик per-PID, снятие пока `n > 0`, предупреждение при расхождении | низкий |
| H82 | Вместо «слепого» `Sleep(300)` — верифицированный дренаж: ждать фактического исчезновения модуля с дедлайном, разбуживая потоки | средний |
| H84 | Цикл до 3 remote `FreeLibrary` с проверкой между ними и отказом при живом GUI | средний |
| H85 | Возвращать BOOL от `FreeLibrary` из remote-потока, прекращать проходы при неуспехе, финальный отчёт `left=` | низкий |
| H86 | Всегда `LoadLibrary` для `g_hTaskbar`/`g_hExplorer` (или флаг владения), `FreeLibrary` только по своей ссылке | низкий |
| H87 | Снятие пина в каждой ветке выхода watcher'а + сбрасываемый флаг вместо `call_once` | низкий |
| H90 | GUI-цикл path=1 слушает `UnloadRequest` → `ExecuteShutdown` + FreeSelf | средний |
