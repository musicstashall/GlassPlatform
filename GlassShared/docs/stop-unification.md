# Унификация остановки платформы: единый центр `Shared::GlassStop`

Документ описывает, как остановка платформы устроена сейчас: кто её инициирует, где
выполняется остаточная выгрузка и почему все инициаторы сведены к одному механизму.
Контракт выгрузки в инжектированных процессах — `GlassCompat/docs/GlassCompat-unload.md`
(его фазы, таймауты и семантика `path=1/2/3` от этого координатора не зависят).

---

## 1. Кто останавливает платформу

| # | Инициатор | Точка входа | Что делает | Файлы |
|---|---|---|---|---|
| 1 | **GlassSettings** (кнопка «Остановить») | `GlassPlatformService.StopPlatformAndWait` | `NativeHostInterop.RequestPlatformStop` → координатор (`Reason::User`); ждёт результата и показывает остатки | `GlassSettings/Core/Platform/GlassPlatformService.cs`, `Native/OpenGlassHostTask.cpp` |
| 2 | **GlassShellExt** (меню рабочего стола) | `DesktopShellActions.ExecutePlatformToggle` | `Shared::GlassStop::Request(Reason::User)`; при остатках показывает пользователю итог остановки | `GlassShellExt/DesktopShellActions.cpp` |
| 3 | **GlassSetup** (установка/обновление) | `StopGlassPlatformImpl` | `GlassStop::Request(Installer, gracefulMs=15000, allowTaskStop)`; `IRegisteredTask::Stop` — только если хост не ушёл за окно ожидания | `GlassSetup/src/InstallSupport/GlassInstallHost.cpp` |
| 4 | **GlassUnins** (удаление) | тот же путь с `Reason::Uninstaller` | то же; перед снятием MSI дополнительно проверяется, что задача ещё существует | `GlassSetup/src/InstallSupport/GlassInstallRemoval.cpp` |
| 5 | **Ручная остановка задачи в Планировщике** (и любой `TerminateProcess` хоста) | — | хост убивается; инжектированные процессы узнают об этом сами — по abandoned-мьютексу `GlassPlatform.HostAlive` (см. §2а) | `GlassCompat/src/GlassCompat.cpp` |
| 6 | **LogViewer** | `UiMainWindow_ImplPlatform.cpp` | `GlassPlatformTask::SignalPlatformStop` — единственный оставшийся прямой `SetEvent`, ждать не требуется | `LogViewer/Ui/MainWindow/UiMainWindow_ImplPlatform.cpp` |
| 7 | **GlassCompat: CrashGuard** | `SignalPlatformStopBothArch` | `GlassStop::Request(Reason::Crash, gracefulMs=0)` — поведение прежнее (только сигнал) | `GlassCompat/src/CrashGuard.cpp` |
| 8 | **GlassCompat: HostShellWatchdog** | — | `GlassStop::Request(Reason::Crash, gracefulMs=0)` по обоим архам | `GlassCompat/src/HostShellWatchdog.cpp` |
| 9 | Хост платформы | `rundll32 GlassCompat.dll,Main /stop` | `GlassStop::Request(Reason::External)`: сигнал + `UnhookWinEvent` + выход | `GlassCompat/src/EntryRundll.cpp` |
| 10 | **GlassSetup/GlassUnins: «пинок» остаткам** | `GlassSettings_UnlockerW /wakeResiduals [86]` | `WM_NULL` окнам и потокам процессов с `GlassCompat`; ничего не грузит и не выгружает | `GlassSetup/src/InstallSupport/GlassInstallUnlock.cpp` |

Механизмов два, и оба штатные: `StopEvent` (мягкая остановка с полной выгрузкой хостом)
и abandoned-мьютекс `HostAlive` (хост убит — каждый процесс уходит сам). Жёсткое
`IRegisteredTask::Stop` осталось только резервом координатора.

---

## 2. Где выполняется остаточная («дожим») выгрузка

Ни один из уровней не создаёт новый процесс. Схема из четырёх уровней:

