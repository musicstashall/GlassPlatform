# Унификация остановки платформы: единый центр `Shared::GlassStop`

Документ описывает, как остановка платформы устроена сейчас: кто её инициирует, где
выполняется остаточная выгрузка и почему все инициаторы сведены к одному механизму.
Инструментация, на которой построены выводы, — `GlassCompat/docs/unload-instrumentation.md`
(разделы R6–R12i).

---

## 1. Кто останавливает платформу

| # | Инициатор | Точка входа | Что делает | Файлы |
|---|---|---|---|---|
| 1 | **GlassSettings** (кнопка «Остановить») | `GlassPlatformService.StopPlatformAndWait` | `NativeHostInterop.RequestPlatformStop` → координатор (`Reason::User`); ждёт результата и показывает остатки | `GlassSettings/Core/Platform/GlassPlatformService.cs:88`, `Native/OpenGlassHostTask.cpp:387` |
| 2 | **GlassShellExt** (меню рабочего стола) | `DesktopShellActions.ExecutePlatformToggle` | `Shared::GlassStop::Request(Reason::User)`; при остатках — `stop_incomplete` | `GlassShellExt/DesktopShellActions.cpp:523` |
| 3 | **GlassSetup** (установка/обновление) | `StopGlassPlatformImpl` | `GlassStop::Request(Installer, gracefulMs=15000, allowTaskStop)`; `IRegisteredTask::Stop` — только если хост не ушёл за окно ожидания | `GlassSetup/src/InstallSupport/GlassInstallHost.cpp:50` |
| 4 | **GlassUnins** (удаление) | тот же путь с `Reason::Uninstaller` | то же; перед снятием MSI дополнительно проверяется, что задача ещё существует | `GlassSetup/src/InstallSupport/GlassInstallRemoval.cpp` |
| 5 | **Ручная остановка задачи в Планировщике** (и любой TerminateProcess хоста) | — | хост убивается; инжектированные процессы узнают об этом сами — по abandoned-мьютексу `GlassPlatform.HostAlive` (см. §2а) | `GlassCompat/src/GlassCompat.cpp:3283` |
| 6 | **LogViewer** | `UiMainWindow_ImplPlatform.cpp` | `GlassPlatformTask::SignalPlatformStop` — единственный оставшийся прямой `SetEvent`, ждать не требуется | `LogViewer/Ui/MainWindow/UiMainWindow_ImplPlatform.cpp:40` |
| 7 | **GlassCompat: CrashGuard** | `SignalPlatformStopBothArch` | `GlassStop::Request(Reason::Crash, gracefulMs=0)` — поведение прежнее (только сигнал) | `GlassCompat/src/CrashGuard.cpp:576` |
| 8 | **GlassCompat: HostShellWatchdog** | — | `GlassStop::Request(Reason::Crash, gracefulMs=0)` по обоим архам | `GlassCompat/src/HostShellWatchdog.cpp:56` |
| 9 | Хост платформы | `rundll32 GlassCompat.dll,Main /stop` | `GlassStop::Request(Reason::External)`: сигнал + `UnhookWinEvent` + выход | `GlassCompat/src/EntryRundll.cpp:912` |
| 10 | **GlassSetup/GlassUnins: «пинок» остаткам** | `GlassSettings_UnlockerW /wakeResiduals [86]` | `WM_NULL` окнам и потокам процессов с `GlassCompat`; ничего не грузит и не выгружает | `GlassSetup/src/InstallSupport/GlassInstallUnlock.cpp` |

Механизмов теперь два, и оба штатные: `StopEvent` (мягкая остановка с полной выгрузкой
хостом) и abandoned-мьютекс `HostAlive` (хост убит — каждый процесс уходит сам). Жёсткое
`IRegisteredTask::Stop` осталось только резервом координатора.

---

## 2. Где выполняется остаточная («дожим») выгрузка

Ни один из уровней не создаёт новый процесс. Схема из четырёх уровней:

| Уровень | Кто выполняет | В каком процессе | Что делает | События в логе |
|---|---|---|---|---|
| 1 | GUI-поток GlassCompat | **в каждом инжектированном процессе** | `stop_trigger` → `ExecuteShutdown` (peer-модули) → `FreeLibraryAndExitThread` | `stop_trigger`, `shutdown_begin/end`, `module_free`, `freeself_*` |
| 2 | Minimal-watcher (`StartMinimalUnloadWatcher`) | в том же процессе, отдельный поток | для `path=2/3` (BlockList/Forbidden): ждёт StopEvent / UnloadRequest / HostAlive → FLAET | `watcher_wake`, `watcher_exit` |
| 3 | Хост (`ForceUnloadResiduals`) | **в процессе хоста** (тот же `rundll32` + GlassCompat, который был `/start`) | `TryRemoteRequestShutdown` — `CreateRemoteThread` **внутрь чужого процесса**, вызывает его же export `GlassCompat_RequestShutdown`; затем guarded sweep (`ack` + число окон, `mode=2` — оставить) | `cw_residual_*`, `cw_remote_shutdown`, `cw_sweep_guard`, `cw_sweep_pid`, `cw_sweep_end` |
| 4 | ОС | — | когда процесс-владелец INCONTEXT-хуков (хост) завершается, user32 отпускает per-process ссылки, и оставшиеся процессы доканчивают свой unload | `dll_detach` |

Проверка по прогону 11 (`debug-8e6259-11.log`):