| Уровень | Кто выполняет | В каком процессе | Что делает |
|---|---|---|---|
| 1 | GUI-поток GlassCompat | **в каждом инжектированном процессе** | по `StopEvent` → `ExecuteShutdown` (peer-модули) → `FreeLibraryAndExitThread` |
| 2 | Minimal-watcher (`StartMinimalUnloadWatcher`) | в том же процессе, отдельный поток | для `path=2/3` (BlockList/Forbidden): ждёт `StopEvent` / `UnloadRequest` / `HostAlive` → FLAET |
| 3 | Хост (`ForceUnloadResiduals`) | **в процессе хоста** (тот же `rundll32` + GlassCompat, который был `/start`) | `TryRemoteRequestShutdown` — `CreateRemoteThread` **внутрь чужого процесса** вызывает его же export `GlassCompat_RequestShutdown`; затем sweep с graceful-просьбой и «пинком» окон/потоков |
| 4 | ОС | — | когда завершается хост — владелец INCONTEXT-хуков, user32 отпускает per-process ссылки, и оставшиеся процессы доканчивают свой unload |

Отсюда правила, которые нельзя терять при рефакторинге:

* **raw `FreeLibrary` извне запрещён** (политика R6/R7): снять ссылку user32 может только
  сама ОС, когда процесс обработает `UnhookWinEvent`;
* «дожим» — это просьба к процессу (graceful) и ожидание, а не принудительная выгрузка;
* окончательный результат виден только **после** выхода хоста;
* инертный процесс может отдать модуль через минуты — это норма, не ошибка.

---

## 2а. Стоп через завершение хоста — штатный путь, а не дефект

Требование к платформе: **остановка хоста = сигнал остановки всем процессам**. Оно
выполнено и должно сохраниться при унификации. Механика:

| Шаг | Кто | Что происходит |
|---|---|---|
| 1 | хост | всё время держит мьютекс `Local\GlassPlatform.HostAlive.x64` (own) |
| 2 | каждый инжектированный процесс | в GUI-цикле `WaitForMultipleObjects` ждёт `StopEvent` / `UnloadRequest` / **`HostAlive`** |
| 3 | хост убит (`End` в планировщике, диспетчер, падение) | мьютекс становится **abandoned** → ожидание отдаёт `WAIT_ABANDONED_0` |
| 4 | процесс | `ReleaseMutex` (передать сигнал следующему ждущему) → `ExecuteShutdown()` → `FreeLibraryAndExitThread` |
| 5 | процессы `path=2/3` (BlockList/Forbidden) | то же в `StartMinimalUnloadWatcher`: `trigger=hostAlive` → FLAET |
| 6 | ОС | после смерти процесса-владельца INCONTEXT-хуков user32 отпускает per-process ссылки, и оставшиеся процессы доканчивают unload |

Что при этом **не** происходит (и это нормально): хост, убитый жёстко, не выполняет свою
часть работы — `UnhookWinEvent`, `host_broadcast`, `ForceUnloadResiduals`. Компенсируют это
пункты 4–6: каждый процесс уходит сам, а ссылки на нашу DLL снимает ОС по факту смерти
процесса-владельца хуков.

Что в этом пути доработано:

| # | Проблема | Что сделано |
|---|---|---|
| A1 | процесс, в который DLL попала INCONTEXT-инъекцией, но `WinEventProc` больше не вызывался, не имеет ни GUI-цикла, ни watcher'а → узнает о стопе только при следующем событии | при неудаче `StartGuiThreadIfNeeded` (и при `Initialize`, если GUI не поднялся) поднимается `StartMinimalUnloadWatcher(1)` — liveness-ожидание есть у любого процесса с нашей DLL |
| A2 | «тихий» процесс может отдать модуль через минуты | «ещё не проснулся» и «не может» различаются по времени: для инсталлятора остаток — повод показать страницу остатков и дать решить пользователю, а не жать каждый процесс |
| A3 | стоп по смерти хоста не отличался от обычного | `Reason::HostDead` в координаторе: снимок остатков и «пинок» без сигнала и без остановки задачи |
| A4 | никто не проверял, что путь «смерть хоста» доходит до всех | проверено прогонами: `End` задачи → `ExecuteShutdown` во всех процессах с нашей DLL → остатков нет |