```
13:52:31.664  stop_trigger  в 11 процессах (каждый — свой pid)
13:52:32.453  host_shutdown_signal  pid 35788 (rundll32-хост)
13:52:35.6    freeself_flaet  DllHost/Happ/Rainmeter/CV_64 …
13:52:43.417  cw_residual_end  left=1 (explorer)
13:52:45…54    cw_sweep_guard mode=2 ×5 → denied=5 (никого не добивали FreeLibrary)
13:52:54.988  dll_detach хоста (process_exit=1)
13:52:55.0    dll_detach CV_64, explorer, Rainmeter, Happ — уже после выхода хоста
13:55:10.597  dll_detach DllHost (простаивал 2 мин 15 с — очередь не просыпалась)
```

Отсюда правила, которые нельзя терять при рефакторинге:

* **raw `FreeLibrary` извне запрещён** (R6/R7): снять ссылку user32 может только сама ОС;
* «дожим» — это просьба к процессу (graceful) и ожидание, а не принудительная выгрузка;
* окончательный результат виден только **после** выхода хоста (R8/R9, `verify post2/post8`);
* инертный процесс может отдать модуль через минуты (DllHost выше) — это норма, не ошибка.

---

---

## 2б. `ForceUnloadResiduals` пошагово (что делает «дожим»)

Функция живёт **в процессе хоста** (`GlassCompat/src/CoreWindowHostInject.cpp:2033-2440`) и
вызывается из `EntryRundll.cpp` после `host_shutdown_signal`. Ни одного нового процесса она
не создаёт.

| Шаг | Что делает | События |
|---|---|---|
| 1 | `CollectCompatCandidates` — снимок процессов, где есть `GlassCompat` (до всех ожиданий, чтобы не зацепить свежие инжекты) | — |
| 2 | забирает `g_remoteLoadPids` (процессы, куда модули грузили через `LoadLibrary` удалённо: ShellHost/SEH/Start/Search) и очищает список | — |
| 3 | добавляет CoreWindow-хосты, `explorer.exe`, `Widgets*`, у которых реально загружен `GlassCompat` (Toolhelp) | `cw_residual_begin`, `cw_residual_pid` |
| 4 | UICompat-restore: для каждого (кроме explorer — там remote shutdown давал AV в `Windows.UI.Xaml`) `TrySehPlmWake` → проверка `IsRemoteProcessSchedulable` → `WakeRemoteCwProcess` → `PostWmNullToTrackedHwnds` → `TryRemoteUICompatShutdown` | — |
| 5 | мягкий сигнал: `SignalRemoteUnload` (флаг в WPM + named event + `WM_QUIT`), wake, `WM_NULL` | — |
| 6 | немедленный graceful: `TryRemoteRequestShutdown` — `CreateRemoteThread` **внутрь** процесса с его же export `GlassCompat_RequestShutdown` (внутри: `RequestShutdown` → GUI-цикл → `ExecuteShutdown` → `FreeLibraryAndExitThread`). Это **просьба**, а не выгрузка | `cw_remote_shutdown` |
| 7 | ожидание №1: до 20 тиков (40 для смеси процессов) × 50 мс, каждый тик дешёвый wake; полное перечисление потоков — каждый 10-й тик; ранний выход, если остался только OS-pin (все `GuiTid=0`) | — |
| 8 | повторный сигнал + ожидание №2: 10–20 тиков × 50 мс | — |
| 9 | ушедшим — `TrySehPlmRest(DisableDebugging)`; у кого модуль остался — `EnableDebugging` не снимаем | — |
| 10 | подсчёт остатка | `cw_residual_end` (`left`, `left_pids`, `ms`) |
| 11 | sweep: по каждому кандидату с `mask==1` (только `compat`) — `ack` (named mutex `GlassCompat_ShutdownDone_<pid>`), число видимых окон, graceful + «пинок» окон/потоков, ожидание 0.6 с; не ушёл → `mode=2` (оставить) | `cw_sweep_guard`, `cw_sweep_pid` |
| 12 | агрегат | `cw_sweep_end` (`candidates/tried/unloaded/graceful/denied/left/left_pids`) |

Чего функция **не** делает (по политике R6/R7): не вызывает `FreeLibrary` в чужих процессах
(только при `GLASS_ULTRACE_SWEEP_ALL=1`), не завершает процессы, не трогает explorer-UICompat.

Бюджет времени в прогоне 11: `cw_residual_begin` 13:52:32.860 → `cw_residual_end` 13:52:43.417
(10.9 с, из них ожидания 1–2 и UICompat), затем sweep 13:52:45.7…54.7 (~9 с — по ~2.2 с на
кандидата, потому что каждый graceful-вызов ждёт до 3 с). Основной резерв ускорения Stop —
параллельный обход кандидатов и отказ от graceful для процессов с `ack=1` и без окон.

---

## 2а. Стоп через завершение хоста — штатный путь, а не дефект

Требование к платформе: **остановка хоста = сигнал остановки всем процессам**. Оно
выполнено и должно сохраниться при унификации. Механика (работает уже сейчас):

| Шаг | Кто | Что происходит |
|---|---|---|
| 1 | хост | всё время держит мьютекс `Local\GlassPlatform.HostAlive.x64` (own) |
| 2 | каждый инжектированный процесс | в GUI-цикле `WaitForMultipleObjects` ждёт `StopEvent` / `UnloadRequest` / **`HostAlive`** (`GlassCompat.cpp:3226-3290`) |
| 3 | хост убит (TaskScheduler `End`, диспетчер, падение) | мьютекс становится **abandoned** → ожидание отдаёт `WAIT_ABANDONED_0` |
| 4 | процесс | `ReleaseMutex` (передать сигнал следующему ждущему) → `guiStopTrigger="hostAlive"` → `ExecuteShutdown()` → `FreeLibraryAndExitThread` |
| 5 | процессы `path=2/3` (BlockList/Forbidden) | то же в `StartMinimalUnloadWatcher`: `trigger=hostAlive` → FLAET (`GlassCompat.cpp:4270-4290`) |
| 6 | ОС | после смерти процесса-владельца INCONTEXT-хуков user32 отпускает per-process ссылки, и оставшиеся процессы доканчивают unload |