**Вывод:** `StopEvent` остаётся предпочтительным путём (хост успевает снять хуки и разослать
`host_broadcast`), но путь «хост убит» — равноправный штатный режим, а не ошибка. Поэтому в
`GlassStop` он выражен явной причиной `Reason::HostDead`, а установщику достаточно остановить
задачу и дождаться, пока процессы сами снимут модули (шаги 4–6) — принудительная выгрузка
не нужна.

---

## 2в. Если хост убит планировщиком: `ForceUnloadResiduals` не выполняется — и что вместо него

**Короткий ответ: для самой выгрузки он и не нужен.** Функция целиком живёт в хосте, поэтому
при `TerminateProcess` её нет. Остаточную выгрузку в этом сценарии делают механизмы
**внутри каждого процесса**, которым хост не требуется:

| Механизм | Как срабатывает |
|---|---|
| abandoned-мьютекс `HostAlive` | ядро отдаёт `WAIT_ABANDONED_0` в `WaitForMultipleObjects` — не нужны ни очередь сообщений, ни окно |
| PID-poll хоста | `CheckHostAlive`: `ReadHostPid()` + `OpenProcess`/`WaitForSingleObject` на каждой итерации цикла (~1 с), debounce 2 тика |
| minimal-watcher (`path=2/3`) | ждёт тот же мьютекс |

Дальше — как при мягком стопе: `ExecuteShutdown()` → `FreeLibraryAndExitThread`, а
оставшиеся ссылки снимает user32 по факту смерти процесса-владельца хуков.

**Чего при убитом хосте действительно нет** (это не выгрузка, а «разбудить и подстраховать»):

| Возможность хоста | Что теряется | Кто компенсирует |
|---|---|---|
| `host_broadcast` (`HWND_BROADCAST`, `WM_NULL`) | общий пинок очередей | сам процесс: `DrainBeforeFreeSelf` пинает свои окна и callback-потоки |
| `PostWmNullAllThreads(pid)` — по ВСЕМ потокам чужого процесса | пробуждение потоков без окна (на них висит per-thread hook-ссылка) | никто: ссылка снимется, когда поток сам зайдёт в user32 |
| `TrySehPlmWake` | thaw PLM-frozen процесса | никто: процесс проснётся при активации |
| `TryRemoteRequestShutdown` | graceful в процесс, чей GUI-цикл не крутится (заморожен/застрял в XAML) | никто |
| `TryRemoteUICompatShutdown` | backup-restore для CoreWindow-хостов | сам процесс в `ExecuteShutdown` |

Цена этого — **время, а не стабильность**: при убитом хосте ничего не выгружается
принудительно (политика R6/R7), поэтому худший случай — «модуль ещё лежит», а не AV.

### Закрытие окна «некому пинать»

| Вариант | Что делаем | Плюсы | Минусы |
|---|---|---|---|
| **W1. Только in-process** | ничего нового: процесс сам пинает свои окна/потоки, остаток снимается при следующем событии | ноль риска, ноль новых процессов | «тихий» процесс может держать модуль минутами |
| **W2. Побудитель по запросу** | после остановки задачи вызвать `rundll32 GlassSettings.Native.dll,GlassSettings_UnlockerW /wakeResiduals`: `PostMessage(WM_NULL)` окнам и потокам процессов с `GlassCompat`, **без** `FreeLibrary` | покрывает замороженные/застрявшие GUI; носитель уже есть (`GlassSettings.Native`, в нём нет Glass-модулей) | не работает при «голом» `End` задачи — будить некому |
| **W3. Сторожевая задача** | при старте платформы ставится короткоживущая задача-«сторож»: если хост исчез, выполняет W2; при штатном стопе удаляется | покрывает ЛЮБОЙ способ смерти хоста, включая `End` задачи и падение | +1 задача → обязательны правки GlassSetup/GlassUnins (создание/удаление/перечисление), иначе «фантом» после удаления |

Принято: **W2** — реализован (`GlassSettings_UnlockerW /wakeResiduals`, флаг `wakeResiduals`
в `GlassStop::Options`). **W3** не реализован: сторожевая задача меняет установку/удаление,
а «`End` задачи = полная остановка» уже обеспечивается механизмами внутри процессов (W1).

Правила для W2/W3 (те же, что для верификатора): запуск только при отсутствии
`GlassSetup.exe`/`GlassUnins.exe`/`msiexec.exe`; никакого `LoadLibrary` наших модулей в чужие
процессы; никакого `FreeLibrary`; помощник выходит сам за 8–10 с.

---

## 3. Почему появился координатор (состояние до R11)

1. **Пять инициаторов — четыре механизма.** Поведение зависело от того, кто нажал «стоп»:
   из установщика процесс умирал жёстко, из меню — мягко.
2. **Установщик не посылал `StopEvent`, а сразу убивал задачу.** Процессы при этом всё равно
   получали стоп (abandoned `HostAlive`, §2а), но хост не успевал сделать свою часть:
   `UnhookWinEvent` + `host_broadcast` + дожим остатков.
3. **Установщик умел raw `ForceUnload`** (`/forceUnload` → `CreateRemoteThread(FreeLibrary)`),
   что противоречит политике R6/R7 и ровно этим был вызван AV в `Яндекс Музыка.exe`.
   При этом он и не нужен: после смерти хоста процессы снимают модуль сами (§2а).
4. **`StopEvent` не сбрасывался при старте хоста.** Событие manual-reset: `CreateEventW`
   на существующее имя вернул бы уже сигнальное состояние, если кто-то ещё держит хэндл.
5. **`LifecycleBusy` брали не все** (GlassSetup/GlassUnins/LogViewer — нет), поэтому GS и
   ShellExt могли одновременно показать «останавливается» и запустить второй цикл.
6. **Не было единого результата**: кто ждал `HostShutdown`, кто мьютекс, кто задачу;
   таймауты (3 с / 20 с / 30 с) были разбросаны по местам вызова.

---

## 4. Устройство координатора

Единый координатор в `GlassShared` (его линкуют Setup, Unins, ShellExt и GS-native):

```cpp
// GlassShared/include/GlassStop.h
namespace Shared::GlassStop
{
    enum class Reason : int
    {
        User = 0,        // меню / кнопка в GlassSettings / LogViewer
        Installer = 1,   // GlassSetup
        Uninstaller = 2, // GlassUnins
        Crash = 3,       // CrashGuard / HostShellWatchdog
        External = 4,    // задача/хост завершены извне
        HostDead = 5,    // зафиксирована смерть хоста без StopEvent
    };

    struct Options
    {
        Reason reason = Reason::User;
        DWORD  gracefulMs = kDefaultPlatformShutdownWaitMs; // ожидание штатного завершения
        DWORD  hostGoneMs = 5000;   // ожидание исчезновения HostAlive после TaskStop
        bool   allowTaskStop = false;   // разрешить IRegisteredTask::Stop как fallback
        bool   wakeResiduals = false;   // WM_NULL окнам/потокам остатков
        // Принудительной выгрузки в чужих процессах координатор не умеет:
        // raw FreeLibrary извне запрещён (политика R6/R7), опции для неё нет.
    };

    struct Result
    {
        bool hostGone = false;
        bool hostKilledByTask = false;
        unsigned residualProcesses = 0;   // из InjectModuleScan, только чтение
        DWORD elapsedMs = 0;
        const wchar_t* failedPhase = L""; // "" | "dead_probe" | "signal" | "host" | "busy" | "already_stopped"
        unsigned residualPidCount = 0;
        DWORD residualPids[16]{};
    };

    Result Request(PlatformArch arch, const Options& options) noexcept;
    Result Request() noexcept;                       // = Reason::User для текущей арх
    unsigned SnapshotResiduals(PlatformArch, DWORD* outPids, unsigned maxPids) noexcept;
    unsigned WakeResidualProcesses(PlatformArch, DWORD* outPids, unsigned maxPids) noexcept;
}
```