Что при этом **не** происходит (и это нормально): хост, убитый жёстко, не выполняет свою
часть работы — `UnhookWinEvent`, `host_broadcast`, `ForceUnloadResiduals`. Компенсируют это
пункты 4–6: каждый процесс уходит сам, а ссылки на нашу DLL снимает ОС по факту смерти
процесса-владельца хуков. Именно поэтому в логе прогона 11 `dll_detach` приходят **после**
`dll_detach` хоста.

Что в этом пути было доработано (A1–A4, R11):

| # | Проблема | Что сделано |
|---|---|---|
| A1 | процесс, в который DLL попала INCONTEXT-инъекцией, но `WinEventProc` больше не вызывался, не имеет ни GUI-цикла, ни watcher'а → узнает о стопе только при следующем событии | при неудаче `StartGuiThreadIfNeeded` (и при `Initialize`, если GUI не поднялся) поднимать `StartMinimalUnloadWatcher(1)` — тогда liveness-ожидание есть у любого процесса с нашей DLL |
| A2 | «тихий» процесс может отдать модуль через минуты (в прогоне 11 `DllHost` 35892 — 2 мин 15 с после стопа) | различать «ещё не проснулся» и «не может»: watcher пишет `host_alive_seen` с задержкой; для инсталлятора это повод показать страницу остатков, а не жать |
| A3 | нет отдельной записи «стоп пришёл по смерти хоста» единого формата | `stop_trigger what="hostAlive"`, `stop_result reason=host_dead` в общем формате `GlassStop` |
| A4 | никто не проверяет, что путь «смерть хоста» реально доходит до всех | тест-сценарий U7: End задачи → у каждого процесса `stop_trigger what=hostAlive` → `verify post8 hits=0` |

**Вывод для унификации:** `StopEvent` остаётся предпочтительным путём (хост успевает снять
хуки и разослать `host_broadcast`), но путь «хост убит» — равноправный штатный режим, а не
ошибка. Поэтому в `GlassStop` он становится явной причиной `Reason::HostDead`, а не
«недостатком, который надо исключить»; установщику достаточно остановить задачу и
дождаться, пока процессы сами снимут модули (шаги 4–6), — принудительная выгрузка не нужна.

---

## 2в. Если хост убит планировщиком: `ForceUnloadResiduals` не выполняется — и что вместо него

**Короткий ответ: для самой выгрузки он и не нужен.** Функция целиком живёт в хосте, поэтому
при `TerminateProcess` её нет. Остаточную выгрузку в этом сценарии делают механизмы
**внутри каждого процесса**, которым хост не требуется:

| Механизм | Как срабатывает | Лог |
|---|---|---|
| abandoned-мьютекс `HostAlive` | ядро отдаёт `WAIT_ABANDONED_0` в `WaitForMultipleObjects` — не нужны ни очередь сообщений, ни окно | `stop_trigger what="hostAlive"` (`GlassCompat.cpp:3283`) |
| PID-poll хоста | `CheckHostAlive`: `ReadHostPid()` + `OpenProcess`/`WaitForSingleObject` на каждой итерации цикла (~1 с), debounce 2 тика | `stop_trigger what="hostLiveness"` (`GlassCompat.cpp:3641`) |
| minimal-watcher (`path=2/3`) | ждёт тот же мьютекс | `watcher_wake trigger=hostAlive` |

Дальше — как при мягком стопе: `ExecuteShutdown()` → `FreeLibraryAndExitThread`, а
оставшиеся ссылки снимает user32 по факту смерти процесса-владельца хуков.

**Чего при убитом хосте действительно нет** (это не выгрузка, а «разбудить и подстраховать»):

| Возможность хоста | Что теряется | Кто компенсирует |
|---|---|---|
| `host_broadcast` (HWND_BROADCAST WM_NULL) | общий пинок очередей | сам процесс: `DrainBeforeFreeSelf` пинает свои окна и callback-потоки |
| `PostWmNullAllThreads(pid)` — по ВСЕМ потокам чужого процесса | пробуждение потоков без окна (на них висит per-thread hook-ссылка) | никто: ссылка снимется, когда поток сам зайдёт в user32 |
| `TrySehPlmWake` | thaw PLM-frozen процесса | никто: процесс проснётся при активации |
| `TryRemoteRequestShutdown` | graceful в процесс, чей GUI-цикл не крутится (заморожен/застрял в XAML) | никто |
| `TryRemoteUICompatShutdown` | backup-restore для CoreWindow-хостов | сам процесс в `ExecuteShutdown` (в прогоне 11 `module_shutdown UICompat` у ShellHost 26352 отработал) |

Наблюдаемое следствие в прогоне 11: `DllHost` 35892 сделал `stop_trigger` в 13:52:31.664 и
`freeself_flaet` в 13:52:35.629 (`os_refs=1`), а `dll_detach` пришёл только в 13:55:10.597 —
через 2 мин 35 с. Это ровно тот класс остатка, который при живом хосте снимается пинком по
всем потокам.