Последовательность в `Request` (`GlassShared/src/GlassStop.cpp`):

| Шаг | Действие |
|---|---|
| 0 | `Reason::HostDead` — особый путь: снимок остатков, при `wakeResiduals` «пинок», `failedPhase="already_stopped"`; сигнала и остановки задачи нет |
| 1 | платформа уже не работает → `hostGone=1`, `failedPhase="already_stopped"`, снимок остатков |
| 2 | `SignalPlatformStop` — единственное место, где выставляется `StopEvent`; отказ → `failedPhase="signal"` |
| 3 | `TryBeginLifecycleBusy` и **сразу** `EndLifecycleBusy`: busy держится только на время сигнала (см. §7.1) |
| 4 | при `wakeResiduals` — «пинок» остаткам **до** ожидания (инициатор может исчезнуть вместе со своим процессом) |
| 5 | `WaitForPlatformStopped(gracefulMs)` → `hostGone` |
| 6 | если хост не ушёл и `allowTaskStop` — `StopOrphanAutorunTaskInstance`, затем ожидание `hostGoneMs` |
| 7 | снимок остатков (`CollectResiduals` → `InjectModuleScan`, только чтение); при `wakeResiduals` «пинок» и повторный снимок |
| 8 | `RefreshInjectResidualSticky`; `failedPhase="host"`, если хост остался |

Инварианты:

* **никогда** не вызывать `FreeLibrary` в чужих процессах (R6/R7);
* порядок фаз и семантика `path=1/2/3` не меняются (`GlassCompat/docs/GlassCompat-unload.md`);
* `StopEvent` остаётся тем же именем и тем же смыслом — совместимость с уже установленными
  версиями и с ручным `/stop` сохраняется.

Экспорт для управляемого кода:

```cpp
// GlassSettings/Native/OpenGlassHostTask.cpp
__declspec(dllexport) int WINAPI GlassSettings_RequestPlatformStop(int arch, int reason, int options);
```

---

## 5. Состав изменений (R11)

| Работа | Что сделано | Где |
|---|---|---|
| Координатор | `Shared::GlassStop::Request`: сигнал → (при необходимости) остановка задачи → снимок остатков; единый `Result` вместо трёх разных ожиданий | `GlassShared/include/GlassStop.h`, `GlassShared/src/GlassStop.cpp` |
| Сброс залипшего `StopEvent` | при старте хоста сигнальное событие сбрасывается | `GlassCompat/src/EntryRundll.cpp` |
| Мягкие инициаторы | GlassSettings (кнопка), GlassShellExt (меню), CrashGuard, HostShellWatchdog, `/stop` вызывают координатор со своим `Reason` | `GlassPlatformService.cs`, `DesktopShellActions.cpp`, `CrashGuard.cpp`, `HostShellWatchdog.cpp`, `EntryRundll.cpp` |
| Установщик и деинсталлятор | `StopGlassPlatformImpl` → `GlassStop::Request(Installer/Uninstaller, gracefulMs=15000, allowTaskStop)`; жёсткая остановка задачи осталась резервом | `GlassInstallHost.cpp`, `GlassInstallRemoval.cpp` |
| Принудительная выгрузка | `/forceUnload` из установщика по умолчанию не вызывается; вместо него страница остатков и штатное закрытие приложения | `GlassInstallUnlock.cpp` |
| Паритет «хост завершён» | `Reason::HostDead`, страховочный `StartMinimalUnloadWatcher(1)` при неудаче GUI-потока | `GlassCompat.cpp`, `EntryRundll.cpp` |
| «Пинок» остаткам (W2) | `GlassSettings_UnlockerW /wakeResiduals [86]`: `WM_NULL` окнам и потокам, без `FreeLibrary` | `GlassSetup/src/InstallSupport/GlassInstallUnlock.cpp` |

Не реализовано по решению пользователя: сторожевая задача W3 (меняет установку/удаление).
`LogViewer` вызывает `SignalPlatformStop` напрямую (единственный оставшийся прямой `SetEvent`).

Матрица пересборки после любых правок координатора: `GlassShared` → `GlassCompat` →
`GlassSettings.Native` и C# (`GlassSettings`) → `GlassShellExt` → `GlassSetup` → `GlassUnins`.
Только `GlassShared` пересобирать бессмысленно: координатор компилируется в каждый линкующий
проект (`GlassUnins` собирает исходники GlassShared напрямую).

### Приёмка рефакторинга

| # | Требование | Состояние |
|---|---|---|
| U1 | Все инициаторы вызывают `GlassStop::Request`; одно место с `SetEvent(StopEvent)` | выполнено (R11); исключение — LogViewer, только сигнал |
| U2 | Один формат результата остановки: `Result` из координатора | выполнено (R11) |
| U3 | Установщик и деинсталлятор не делают `FreeLibrary` в чужих процессах без `ack=1` | выполнено (R11) |
| U4 | Меню, GS, установщик и удаление дают одинаковый итог | выполнено: прогоны 12–19 |
| U5 | `StopEvent` не «протекает» между циклами старт/стоп | выполнено (R11) |
| U6 | Второй одновременный стоп не запускается | выполнено (R11) |
| U7 | «`End` задачи» = полная остановка | выполнено (R12b), подтверждено прогонами 14–16 |
| U8 | Ни один сценарий остановки не требует `FreeLibrary` в чужом процессе | выполнено (R11) |

---

## 6. Ограничения и как они сняты

| Риск | Снятие |
|---|---|
| Жёсткий TaskStop из установщика был выбран, чтобы не ждать 20 с | `Options.gracefulMs` задаёт вызывающий; для установщика 15 с, затем TaskStop |
| `LifecycleBusy` берут все — возможен «залипший» busy | busy держится только на время сигнала и снимается в той же фазе |
| Инжектированный процесс может не просыпаться | ожидание остатков не блокирует установку: страница остатков + решение пользователя |
| Изменение поведения `CrashGuard` | `gracefulMs=0`, только `SetEvent` |

---

## 7. Решения, принятые по ходу отладки (R11–R13)

Инструментация, на которой эти решения были получены, из дерева удалена; ниже — только
поведение, которое осталось в коде.

### 7.1. Busy не держится на время ожидания (R12)

Инициатор стопа сам может быть инжектированным процессом (`GlassShellExt` в explorer или в
`DllHost`) и выгружается вместе с остальными. Если его поток умрёт на ожидании, busy остался
бы залипшим навсегда. Поэтому `TryBeginLifecycleBusy` берётся и сразу отпускается: ожидание
хостом не требует busy.

Кроме того, `DllMain` вызывает `GlassCompat::EnsureAttachWatcher`: процесс, в который DLL
попала через глобальный хук без `WinEventProc`, всё равно получает minimal-watcher и
liveness-ожидание.

### 7.2. «`End` задачи» = полная остановка (R12b)

Ветка обработки закрытия хоста (`EntryRundll`) перед выходом делает несколько
`SendNotifyMessage(HWND_BROADCAST, WM_NULL)`: инжектированные процессы успевают выгрузиться,
«спящие» (`DllHost`) — обработать deferred free. Полный `ForceUnloadResiduals` (~20 с) при
`End` задачи намеренно не вызывается: планировщик после `WM_CLOSE` переходит к
`TerminateProcess`.

### 7.3. Список процессов в GlassSettings (R12b–R12g)

* строки не исчезают мгновенно: `WithFade()` гасит по одной точке модуля за тик (до 6 тиков),
  догорающие строки не считаются остатком и не питают sticky-кэш;
* исключённые гейтом процессы видны как «отвергнутые» — слот `RejectedBlockList` с битом
  `ModuleCompat` пишет сам `DllMain` (до `Initialize` дело не доходит);
* `GlassSettings.exe` и `LogViewer.exe` в список не попадают; хост платформы
  (`rundll32` + `GlassCompat`) виден как `IsToolProcess`, но в счётчики остатков не входит.

### 7.4. Остановка хостов в установщике (R12h)