Цена этого — **время, а не стабильность**: при убитом хосте ничего не выгружается
принудительно (policy R6/R7), поэтому худший случай — «модуль ещё лежит», а не AV.

### Закрытие окна «некому пинать»

| Вариант | Что делаем | Плюсы | Минусы |
|---|---|---|---|
| **W1. Только in-process** | ничего нового: процесс сам пинает свои окна/потоки, остаток снимается при следующем событии | ноль риска, ноль новых процессов | «тихий» процесс может держать модуль минутами (DllHost в прогоне 11) |
| **W2. Побудитель по запросу** | после `TaskStop` вызвать `rundll32 GlassSettings.Native.dll,GlassSettings_UnlockerW /wakeResiduals`: `PostMessage(WM_NULL)` окнам и потокам процессов с `GlassCompat`, **без** `FreeLibrary` | покрывает замороженные/застрявшие GUI; носитель уже есть (`GlassSettings.Native`, в нём нет Glass-модулей) | не работает при «голом» End задачи — будить некому |
| **W3. Сторожевая задача** | при старте платформы ставится короткоживущая задача-«сторож»: если хост исчез, выполняет W2; при штатном стопе удаляется | покрывает ЛЮБОЙ способ смерти хоста, включая End задачи и падение | +1 задача → обязательны правки GlassSetup/GlassUnins (создание/удаление/перечисление), иначе «фантом» после удаления |

Принято: **W2** — реализован в R11 (`GlassSettings_UnlockerW /wakeResiduals`, флаги
`wakeResiduals` в `GlassStop::Options`). **W3** не реализован: сторожевая задача меняет
установку/удаление, а «End задачи = полная остановка» уже обеспечивается механизмами
внутри процессов (W1).

Правила для W2/W3 (те же, что для верификатора в R10): запуск только при отсутствии
`GlassSetup.exe`/`GlassUnins.exe`/`msiexec.exe`; никакого `LoadLibrary` наших модулей в чужие
процессы; никакого `FreeLibrary`; помощник выходит сам за 8–10 с.


## 3. Почему появился координатор (состояние до R11)

1. **Пять инициаторов — четыре механизма.** Поведение зависит от того, кто нажал «стоп»:
   из установщика процесс умирает жёстко, из меню — мягко.
2. **Установщик не посылает StopEvent, а сразу убивает задачу.** Процессы при этом всё равно
   получают стоп (abandoned `HostAlive`, §2а), но хост не успевает сделать свою часть:
   `UnhookWinEvent` + `host_broadcast` + дожим остатков. Это не «лишний путь», а менее
   полный: его приблизили к штатному в R11 (A1–A4, §2а).
3. **Установщик умеет raw `ForceUnload`** (`/forceUnload` → `CreateRemoteThread(FreeLibrary)`),
   что противоречит политике R6/R7 и ровно этим был вызван AV в `Яндекс Музыка.exe`.
   При этом он и не нужен: после смерти хоста процессы снимают модуль сами (§2а).
4. **`StopEvent` не сбрасывается при старте хоста.** Событие manual-reset: `CreateEventW`
   на существующее имя вернёт уже сигнальное состояние, если кто-то ещё держит хэндл.
5. **`LifecycleBusy` берут не все** (GlassSetup/GlassUnins/LogViewer — нет), поэтому GS и
   ShellExt могут одновременно показать «останавливается» и запустить второй цикл.
6. **Нет единого результата**: кто ждёт `HostShutdown`, кто мьютекс, кто задачу; таймауты
   (3 с / 20 с / 30 с) разбросаны по местам вызова.
7. **Диагностика размазана**: `stop_trigger` пишет каждый процесс, но нет одной записи
   «кто, зачем и когда попросил стоп» и «чем это кончилось».

---

## 4. Устройство координатора

Единый координатор в `GlassShared` (его линкуют и Setup, и Unins, и ShellExt, и GS-native):

```cpp
// GlassShared/include/GlassStop.h
namespace Shared::GlassStop
{
    enum class Reason : std::uint8_t
    {
        User = 0,        // меню / кнопка в GlassSettings / LogViewer
        Installer = 1,   // GlassSetup
        Uninstaller = 2, // GlassUnins
        Crash = 3,       // CrashGuard / HostShellWatchdog
        External = 4,    // задача убита планировщиком/диспетчером
    };

    struct Options
    {
        Reason reason = Reason::User;
        DWORD  gracefulMs = 20000;  // ожидание HostShutdown
        DWORD  hostGoneMs = 5000;   // ожидание исчезновения HostAlive
        bool   allowTaskStop = false;   // разрешить IRegisteredTask::Stop как fallback
        bool   allowForceUnload = false; // ЗАПРЕЩЕНО по умолчанию (политика R6/R7)
    };

    struct Result
    {
        bool hostGone = false;
        bool hostKilledByTask = false;
        unsigned residualProcesses = 0; // из InjectModuleScan, только чтение
        DWORD elapsedMs = 0;
        const wchar_t* failedPhase = L""; // "" | "busy" | "signal" | "host" | "residuals"
    };

    Result Request(PlatformArch arch, const Options& options) noexcept;
    Result Request(PlatformArch arch) noexcept; // = Options{ Reason::User }
}
```

Внутри `Request` — единственная последовательность фаз (все инициаторы получают её же):

| Фаза | Действие | Отказ |
|---|---|---|
| 0 | если `HostAlive` нет → уже остановлено: посчитать остатки и вернуть | — |
| 1 | `TryBeginLifecycleBusy`; при `false` → `failedPhase="busy"` | второй стоп не запускается |
| 2 | `SetEvent(StopEvent)` + лог `stop_request{reason,arch}` | `failedPhase="signal"` |
| 3 | ждать `HostShutdown` ≤ `gracefulMs`, затем исчезновения `HostAlive` ≤ `hostGoneMs` | — |
| 4 | если хост жив и `allowTaskStop` → `IRegisteredTask::Stop` / `schtasks /End`, пометить `hostKilledByTask` | `failedPhase="host"` |
| 5 | `InjectModuleScan::RefreshInjectResidualSticky` (**только чтение**) → `residualProcesses` | `failedPhase="residuals"` |
| 6 | `EndLifecycleBusy` + `PulsePresence` + лог `stop_result{...}` | — |

Инварианты:

* **никогда** не вызывать `FreeLibrary` в чужих процессах (R6/R7) — опция есть только чтобы
  явно запрещать; raw-путь `FreeLibrary` из установщика удалён в R11;
* порядок фаз и семантика `path=1/2/3` не меняются (`GlassCompat/docs/GlassCompat-unload.md`);
* `StopEvent` остаётся тем же именем и тем же смыслом — совместимость с уже установленными
  версиями и с ручным `/stop` сохраняется.

Экспорт для управляемого кода (один, вместо `SignalPlatformStop*` + `StopOrphan*`):

```cpp
// GlassSettings/Native/OpenGlassHostTask.cpp
__declspec(dllexport) int WINAPI GlassSettings_RequestPlatformStop(int arch, int reason, int options);
```

---

## 5. Состав изменений (реализовано в R11)

| Работа | Что сделано | Где |
|---|---|---|
| Координатор | `Shared::GlassStop::Request`: фазы `busy` → сигнал → ожидание хоста → (при необходимости) остановка задачи → подсчёт остатков; единые `stop_request` / `stop_phase` / `stop_result` | `GlassShared/include/GlassStop.h`, `GlassShared/src/GlassStop.cpp` |
| Сброс залипшего `StopEvent` | при старте хоста сигнальное событие сбрасывается, в лог идёт `stop_event_stale_reset` | `GlassCompat/src/EntryRundll.cpp` |
| Мягкие инициаторы | GlassSettings (кнопка), GlassShellExt (меню), CrashGuard, HostShellWatchdog, `/stop` вызывают координатор со своим `Reason` | `GlassPlatformService.cs`, `DesktopShellActions.cpp`, `CrashGuard.cpp`, `HostShellWatchdog.cpp`, `EntryRundll.cpp` |
| Установщик и удалятор | `StopGlassPlatformImpl` → `GlassStop::Request(Installer/uninstaller, gracefulMs=15000, allowTaskStop)`; жёсткая остановка задачи осталась резервом | `GlassInstallHost.cpp`, `GlassInstallRemoval.cpp` |
| Принудительная выгрузка | `/forceUnload` из установщика по умолчанию не вызывается; вместо него страница остатков и штатное закрытие приложения | `GlassInstallUnlock.cpp` |
| Паритет «хост завершён» | `Reason::HostDead`, страховочный `StartMinimalUnloadWatcher(1)` при неудаче GUI-потока, `host_alive_ms` в `freeself_flaet` | `GlassCompat.cpp`, `EntryRundll.cpp` |
| «Пинок» остаткам (W2) | `GlassSettings_UnlockerW /wakeResiduals [86]`: `WM_NULL` окнам и потокам, без `FreeLibrary` | `GlassSetup/src/InstallSupport/GlassInstallUnlock.cpp` |

Не реализовано по решению пользователя: сторожевая задача W3 (меняет установку/удаление).
`LogViewer` инициатором стопа в коде не является — правок не потребовалось.

Матрица пересборки после любых правок координатора: `GlassShared` → `GlassCompat` →
`GlassSettings.Native` и C# (`GlassSettings`) → `GlassShellExt` → `GlassSetup` → `GlassUnins`.
Только `GlassShared` пересобирать бессмысленно: координатор компилируется в каждый линкующий
проект (`GlassUnins` собирает исходники GlassShared напрямую).

### Приёмка рефакторинга

| # | Требование | Состояние |
|---|---|---|
| U1 | Все инициаторы вызывают `GlassStop::Request`; одно место с `SetEvent(StopEvent)` | выполнено (R11) |
| U2 | Один формат записи о стопе: `stop_request` + `stop_result` | выполнено (R11) |
| U3 | Установщик и удалятор не делают `FreeLibrary` в чужих процессах без `ack=1` | выполнено (R11) |
| U4 | Меню, GS, установщик и удаление дают одинаковый итог | выполнено: прогоны 12–18, `left=0` |
| U5 | `StopEvent` не «протекает» между циклами старт/стоп | выполнено (R11) |
| U6 | Второй одновременный стоп не запускается (`failedPhase="busy"`) | выполнено (R11) |
| U7 | «End задачи» = полная остановка | выполнено (R12b), подтверждено прогонами 14–16 |
| U8 | Ни один сценарий остановки не требует `FreeLibrary` в чужом процессе | выполнено (R11) |

## 6. Ограничения и как они сняты

| Риск | Снятие |
|---|---|
| Жёсткий TaskStop из установщика был выбран, чтобы не ждать 20 с | `Options.gracefulMs` задаёт вызывающий; для установщика 15 с, затем TaskStop |
| `LifecycleBusy` теперь берут все — возможен «залипший» busy | busy всегда снимается в конце `Request` (RAII-хранитель) |
| Инжектированный процесс может не просыпаться (DllHost в прогоне 11 — 2 мин) | ожидание остатков не блокирует установку: страница остатков + решение пользователя |
| Изменение поведения `CrashGuard` | `gracefulMs=0`, только `SetEvent` — эквивалентно текущему |