| # | Файл | Правка |
|---|---|---|
| R12h-1 | `GlassInstallHost.cpp` | если координатор вернул `hostGone=1` (или мьютекс уже свободен) — задача не останавливается, установка продолжается |
| R12h-2 | `GlassInstallRemoval.cpp` | `StopValidatedPlatformTask` в начале проверяет мьютекс: хост успел уйти — «останавливать нечего» вместо ошибки «ровно один экземпляр» |
| R12h-3 | `GlassInstallHost.cpp` | `gracefulMs` 5000 → 15000: у хоста есть время на штатную выгрузку, жёсткий `IRegisteredTask::Stop` остаётся резервом |
| R12h-4 | `GlassSetup/src/InstallEngine.cpp` | установка с нуля пропускает этап сканирования модулей в процессах: на машине нет ни платформы, ни задач, которые могли бы держать `Glass*.dll` |

R12h-2 действует и на деинсталлятор: `StopAllForUninstall` идёт тем же `StopGlassPlatformImpl`.

### 7.5. Повторный отказ гейта (R12i)

Повторные `LoadLibrary` от user32 в исключённом процессе (msedge — сотни попыток за сессию)
не должны заново считать решение гейта (файл, реестр, среда):

| # | Что сделано | Файл |
|---|---|---|
| R12i-1 | проверка кэша отказа (мьютекс `GlassCompatGateDeny_<pid>`) вынесена **до** гейта: попавший в кэш процесс сразу `return FALSE` | `GlassCompat/DllMain.cpp` |
| R12i-2 | слот отказа публикуется при **каждом** отказе — и по кэшу, и по свежему решению | `GlassCompat/DllMain.cpp` |
| R12i-3 | тело attach-гейта вынесено в собственный блок внутри `case` (иначе MSVC C2360) | `GlassCompat/DllMain.cpp` |

Почему «каждого»: карту инжекта создаёт хост на старте (`CreateHostResources` её обнуляет) и
закрывает на остановке, то есть у каждой сессии платформы своя карта. Промежуточная попытка
писать слот «один раз на процесс» (по маркеру `GlassCompatGateSlot_<pid>`) ломала список:
процесс, получивший отказ в прошлой сессии, в новую карту уже не попадал, и «отвергнутые»
исчезали из GlassSettings, хотя user32 продолжал подсовывать им `GlassCompat`.
Цена — один `OpenFileMapping`+`MapViewOfFile` на попытку загрузки. Плюс хост на старте
(`RepublishCachedAttachDenials`) публикует слоты по маркерам отказа `GlassCompatGateDeny_<pid>`:
процесс, в который user32 больше не пытается загрузить DLL, иначе выпадал бы из списка до
следующего события в нём.

Контракт `GlassCompat/docs/GlassCompat-unload.md` не тронут: фазы, таймауты и семантика
`path=1/2/3` те же.

### 7.6. Прогоны 18 (установка поверх) и 19 (удаление)

| Прогон | Остановка хостов | Итог |
|---|---|---|
| 18, установка поверх | x64 `hostGone=1 ms=8531`, x86 `hostGone=1 ms=2203`, `task_stop=0`, `residuals=0` | оба хоста ушли сами, задача не останавливалась; полный цикл установки 37 с |
| 19, удаление | x64 `hostGone=0 ms=15015` → `IRegisteredTask::Stop`; x86 `hostGone=1 ms=3438` | x64-хост не уложился в окно 15 с |

Почему в прогоне 19 x64-хост не ушёл за 15 с: после остановки в explorer остался
`GlassCompat`, у которого ссылку держит ОС (INCONTEXT-ссылка от `SetWinEventHook`), снять её наша
выгрузка не может (политика R6/R7), и `ForceUnloadResiduals` ждал этого остатка до конца окна.

Ошибкой это не является (R12h-1 корректно продолжил установку), но резерв ускорения есть:
не ждать кандидата, чей refcount уже не уменьшается. Правка не внесена — меняет ожидание в
`ForceUnloadResiduals`, нужен отдельный прогон.