---

## 7. Статус реализации (R11)

Этапы 1–5.5 реализованы в одном пакете; 5.6 отложен по решению пользователя.
Ниже — что именно поменялось и как это увидеть в логе.

| Что | Где | Тег/смысл в логе |
|---|---|---|
| Координатор `Shared::GlassStop` | `GlassShared/include/GlassStop.h`, `GlassShared/src/GlassStop.cpp` | `stop_request`, `stop_phase`, `stop_result`, `stop_residual`, `stop_wake_pid`, `stop_wake_end` |
| GlassSettings (C#) | `GlassPlatformService.StopPlatformAndWait` → `NativeHostInterop.RequestPlatformStop` | `reason="user"`, фазы `busy`/`wait_host`/`task_stop` |
| Нативный мост для managed | `GlassSettings_Native`: `GlassSettings_RequestPlatformStop(arch, reason, gracefulMs, hostGoneMs, flags)` | flags: bit0 `allowTaskStop`, bit1 `wakeResiduals`; `allowForceUnload` в API отсутствует (R6/R7) |
| GlassShellExt (меню) | `DesktopShellActions.ExecutePlatformToggle` (ветка `Running`) | `stop_request reason="user"` + `stop_incomplete`, если остатки/хост не ушли |
| CrashGuard / HostShellWatchdog | `GlassCompat/src/CrashGuard.cpp`, `HostShellWatchdog.cpp` | `reason="crash"`, `gracefulMs=0` — поведение прежнее (только сигнал) |
| `/stop` внутри хоста | `GlassCompat/src/EntryRundll.cpp` (Main) | `reason="external"`, без ожидания |
| Установщик | `GlassInstallHost::StopGlassPlatformImpl` | `reason="installer"`, 15 с штатного ожидания (R12h); задача останавливается только если хост за это время не ушёл, затем ожидание до 45 с |
| Удаляльщик | тот же путь с `Reason::Uninstaller` | `reason="uninstaller"` |
| Сброс залипшего `StopEvent` | `EntryRundll` после `CreateEventW` | `stop_event_stale_reset` (этап 2) |
| W2 «пинок» остаткам | `GlassSettings_UnlockerW /wakeResiduals [86]` | `stop_wake_pid`/`stop_wake_end`; ничего не грузит и не выгружает |
| Abort-parity | `Reason::HostDead` (ветка `host_dead_probe`), fallback `StartMinimalUnloadWatcher(1)` при неудаче `CreateThread` GUI, `host_alive_ms` в `freeself_flaet` | 5.1/5.2/5.3 |

Отложено:
* 5.6 (сторожевая задача W3) — требует создания/удаления задачи установщиком; без
  отдельного прогона не включаем, чтобы не менять установку/удаление (R10).
* `LogViewer`: инициатора стопа в коде нет — правка не требуется.

Матрица пересборки после этих правок: `GlassShared` → `GlassCompat` → `GlassSettings.Native`
и C# (`GlassSettings`) → `GlassShellExt` → `GlassSetup` → `GlassUnins`. Только `GlassShared`
пересобирать бессмысленно: координатор компилируется в каждый линкующий проект
(`GlassUnins` собирает исходники GlassShared напрямую).

Что искать в логе нового прогона:

1. `stop_request` с ожидаемым `reason` — ровно одна запись на инициатора.
2. `stop_phase phase="busy" acquired=1` — стоп захватил `LifecycleBusy` (U6).
3. `stop_phase phase="wait_host" gone=1` — хост ушёл сам, `task_stop` не потребовался.
4. `stop_result` + `stop_residual` — кто остался и с каким `target_pid`.
5. В инжектированных процессах при End задачи: `watcher_wake trigger="hostAlive"`,
   затем `freeself_flaet` с `host_alive_ms` (5.3) — это и есть доказательство
   оркестрации по §2а.
6. `verify tag="post8" hits=0` — итог (U4/U7).

### 7.1. Правки после прогона 12 (R12)

| Симптом прогона 12 | Причина | Правка |
|---|---|---|
| `stop_request` из DllHost → `stop_phase busy` и больше ничего, busy залипает | инициатор сам инжектирован и выгружается вместе с процессом | busy берётся и сразу отпускается (`held=0`); ожидание не требует busy |
| DllHost с нашими модулями не выгрузился вообще | DLL попала через глобальный хук, `Initialize` (и watcher) не вызывались | `GlassCompat::EnsureAttachWatcher()` из `DllMain` → `StartMinimalUnloadWatcher(4)` для всех, кроме хоста/rundll32/short-lived |
| «модули не выгружаются», выгрузка висит десятки секунд | `ScanWindowProcCb` рекурсивно вызывал `EnumChildWindows` (тот сам рекурсивен) → `windows=924992` при 184 окнах | отдельный `ScanChildWindowCb` без рекурсии |
| остатки в DllHost после FLAET (`os_refs=1`) | пинок делался только после ожидания, когда инициатора уже нет | WM_NULL-пинок сразу после сигнала (фаза 2b) + повтор в фазе 5 |
| не видно, почему процесс без watcher'а | не было записи о входе в `Initialize` | `init_begin`/`init_end` |

Проверка следующего прогона: `watcher_start path=4` у процессов из глобального хука,
`stale_scan windows` ≈ реальному числу окон, `stop_result` появляется и при стопе из ShellExt.

### 7.2. Правки после прогона 13 (R12b)

* **End задачи = полная остановка.** Раньше ветка `s_stopFromCloseMessage` в `EntryRundll`
  делала только `FinishTaskStop()`: хост уходил за 0.1 с, не разбудив и не дожав никого.
  Теперь перед выходом — четыре `SendNotifyMessage(HWND_BROADCAST, WM_NULL)` с шагом 700 мс:
  инжектированные процессы успевают выгрузиться, «спящие» (DllHost) — обработать deferred free.
  Полный `ForceUnloadResiduals` (≈20 с) при End задачи намеренно не вызывается: планировщик
  после `WM_CLOSE` переходит к `TerminateProcess`.
* **«Пропавшие» процессы в списке.** Слот, который писал страховочный watcher (`path=4`),
  теперь `Kind::ActiveGui`/`path=1` и не затирает уже существующую запись. Для UI `path=4` —
  это не «отвергнутый» процесс.
* **Список процессов показывает всё.** `InjectRuntimeMonitor` больше не выбрасывает
  rundll32-хост и сам GlassSettings: они видны как `IsToolProcess`, но не входят в счётчик
  остатков (иначе `HasResiduals` горел бы всегда).
* **Наблюдаемость.** `watcher_handles` — какие объекты открыл watcher; `crash_seen`/`crash_unhandled` —
  кто и с каким кодом упал.

### 7.3. Правки после прогона 14 (R12c)

* **Выгорание индикаторов вместо мгновенного удаления строк.** `InjectRuntimeMonitor` на
  cheap-idle отдавал пустой снимок (`PlatformRunning=false`), и таблица удаляла все строки
  одним sync'ом — стоп из планировщика выглядел «рвано» по сравнению со стопом из меню.
  Теперь снимок догорает: `WithFade()` держит строку, гася по одной точке модуля за тик
  (до 6 тиков), и только после этого строка уходит. Догорающие строки не считаются
  остатком и не питают sticky/bridge.
* **Отвергнутые видны.** Гейт загрузки на отказе пишет слот `RejectedBlockList`/`path=2`
  (`moduleBits=0`, `State::Alive`) и лог `deny_slot`; до этого отказанный процесс не
  существовал ни в карте, ни в UI.
* **Точный путь не теряется.** Регистрация слота в `StartMinimalUnloadWatcher` вынесена из
  `std::call_once`: страховка `path=4` больше не «съедает» право записать `path=2/3`.
* **Меньше шума.** `0x40010006`/`0x4001000A`/`0x406D1388` (OutputDebugString, имя потока)
  не считаются падениями в `CrashGuard`.

### 7.4. Правки после прогона 15 (R12d)

* **Оба пути стопа подтверждены логами.** Планировщик: `host_close_ping pings=4` → `StopEvent`
  у всех → `dll_detach` по одному (36.8…47.7 с). Меню: `stop_request reason=user`,
  `stop_wake_pid` 8/8, `freeself_flaet path=1 rc=2`, остаток `left=2` дожат позже.
* **Отвергнутые не зависят от жизни карты.** `InjectRuntimeMonitor` держит собственный кэш
  отказов (`Kind::Rejected*`): запись снимается, только когда процесс завершился. Раньше
  отказ показывался ровно столько, сколько живёт общая карта хоста.
* **Диагностика вместо догадок.** Экспорт `GlassSettings_LogMonitorSummary` пишет
  `monitor_summary`: слоты и отвергнутые слоты в `map.x64`/`map.x86` (нативно), то же после
  разбора в C# и то, что реально попало в список UI.

### 7.5. Регрессия относительно `master` (R12e)

* **Исключённые процессы пропали из «отвергнутых» после R7.** В `master` DLL грузилась в
  процесс из BlockList и `Initialize` писал слот `RejectedBlockList`. Гейт R7 отказывает
  до `Initialize`, поэтому слот не писался. Теперь отказ пишется в карту и при первом
  отказе, и при повторном (по кэш-мьютексу), с полем `cached` в логе `deny_slot`.
* Цвета статусов не менялись: жёлтый `#C49100` — «отвергнут», красный — остаток DLL.

### 7.6. Отвергнутые строки (R12f)

* **Бит модуля.** Слот отказа гейта пишется с `ModuleCompat` — так же, как `path=2` в
  `master`. Без бита строка отвергнутого процесса пустая: ни одной точки в колонках.
* **Без собственного кэша.** Список собирается только из карты и скана: закрылась карта —
  список очистился. Выгорание точек (R12c) убирает строки за ≤6 тиков после Stop.
* Рефакторинг выгрузки не затрагивался.

### 7.7. Свои инструменты в списке процессов (R12g)

* **GlassSettings и LogViewer не показываются.** В `master` отбрасывался только GlassSettings;
  теперь тот же фильтр применяется во всех ветках сборки списка и включает LogViewer.
  Хост платформы (rundll32 + GlassCompat) остаётся видимым как `IsToolProcess` — он и есть
  предмет наблюдения, но в счётчики остатков не входит.
* Ранее действовавшее правило «показывать всё, включая свои процессы» этим уточнено:
  речь шла о хосте, а не о tool-host'ах с локальными модулями.

### 7.8. Прогон 16: установка поверх падала на остановке x86 (R12h)

**Данные.** `GlassSetup.log`: `HostStop: ... (x64) signal hostGone=0 ms=5015` → далее
`IRegisteredTask::Stop hr=0` → «x64: stopped via Task Scheduler». Затем
`HostStop: ... (x86) signal hostGone=1 ms=2969 residuals=0` — и сразу ошибка
«Работающий экземпляр платформы не удалось однозначно связать с задачей».
В `debug-8e6259-16.log`: `stop_result reason=installer phase=done host_gone=1 ms=2969`,
все процессы выгрузились (`dll_detach` у Telegram/ShellHost/SearchHost/Explorer).

| Наблюдение | Причина |
|---|---|
| x86-хост остановился, установка всё равно упала | `StopGlassPlatformImpl` после graceful-сигнала шёл в `StopValidatedPlatformTask`, а тот требует `GetInstances() == 1`. Хост ушёл за время окна → экземпляров 0 → «не удалось связать» |
| x64-хост убивали задачей посреди дренажа | окно `gracefulMs` было 5 с, а штатная выгрузка 8-9 процессов занимает 10-20 с |

Правки R12h (только `GlassSetup`, механизм выгрузки не менялся):

| # | Файл | Правка |
|---|---|---|
| R12h-1 | `GlassSetup/src/InstallSupport/GlassInstallHost.cpp` | если `GlassStop::Request` вернул `hostGone=1` (или мьютекс уже свободен) — задача не останавливается, установка продолжается |
| R12h-2 | `GlassSetup/src/InstallSupport/GlassInstallRemoval.cpp` | `StopValidatedPlatformTask` в начале проверяет мьютекс: хост успел уйти — «останавливать нечего» вместо ошибки «ровно один экземпляр» |
| R12h-3 | `GlassSetup/src/InstallSupport/GlassInstallHost.cpp` | `gracefulMs` 5000 → 15000: у хоста есть время на штатную выгрузку, жёсткий `IRegisteredTask::Stop` остаётся только как резерв |
| R12h-4 | `GlassSetup/src/InstallEngine.cpp` | установка с нуля (`cleanInstall_`) пропускает этап сканирования модулей в процессах; на машине нет ни платформы, ни задач/служб/продуктов, которые могли бы держать Glass*.dll |

R12h-2 действует и на удалятор: `StopAllForUninstall` идёт тем же `StopGlassPlatformImpl`.

### 7.9. Прогон 17 → R12i: повторный отказ гейта дороже самого отказа

В `debug-8e6259-17.log` на 6098 строк было 1878 `deny_slot` — из них 1874 с `cached=1`,
1663 на одном `msedge.exe`. Это не 1878 процессов, а один Edge, который на каждой вкладке
снова просил user32 загрузить Glass: решение уже было закэшировано мьютексом
`GlassCompatGateDeny_<pid>`, но до проверки кэша успевали записаться `dll_attach`, слот карты
и `dll_detach`.

| # | Что сделано | Файл |
|---|---|---|
| R12i-1 | проверка кэша отказа вынесена **до** логов загрузки: попавший в кэш процесс пишет один слот и сразу `return FALSE` | `GlassCompat/DllMain.cpp` |
| R12i-2 | слот отказа пишется **один раз на процесс** (маркер `GlassCompatGateSlot_<pid>`), а не на каждую попытку | `GlassCompat/DllMain.cpp` |
| R12i-3 | `dll_detach` пишется только в паре с `dll_attach` (`g_ultraceAttachTick != 0`) | `GlassCompat/DllMain.cpp` |
| R12i-4 | тело attach-гейта вынесено в собственный блок внутри `case` (иначе MSVC C2360) | `GlassCompat/DllMain.cpp` |

Контракт `GlassCompat/docs/GlassCompat-unload.md` не тронут: фазы, таймауты и семантика
`path=1/2/3` те же. Прогон 18 подтвердил результат: 391 строка вместо 6098, `deny_slot` 5 на
5 процессов, `dll_attach`/`dll_detach` 17/12 — смысловые события не изменились.

### 7.10. Прогоны 18 (установка поверх) и 19 (удаление)

| Прогон | Остановка хостов | Итог |
|---|---|---|
| 18, установка поверх (`GlassSetup-3.log`) | x64 `hostGone=1 ms=8531`, x86 `hostGone=1 ms=2203`, `task_stop=0`, `residuals=0` | оба хоста ушли сами, задача не останавливалась; полный цикл установки 37 с |
| 19, удаление (`GlassInstallSupport.log`) | x64 `hostGone=0 ms=15015` → `IRegisteredTask::Stop hr=0`; x86 `hostGone=1 ms=3438` | x64-хост не уложился в окно: см. ниже |

Почему в прогоне 19 x64-хост не ушёл за 15 с (в прогоне 18 — за 8.5 с):

* `cw_residual_end left=1 left_pids=39400` — после остановки в explorer остался `GlassCompat`;
* у explorer `rc_wait rc_start=2 rc_end=2 released=0` и `freeself_flaet os_refs=1` — ссылку
  держит ОС, снять её наша выгрузка не может (политика R6/R7: `FreeLibrary` извне не делаем);
* `ForceUnloadResiduals` ждал этого остатка до 17:45:02.7, хост вышел сразу после — то есть
  окна в 15 с не хватило на 1–2 с, и задачу добил установщик.

Ошибкой это не является (R12h-1 корректно продолжил установку), но резерв ускорения есть:
не ждать кандидата, который уже ответил `hostshutdown_done` и чей `rc_wait` не показывает
уменьшения refcount. Правка не внесена — меняет ожидание в `ForceUnloadResiduals`, нужен
отдельный прогон.

